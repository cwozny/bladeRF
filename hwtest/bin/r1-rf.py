#!/usr/bin/env python3
"""r1-rf.py <hwtest-stream-r1> <out dir> [bw]: RF check of 1R1T mode on the xA5 RX1.

The B200mini sends a CW tone at FC + offset (its carrier F_B below that, a +F_B tone
at baseband) into bladeRF RX1 through the splitter and 20 dB. For each offset,
hwtest-stream-r1 rxcap captures RX1 at FC in each configuration of CONFIGS (a new
bladeRF session each time, host tuning mode, the no-TX guard preloaded by the
caller). Per capture: tone level (dBFS), frequency error, the bladeRF's I/Q image
of the tone, the worst other spur and the median noise bin, both relative to the
tone. The B200mini's own carrier leak and image (F_B and 2 F_B below the tone)
are left out of the spur search."""
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rf", os.path.join(HERE, "rf-check.py"))
rf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rf)

FC = 2450e6
F_B, FS_B = 2e6, 8e6
GAIN = 15
N = 1 << 20
OFFSETS = [-55e6, -40e6, -25e6, -10e6, 3e6, 10e6, 25e6, 40e6, 55e6]
# name, rate, format, extra hwtest-stream options
CONFIGS = [
    ("1r1t-122.88", 122.88e6, "sc8", ["-O", "-1"]),
    ("2r2t-122.88-d70", 122.88e6, "sc8", ["-O", "-R", "0x006=0x70"]),
    ("2r2t-122.88", 122.88e6, "sc8", ["-O"]),
    ("1r1t-61.44", 61.44e6, "sc16", ["-1"]),
    ("2r2t-61.44", 61.44e6, "sc16", []),
]
# the same with the RX analog bandwidth set to 56 MHz after the rate (libbladeRF keeps its
# 18 MHz default otherwise; with OVERSAMPLE it warns that the result is unknown)
CONFIGS_BW = [
    ("1r1t-122.88-bw56", 122.88e6, "sc8", ["-O", "-1", "-w", "56M"]),
    ("1r1t-122.88", 122.88e6, "sc8", ["-O", "-1"]),
    ("2r2t-61.44-bw56", 61.44e6, "sc16", ["-w", "56M"]),
]


def analyze(path, fmt, fs, f_exp):
    if fmt == "sc8":
        x = np.fromfile(path, dtype=np.int8).astype(np.float64) / 128.0
    else:
        x = np.fromfile(path, dtype=np.int16).astype(np.float64) / 2048.0
    z = x[0::2] + 1j * x[1::2]
    n = len(z)
    w = np.blackman(n)
    s = np.abs(np.fft.fftshift(np.fft.fft(z * w))) ** 2 / np.sum(w) ** 2
    f = np.fft.fftshift(np.fft.fftfreq(n, 1 / fs))

    def peak(fc, half):
        idx = np.where(np.abs(f - fc) <= half)[0]
        if len(idx) == 0:
            return None, None
        k = idx[int(np.argmax(s[idx]))]
        return k, 10 * np.log10(np.sum(s[max(k - 4, 0):k + 5]) + 1e-30)

    k, tone = peak(f_exp, 50e3)
    ft = f[k]
    _, image = peak(-ft, 20e3)
    mask = np.ones(n, dtype=bool)
    for c in (ft, -ft, 0.0, ft - F_B, ft - 2 * F_B):
        mask &= np.abs(f - c) > 200e3
    j = np.where(mask)[0]
    ks = j[int(np.argmax(s[j]))]
    spur = 10 * np.log10(np.sum(s[max(ks - 4, 0):ks + 5]) + 1e-30)
    floor = 10 * np.log10(np.median(s) + 1e-30)
    clip = float(np.mean(np.abs(x) >= 0.999))
    return {"tone_dbfs": round(tone, 2), "ferr_hz": round(ft - f_exp, 1),
            "image_dbc": round(image - tone, 1), "spur_dbc": round(spur - tone, 1),
            "spur_mhz": round(f[ks] / 1e6, 3), "floor_dbc_bin": round(floor - tone, 1),
            "clipped": clip, "samples": n}


def main():
    hs, out = sys.argv[1:3]
    configs = CONFIGS_BW if len(sys.argv) > 3 and sys.argv[3] == "bw" else CONFIGS
    os.makedirs(out, exist_ok=True)
    u = rf.usrp()
    proto = rf.tone(int(FS_B * 0.01), FS_B, F_B, 0.5).astype(np.complex64)
    results = open(os.path.join(out, "rf.jsonl"), "a")
    for off in OFFSETS:
        stop, txr = threading.Event(), {}

        def tx():
            try:
                rf.b200_transmit(u, proto, FC + off - F_B, FS_B, rf.B200_TX_GAIN, stop, txr)
            except Exception as e:
                txr["error"] = str(e)
        t = threading.Thread(target=tx)
        t.start()
        try:
            time.sleep(1.5)
            for name, rate, fmt, extra in configs:
                if abs(off) > 0.45 * rate:
                    continue
                path = os.path.join(out, "cap-%s-%+d.bin" % (name, int(off / 1e6)))
                cmd = [hs, "rxcap", "-f", fmt, "-c", "1", "-r", str(int(rate)), "-n", str(N),
                       "-F", str(int(FC)), "-g", str(GAIN), "-o", path] + extra
                p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
                rec = {"config": name, "offset_mhz": off / 1e6, "rc": p.returncode}
                if p.returncode == 0 and os.path.exists(path):
                    rec.update(analyze(path, fmt, rate, off))
                    os.remove(path)
                else:
                    rec["log"] = (p.stdout + p.stderr)[-600:]
                rec["b200_underflows"] = len(txr.get("underflows", []))
                results.write(json.dumps(rec) + "\n")
                results.flush()
                print("RF %-16s %+6.1f MHz: %s" % (name, off / 1e6, json.dumps(
                    {k: rec.get(k) for k in ("rc", "tone_dbfs", "ferr_hz", "image_dbc",
                                              "spur_dbc", "spur_mhz", "floor_dbc_bin")})), flush=True)
        finally:
            stop.set()
            t.join()
        if "error" in txr:
            print("B200mini transmit failed at %+.1f MHz: %s" % (off / 1e6, txr["error"]), flush=True)
    results.close()


if __name__ == "__main__":
    main()
