#!/usr/bin/env python3
"""r1-txrf.py <hwtest-stream> <out dir> [1|2] [iso]: RF check of TX in 1R1T mode (branch 1r1t).

Cabling: the "tx" setup of twoch-check.py (bladeRF TX1 and TX2 -> splitter as a
combiner -> 20 dB -> B200mini "RX2"), or one TX port -> 20 dB -> B200mini "RX2" with
the other on a 50 ohm load; no antennas. Run with HWTEST_TX=1 (the TX cap preloaded by
the caller): the bladeRF transmits; the B200mini only receives. With one port cabled,
"iso" runs only the first configuration on the other port: its tone level against the
cabled port's is the isolation between the ports. Through the splitter, the 2R2T runs'
carrier leak includes the idle channel's (both TX channels are on in 2R2T mode).

For the TX channel (1 or 2, default 1) and each configuration of CONFIGS (a new
bladeRF session each time), hwtest-stream tx sends a CW tone at FC + F_TONE for
TX_S seconds. After TX_START_S the B200mini takes two captures:
  - wide: FS_W around FC: the tone's level (dBFS) and frequency error, the
    bladeRF's I/Q image at FC - F_TONE, its carrier leak at FC, and the worst
    other spur, all relative to the tone;
  - long: LONG_S seconds at FS_L, tuned F_L below the tone: the tone's level in
    blocks of BLOCK samples; a block more than DROP_DB below the median is a
    dropout (a gap or a burst in the transmitted samples).
Also kept: hwtest-stream's rate ("libbladeRF took ... Msps"), its interface mode
line, and its exit status."""
import importlib.util
import json
import os
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rf", os.path.join(HERE, "rf-check.py"))
rf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rf)

FC = 2450e6
F_TONE = 3e6
GAIN = 30                      # bladeRF TX gain, dB (lib/tx-cap.c caps it)
B200_GAIN = rf.B200_RX_GAIN
TX_S = 14
TX_START_S = 4.0
FS_W, N_W = 16e6, 1 << 20
FS_L, F_L, LONG_S = 2e6, 0.5e6, 4.0
BLOCK = 2000                   # 1 ms at FS_L
DROP_DB = 6.0
# name, rate, format, extra hwtest-stream options
CONFIGS = [
    ("auto-122.88", 122.88e6, "sc8", ["-O"]),
    ("2r2t-122.88", 122.88e6, "sc8", ["-O", "-2"]),
    ("2r2t-61.44", 61.44e6, "sc16", []),
]


def wide(x, fs):
    n = len(x)
    w = np.blackman(n)
    s = np.abs(np.fft.fftshift(np.fft.fft(x * w))) ** 2 / np.sum(w) ** 2
    f = np.fft.fftshift(np.fft.fftfreq(n, 1 / fs))

    def peak(fc, half):
        idx = np.where(np.abs(f - fc) <= half)[0]
        k = idx[int(np.argmax(s[idx]))]
        return k, 10 * np.log10(np.sum(s[max(k - 4, 0):k + 5]) + 1e-30)

    k, tone = peak(F_TONE, 50e3)
    ft = f[k]
    _, image = peak(-ft, 20e3)
    _, leak = peak(0.0, 20e3)
    mask = np.ones(n, dtype=bool)
    for c in (ft, -ft, 0.0):
        mask &= np.abs(f - c) > 200e3
    j = np.where(mask)[0]
    ks = j[int(np.argmax(s[j]))]
    spur = 10 * np.log10(np.sum(s[max(ks - 4, 0):ks + 5]) + 1e-30)
    return {"tone_dbfs": round(tone, 2), "ferr_hz": round(ft - F_TONE, 1),
            "image_dbc": round(image - tone, 1), "leak_dbc": round(leak - tone, 1),
            "spur_dbc": round(spur - tone, 1), "spur_mhz": round(f[ks] / 1e6, 3)}


