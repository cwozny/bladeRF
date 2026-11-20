#!/usr/bin/env python3
"""ocrx-locate.py (2026-11-20): where the PRI errors of pulse-check's oc-rx
cases fall in the capture.

The B200mini transmits pulse-check's pulse trains (as pulse-check.py oc-rx);
the bladeRF captures CAP_S at 122.88 Msps with hwtest-stream rxcap (SC8,
32,768-sample buffers; rxcap drops 16 buffers before it keeps any). For
every PRI off by more than pulse-check's limit it prints the time in the
capture, the stream buffer it falls in, and the whole numbers of buffers
(266.667 us each; + = skipped, - = repeated or late) that explain the error
modulo the PRI. It also judges the first 0.25 s (what pulse-check judges)
and 0.25 s from 0.1 s on. The bladeRF only receives; the B200mini transmits
with pulse-check's settings, so it needs pulse-check's cabling and
HWTEST_TX=1 (run it the way run-suite.sh runs pulse-check.py: common.sh
sourced, HWTEST_RF_CH and BLADERF_DEFAULT_TUNING_MODE set).

Used on the xA9 on 2026-11-20 (TESTING.md section 18), when rxcap dropped
16 buffers before it kept any. Since 40626924 rxcap warms up for 0.25 s by
default; this tool asks for -W 0 (OCRX_WARMUP, default 0), which drops
those 16 buffers again, so its buffer numbers count from the stream's start.

  ocrx-locate.py REPS OUT.jsonl
"""
import importlib.util
import json
import os
import sys
import tempfile
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("pulsecheck", os.path.join(HERE, "pulse-check.py"))
pc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pc)
rf = pc.rf

RATE = pc.OC_RATE
BUF = 32768             # rxcap's SC8 buffer: 65,536 bytes
SETTLE = 16             # buffers rxcap drops with -W 0 before it keeps any
WARMUP = os.environ.get("OCRX_WARMUP", "0")  # rxcap -W, seconds (0: the 16 buffers only)
CAP_S = 0.35
T_BUF = BUF / RATE


def rise_times(x, fs):
    """Rising 50 % crossings in samples, found as measure_pulses finds them
    (mixed down by F_OFF, 0.25 us average, hysteresis); not the first or the
    last pulse."""
    w = max(1, int(round(0.25e-6 * fs)))
    bb = x * np.exp(-2j * np.pi * pc.F_OFF * np.arange(len(x)) / fs).astype(np.complex64)
    k = np.ones(w, dtype=np.float32) / w
    env = np.abs(np.convolve(bb.real, k, mode="same") +
                 1j * np.convolve(bb.imag, k, mode="same")).astype(np.float32)
    del bb
    top = float(np.percentile(env, 99.9))
    a_on = float(np.median(env[env > 0.5 * top]))
    s = np.full(len(env), -1, dtype=np.int8)
    s[env > 0.6 * a_on] = 1
    s[env < 0.4 * a_on] = 0
    idx = np.where(s >= 0, np.arange(len(s)), 0)
    np.maximum.accumulate(idx, out=idx)
    s = s[idx]
    del idx
    s[s < 0] = 0
    r = np.nonzero(np.diff(s.astype(np.int16)) == 1)[0] + 1
    thr = 0.5 * a_on
    win = max(8, int(fs * 200e-9))
    out = [pc.crossing(env, max(int(k0) - win, 0), int(k0), thr, True) for k0 in r]
    out = [t for t in out if t is not None]
    return np.array(out[1:-1])


def explain(dev, pri):
    """Buffer counts k (|k| <= 40) with dev + k * T_BUF a whole number of PRIs (within 30 ns)."""
    ks = []
    for k in range(-40, 41):
        if k == 0:
            continue
        r = (dev + k * T_BUF) % pri
        if min(r, pri - r) < 30e-9:
            ks.append(k)
    return sorted(ks, key=abs)[:4]