def blocks(x, fs):
    """Tone level per BLOCK samples (the tone is F_L above the capture's center)."""
    n = len(x) // BLOCK * BLOCK
    t = np.arange(n) / fs
    y = (x[:n] * np.exp(-2j * np.pi * F_L * t)).reshape(-1, BLOCK).mean(axis=1)
    lvl = 20 * np.log10(np.abs(y) + 1e-12)
    med = float(np.median(lvl))
    drops = np.where(lvl < med - DROP_DB)[0]
    return {"blocks": int(len(lvl)), "dropouts": int(len(drops)),
            "first_dropout_ms": int(drops[0]) if len(drops) else None,
            "block_spread_db": round(float(np.percentile(lvl, 99.9) - np.percentile(lvl, 0.1)), 2)}


def self_test():
    fs = FS_L
    n = int(fs * LONG_S)
    t = np.arange(n) / fs
    x = 0.1 * np.exp(2j * np.pi * F_L * t)
    assert blocks(x, fs)["dropouts"] == 0
    x[400000:400100] = 0          # a 50 us gap
    r = blocks(x, fs)
    assert r["dropouts"] == 0, r  # inside one 1 ms block: -0.4 dB only
    x[400000:402500] = 0          # a 1.25 ms gap
    r = blocks(x, fs)
    assert r["dropouts"] >= 1, r
    tw = np.arange(N_W) / FS_W
    z = 0.1 * np.exp(2j * np.pi * F_TONE * tw) + 0.001 * np.exp(-2j * np.pi * F_TONE * tw)
    r = wide(z, FS_W)
    assert abs(r["image_dbc"] + 40) < 1, r
    print("self-test: pass")


def main():
    if sys.argv[1:2] == ["--self-test"]:
        self_test()
        return
    hs, out = sys.argv[1:3]
    ch = sys.argv[3] if len(sys.argv) > 3 else "1"
    iso = sys.argv[4:5] == ["iso"]
    if ch not in ("1", "2"):
        sys.exit("channel must be 1 or 2")
    if os.environ.get("HWTEST_TX") != "1":
        sys.exit("r1-txrf.py transmits: run it with HWTEST_TX=1 and the TX cap")
    os.makedirs(out, exist_ok=True)
    u = rf.usrp()
    results = open(os.path.join(out, "txrf.jsonl"), "a")
    for name, rate, fmt, extra in (CONFIGS[:1] if iso else CONFIGS):
        cmd = [hs, "tx", "-f", fmt, "-c", ch, "-r", str(int(rate)), "-F", str(int(FC)),
               "-g", str(GAIN), "-T", str(int(F_TONE)), "-t", str(TX_S)] + extra
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        rec = {"config": name, "channel": int(ch), "cabled": not iso}
        try:
            time.sleep(TX_START_S)
            if p.poll() is None:
                rec.update(wide(rf.b200_capture(u, N_W, FC, FS_W, B200_GAIN), FS_W))
            if p.poll() is None:
                rec.update(blocks(rf.b200_capture(u, int(FS_L * LONG_S), FC + F_TONE - F_L,
                                                  FS_L, B200_GAIN), FS_L))
            if p.poll() is not None:
                rec["ended_early"] = True
        except Exception as e:
            rec["error"] = str(e)
        finally:
            log, _ = p.communicate(timeout=TX_S + 60)
        rec["rc"] = p.returncode
        for line in log.splitlines():
            if "libbladeRF took" in line or "interface in" in line:
                rec.setdefault("hwtest_stream", []).append(line.strip())
        if p.returncode != 0:
            rec["log"] = log[-600:]
        results.write(json.dumps(rec) + "\n")
        results.flush()
        print("TXRF TX%s %-12s %s%s" % (ch, name, "(not cabled) " if iso else "", json.dumps(
            {k: rec.get(k) for k in ("rc", "tone_dbfs", "ferr_hz", "image_dbc", "leak_dbc",
                                      "spur_dbc", "spur_mhz", "dropouts", "block_spread_db",
                                      "hwtest_stream", "error")})), flush=True)
        time.sleep(1.0)


if __name__ == "__main__":
    main()