def one(u, pw, pri, rep):
    proto = pc.pulse_train(pc.B200_FS, pw, pri, 0.5)
    stop = threading.Event()
    txr = {}
    t = threading.Thread(target=rf.b200_transmit,
                         args=(u, proto, pc.FC, pc.B200_FS, pc.OC_B200_TX_GAIN, stop, txr))
    t.start()
    time.sleep(1.5)
    try:
        with tempfile.TemporaryDirectory(dir="/dev/shm") as d:
            path = os.path.join(d, "rx.bin")
            out = pc.run_stream(["rxcap", "-f", "sc8", "-c", str(pc.BLADERF_CH), "-O",
                                 "-r", "%d" % RATE, "-F", "%d" % pc.FC, "-g", str(pc.OC_RX_GAIN),
                                 "-n", "%d" % int(RATE * CAP_S), "-o", path, "-W", WARMUP], 90)
            x = pc.load_iq(path, True).astype(np.complex64)
    finally:
        stop.set()
        t.join()
    if "underflows" not in txr:
        raise RuntimeError("B200mini transmit thread failed")
    rec = {"pw_us": pw * 1e6, "pri_us": pri * 1e6, "rep": rep,
           "b200_late_underflows": len([s for s in txr["underflows"] if s > 1.0]),
           "rxcap": [l for l in out.splitlines() if l.startswith("rxcap:")]}
    r = rise_times(x, RATE)
    d = np.diff(r) / RATE
    tol = max(pc.PRI_ONE_REL * pri, pc.PRI_ONE_ABS) + pc.PRI_PPM * pri
    rec["pulses"] = int(len(r))
    rec["anomalies"] = [{"t_ms": float(r[i] / RATE * 1e3), "buffer": SETTLE + int(r[i] // BUF),
                         "pri_us": float(d[i] * 1e6), "k": explain(d[i] - pri, pri)}
                        for i in np.nonzero(np.abs(d - pri) > tol)[0]]
    for name, a, b in (("first_0.25s", 0.0, 0.25), ("from_0.1s", 0.1, 0.35)):
        seg = x[int(a * RATE):int(b * RATE)]
        try:
            m = pc.judge(pc.measure_pulses(seg, RATE, 0.25e-6), pw, pri, len(seg) / RATE, pc.FC)
            rec[name] = "PASS" if m["pass"] else "FAIL " + "; ".join(m["errors"])
        except Exception as e:
            rec[name] = "ERROR %s" % e
    return rec


def main():
    reps, outp = int(sys.argv[1]), sys.argv[2]
    if os.environ.get("HWTEST_TX") != "1":
        sys.exit("needs HWTEST_TX=1 (pulse-check's cabling; the B200mini transmits)")
    rf.check_channel()
    u = rf.usrp()
    fails = 0
    with open(outp, "a") as f:
        for rep in range(reps):
            for pw, pri in pc.OC_RX_CASES:
                try:
                    rec = one(u, pw, pri, rep)
                except Exception as e:
                    rec = {"pw_us": pw * 1e6, "pri_us": pri * 1e6, "rep": rep, "error": str(e)}
                    fails += 1
                rec["time"] = time.time()
                f.write(json.dumps(rec) + "\n")
                f.flush()
                an = rec.get("anomalies", [])
                print("PW %g us PRI %g us rep %d: %s; %d PRIs off%s; B200 late underflows %s; "
                      "first 0.25 s: %s; from 0.1 s: %s" % (
                          pw * 1e6, pri * 1e6, rep, rec.get("error", "captured"), len(an),
                          "".join(" [%.2f ms, buffer %d, PRI %.3f us, k %s]"
                                  % (a["t_ms"], a["buffer"], a["pri_us"], a["k"]) for a in an[:8]),
                          rec.get("b200_late_underflows"), rec.get("first_0.25s"),
                          rec.get("from_0.1s")), flush=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
