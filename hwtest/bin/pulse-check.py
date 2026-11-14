#!/usr/bin/env python3
"""Overclock and TX sample-format checks of the bladeRF against the B200mini.

  pulse-check.py oc-rx  [--oc-rate RATE] [--out results.jsonl]
      B200mini TX pulses -> bladeRF RX2, overclocked: 122.88 Msps (or RATE),
      8-bit
  pulse-check.py oc-tx  [--oc-rate RATE] [--oc-format sc8|sc8meta] [--out ...]
      bladeRF TX2 pulses, overclocked: 122.88 Msps (or RATE), 8-bit (sc8meta:
      with metadata, one burst started "now") -> B200mini RX
  pulse-check.py tx-fmt --format FMT --layout x1|x2 --rate RATE [--out ...]
      bladeRF transmits a tone in sample format FMT at RATE (on TX2, and on
      TX1 into its load for x2) -> B200mini RX: the tone must be there,
      at the right frequency, without dropouts

Cabling and power limits as for rf-check.py (whose B200mini helpers this
uses): bladeRF TX2 -> 20 dB -> B200mini RX2, B200mini TX/RX -> 20 dB ->
bladeRF RX2, bladeRF TX gain capped by lib/tx-cap.c, B200mini TX gain by
rf-check.B200_TX_GAIN_MAX. Transmits: only with HWTEST_TX=1.

Pulse cases: the transmitter sends a carrier at F_OFF from the tuned
frequency, switched on for the pulse width (PW) once per pulse repetition
interval (PRI). The receiver finds every pulse (50 % amplitude crossings,
interpolated between samples, after mixing the carrier down and averaging
over 0.2-0.25 us) and measures each PW and PRI, the carrier frequency and
level, and the on/off ratio. A pulse case passes if the mean PRI is within
50 ppm of the transmitted one, every single PRI within 2 % or 250 ns (any
lost or repeated buffer of samples is far more), the mean PW within 2 % or
100 ns and every PW within 10 % or 250 ns, the pulse count matches the
capture length, the carrier is within 10 ppm and the pulses are >= 15 dB
above the gaps. The overclocked receive also records how the 8-bit samples
look with nothing transmitted (spread, share at full scale). The overclocked receive also proves the sample rate: at
any other rate the PW and PRI in samples would be off.

The bladeRF side runs lib/hwtest-stream.c (rxcap/tx), which buffers in
memory; samples are raw interleaved I/Q (SC8: int8, SC16: int16).
"""
import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("rfcheck", os.path.join(HERE, "rf-check.py"))
rf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rf)

STREAM = os.environ.get("HWTEST_STREAM") or os.path.join(
    os.environ.get("HOST_BUILD", os.path.expanduser("~/workarea/hwtest-data/host-build")),
    "output", "hwtest-stream")

FC = 915e6              # carrier
F_OFF = 3e6             # tone offset from the carrier
OC_RATE = 122.88e6      # overclocked bladeRF sample rate (default; --oc-rate)
B200_FS = 20e6          # B200mini sample rate for the pulse cases
CAPTURE_S = 0.25        # capture length
BLADERF_CH = 2          # RX2/TX2 face the B200mini
OC_RX_GAIN = 10         # dB (manual)
OC_B200_TX_GAIN = 60    # dB (cap 65): strong pulses for 8-bit samples
TX_GAIN = 30            # dB overall (capped at 60 by lib/tx-cap.c)
TRIES = 3
SAVE_DIR = None         # --save: keep the first 50 ms of every capture (complex64 .npy)


def save_capture(x, fs, what):
    if SAVE_DIR:
        os.makedirs(SAVE_DIR, exist_ok=True)
        np.save(os.path.join(SAVE_DIR, "%s-%s.npy" % (what, time.strftime("%H%M%S"))),
                x[:int(fs * 0.05)].astype(np.complex64))

# (PW, PRI) in seconds, exact in samples at each transmitter's rate
OC_RX_CASES = [(1e-6, 10e-6), (10e-6, 100e-6), (100e-6, 1e-3)]       # B200mini at 20 Msps


def oc_tx_cases(rate):
    """The same widths and intervals in whole samples at the bladeRF's rate
    (at 122.88 Msps: 123/1229, 1229/12288 and 12288/122880 samples)."""
    return [(round(pw * rate) / rate, round(pri * rate) / rate) for pw, pri in OC_RX_CASES]


PRI_PPM = 50e-6                       # mean PRI
PRI_ONE_REL, PRI_ONE_ABS = 0.02, 250e-9   # each PRI (a lost buffer is >100 us)
PW_MEAN_REL, PW_MEAN_ABS = 0.02, 100e-9
PW_ONE_REL, PW_ONE_ABS = 0.10, 250e-9
ON_OFF_DB = 15
CARRIER_MIN_PW = 3e-6                 # carrier measured on pulses this long or more


def pulse_train(fs, pw, pri, amp):
    """One PRI of samples: carrier at F_OFF for PW, then zeros."""
    npw, npri = pw * fs, pri * fs
    if abs(npw - round(npw)) > 1e-6 or abs(npri - round(npri)) > 1e-6:
        raise ValueError("PW/PRI are not whole samples at %.0f sps" % fs)
    npw, npri = int(round(npw)), int(round(npri))
    x = np.zeros(npri, dtype=np.complex64)
    x[:npw] = amp * np.exp(2j * np.pi * F_OFF * np.arange(npw) / fs)
    return x


def load_iq(path, eight_bit):
    if eight_bit:
        d = np.fromfile(path, dtype=np.int8).astype(np.float32) / 128.0
    else:
        d = np.fromfile(path, dtype=np.int16).astype(np.float32) / 2048.0
    return d[0::2] + 1j * d[1::2]


def crossing(env, k0, k1, thr, rising):
    """Interpolated sample time where env crosses thr between k0 and k1."""
    seg = env[k0:k1 + 1]
    idx = np.nonzero(seg < thr if rising else seg > thr)[0]
    if len(idx) == 0:
        return None
    k = k0 + int(idx[-1])
    if k + 1 >= len(env) or env[k + 1] == env[k]:
        return float(k)
    return k + (thr - env[k]) / (env[k + 1] - env[k])


def measure_pulses(x, fs, smooth_s):
    """Every pulse's PW and PRI, plus carrier frequency, level and on/off.
    The capture is mixed down by F_OFF and averaged over smooth_s (a
    symmetric filter: it slows the edges equally, so the 50 % crossings,
    PW and PRI stay), which keeps the noise out of the envelope."""
    w = max(1, int(round(smooth_s * fs)))
    bb0 = (x * np.exp(-2j * np.pi * F_OFF * np.arange(len(x)) / fs)).astype(np.complex64)
    bb = bb0
    if w > 1:
        k = np.ones(w, dtype=np.float32) / w
        bb = (np.convolve(bb.real, k, mode="same") + 1j * np.convolve(bb.imag, k, mode="same")).astype(np.complex64)
    env = np.abs(bb).astype(np.float32)
    top = float(np.percentile(env, 99.9))
    on = env > 0.5 * top
    if not on.any() or on.all():
        raise RuntimeError("no pulses found (peak %.1f dBFS)" % (20 * np.log10(top + 1e-12)))
    a_on = float(np.median(env[on]))
    a_off = float(np.median(env[~on]))
    thr = 0.5 * a_on
    # hysteresis states: 1 above 60 %, 0 below 40 %, else keep the last
    s = np.full(len(env), -1, dtype=np.int8)
    s[env > 0.6 * a_on] = 1
    s[env < 0.4 * a_on] = 0
    idx = np.where(s >= 0, np.arange(len(s)), 0)
    np.maximum.accumulate(idx, out=idx)
    s = s[idx]
    s[s < 0] = 0                # before the first decision: off
    d = np.diff(s.astype(np.int16))
    rise_k = np.nonzero(d == 1)[0] + 1
    fall_k = np.nonzero(d == -1)[0] + 1
    # complete pulses only: a rise, then its fall
    if len(rise_k) and len(fall_k) and fall_k[0] < rise_k[0]:
        fall_k = fall_k[1:]
    m = min(len(rise_k), len(fall_k))
    rise_k, fall_k = rise_k[:m], fall_k[:m]
    win = max(8, int(fs * 200e-9))
    rises, falls = [], []
    for r, f in zip(rise_k, fall_k):
        tr = crossing(env, max(r - win, 0), r, thr, True)
        tf = crossing(env, max(f - win, 0), f, thr, False)
        if tr is not None and tf is not None:
            rises.append(tr)
            falls.append(tf)
    rises, falls = np.array(rises), np.array(falls)
    # not the first or the last pulse either: the capture's start or end can
    # cut one, and a receiver's first samples can be a short gap that passes
    # for a rise (one 10 us pulse measured 0.22 us that way, 9.8 us late)
    edge = 2 if len(rises) >= 5 else 0
    if edge:
        rises, falls = rises[1:-1], falls[1:-1]
    if len(rises) < 3:
        raise RuntimeError("only %d pulses found" % len(rises))
    pw = (falls - rises) / fs
    pri = np.diff(rises) / fs
    # carrier: the mean phase step over every pulse of the mixed-down signal
    # after a 1 us moving average (lag-1 autocorrelation), unambiguous within
    # +-fs/2. The moving average has nulls at multiples of 1 MHz, so the
    # transmitter's LO leakage and IQ image (3 and 6 MHz from the tone after
    # mixing down) drop out; they are continuous and strong in overclocked
    # transmit, and pulled a shorter average's estimate off by 100-400 kHz.
    # (The strongest spectral line is not reliable either: a pulse train's
    # lines are a PRF apart and neighbouring lines differ by only
    # sinc(PW/PRI), 1.6 % at 10 % duty.)
    # Only on pulses of CARRIER_MIN_PW or more: in a shorter pulse every 1 us
    # window straddles an edge, the gated IQ image no longer cancels, and 1 us
    # pulses read 9.5 kHz high at 122.88 Msps on every image (and 69 kHz low,
    # or not at all, when received at 68 Msps); their PW and PRI still count.
    freq = None
    if np.median(pw) >= CARRIER_MIN_PW:
        n1 = max(1, int(round(fs * 1e-6)))
        k1 = np.ones(n1, dtype=np.float32) / n1
        s1 = (np.convolve(bb0.real, k1, mode="same") + 1j * np.convolve(bb0.imag, k1, mode="same")).astype(np.complex128)
        span = np.zeros(len(x), dtype=bool)
        for r, f in zip(rises, falls):
            span[int(np.ceil(r)):int(np.floor(f)) + 1] = True
        cm = span[1:] & span[:-1]
        if cm.any():
            rr = np.sum(s1[1:][cm] * np.conj(s1[:-1][cm]))
            freq = F_OFF + float(np.angle(rr)) * fs / (2 * np.pi)
    return {
        "pulses": int(len(rises)),
        "edge_pulses_dropped": edge,
        "pw": pw, "pri": pri,
        "freq_hz": freq,
        "on_dbfs": float(20 * np.log10(a_on + 1e-12)),
        "on_off_db": float(20 * np.log10((a_on + 1e-12) / (a_off + 1e-12))),
    }


def judge(m, pw_nom, pri_nom, dur, fc):
    """Pass/fail of one pulse case; adds the summary numbers to m."""
    pw, pri = m.pop("pw"), m.pop("pri")
    exp_pulses = dur / pri_nom
    res = {
        "pw_nom_us": pw_nom * 1e6, "pri_nom_us": pri_nom * 1e6,
        "pw_mean_us": float(pw.mean() * 1e6), "pw_min_us": float(pw.min() * 1e6),
        "pw_max_us": float(pw.max() * 1e6), "pw_std_ns": float(pw.std() * 1e9),
        "pri_mean_us": float(pri.mean() * 1e6), "pri_min_us": float(pri.min() * 1e6),
        "pri_max_us": float(pri.max() * 1e6), "pri_std_ns": float(pri.std() * 1e9),
        "pri_err_ppm": float((pri.mean() - pri_nom) / pri_nom * 1e6),
        "expected_pulses": round(exp_pulses, 1),
        "freq_err_hz": None if m["freq_hz"] is None else m["freq_hz"] - F_OFF,
    }
    m.update(res)
    errors = []
    if abs(pri.mean() - pri_nom) > PRI_PPM * pri_nom:
        errors.append("mean PRI %.4f us, sent %.4f us" % (pri.mean() * 1e6, pri_nom * 1e6))
    tol = max(PRI_ONE_REL * pri_nom, PRI_ONE_ABS) + PRI_PPM * pri_nom
    bad = np.nonzero(np.abs(pri - pri_nom) > tol)[0]
    if len(bad):
        errors.append("%d PRIs off by more than %.0f ns (worst %.3f us, sent %.4f us)"
                      % (len(bad), tol * 1e9, pri[bad[np.argmax(np.abs(pri[bad] - pri_nom))]] * 1e6,
                         pri_nom * 1e6))
    if abs(pw.mean() - pw_nom) > max(PW_MEAN_REL * pw_nom, PW_MEAN_ABS):
        errors.append("mean PW %.4f us, sent %.4f us" % (pw.mean() * 1e6, pw_nom * 1e6))
    badw = np.nonzero(np.abs(pw - pw_nom) > max(PW_ONE_REL * pw_nom, PW_ONE_ABS))[0]
    if len(badw):
        errors.append("%d PWs off by more than %.0f ns" % (len(badw), max(PW_ONE_REL * pw_nom, PW_ONE_ABS) * 1e9))
    found = m["pulses"] + m.get("edge_pulses_dropped", 0)
    if abs(found - exp_pulses) > 2:
        errors.append("%d pulses in %.3f s, expected %.1f" % (found, dur, exp_pulses))
    if m["freq_err_hz"] is not None and abs(m["freq_err_hz"]) > 10e-6 * fc + 100:
        errors.append("carrier %.0f Hz from the expected" % m["freq_err_hz"])
    if m["on_off_db"] < ON_OFF_DB:
        errors.append("pulses only %.1f dB above the gaps" % m["on_off_db"])
    m["pass"] = not errors
    if errors:
        m["errors"] = errors
    return m


def run_stream(args, timeout):
    p = subprocess.run([STREAM] + args, capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError("hwtest-stream %s failed (%d): %s %s"
                           % (args[0], p.returncode, p.stdout[-1500:], p.stderr[-1500:]))
    return p.stdout


def quiet_stats(rate):
    """Overclocked capture with nothing transmitted: the 8-bit samples'
    spread and how many sit at full scale (a sign of corrupted samples)."""
    with tempfile.TemporaryDirectory(dir="/dev/shm") as d:
        path = os.path.join(d, "quiet.bin")
        run_stream(["rxcap", "-f", "sc8", "-c", str(BLADERF_CH), "-O",
                    "-r", "%d" % rate, "-F", "%d" % FC, "-g", str(OC_RX_GAIN),
                    "-n", "%d" % int(rate * 0.02), "-o", path], 60)
        r = np.fromfile(path, dtype=np.int8).astype(np.float32)
    i, q = r[0::2], r[1::2]
    return {"quiet_i_std_lsb": float(i.std()), "quiet_q_std_lsb": float(q.std()),
            "quiet_full_scale_pct": float(100 * np.mean((np.abs(r) >= 127)))}


def oc_rx_case(u, pw, pri, rate=OC_RATE):
    """B200mini transmits pulses; the bladeRF captures them overclocked."""
    proto = pulse_train(B200_FS, pw, pri, 0.5)
    quiet = quiet_stats(rate)
    stop = threading.Event()
    txr = {}
    t = threading.Thread(target=rf.b200_transmit,
                         args=(u, proto, FC, B200_FS, OC_B200_TX_GAIN, stop, txr))
    t.start()
    time.sleep(1.5)
    try:
        with tempfile.TemporaryDirectory(dir="/dev/shm") as d:
            path = os.path.join(d, "rx.bin")
            out = run_stream(["rxcap", "-f", "sc8", "-c", str(BLADERF_CH), "-O",
                              "-r", "%d" % rate, "-F", "%d" % FC, "-g", str(OC_RX_GAIN),
                              "-n", "%d" % int(rate * CAPTURE_S), "-o", path], 60)
            x = load_iq(path, True)
            save_capture(x, rate, "ocrx-%gMsps-pw%gus-pri%gus" % (rate / 1e6, pw * 1e6, pri * 1e6))
    finally:
        stop.set()
        t.join()
    late = [s for s in txr.get("underflows", []) if s > 1.0]
    if "underflows" not in txr:
        raise RuntimeError("B200mini transmit thread failed")
    if late:
        raise RuntimeError("B200mini TX underflowed %d times during the capture" % len(late))
    m = measure_pulses(x, rate, 0.25e-6)
    m.update(quiet)
    m.update(case="oc_rx", fs=rate, fc=FC,
             host_rate_line=[l for l in out.splitlines() if l.startswith("rxcap:")][-1:])
    return judge(m, pw, pri, len(x) / rate, FC)


def oc_tx_case(u, pw, pri, rate=OC_RATE, fmt="sc8"):
    """The bladeRF transmits pulses overclocked; the B200mini measures them."""
    proto = pulse_train(rate, pw, pri, 0.5)
    iq = np.empty(2 * len(proto), dtype=np.int8)
    iq[0::2] = np.round(np.real(proto) * 127)
    iq[1::2] = np.round(np.imag(proto) * 127)
    with tempfile.TemporaryDirectory(dir="/dev/shm") as d:
        path = os.path.join(d, "pulses.sc8")
        iq.tofile(path)
        result = {}

        def bladerf_tx():
            try:
                result["out"] = run_stream(["tx", "-f", fmt, "-c", str(BLADERF_CH), "-O",
                                            "-r", "%d" % rate, "-F", "%d" % FC,
                                            "-g", str(TX_GAIN), "-i", path, "-t", "9"], 60)
            except Exception as e:      # re-raised below
                result["error"] = e

        t = threading.Thread(target=bladerf_tx)
        t.start()
        m, t0, last = None, time.time(), None
        try:
            time.sleep(3.0)     # open, overclock setup, start
            while m is None and time.time() - t0 < 8.0 and t.is_alive():
                x = rf.b200_capture(u, int(B200_FS * CAPTURE_S), FC, B200_FS, rf.B200_RX_GAIN)
                save_capture(x, B200_FS, "octx-%s-%gMsps-pw%gus-pri%gus" % (fmt, rate / 1e6, pw * 1e6, pri * 1e6))
                try:
                    m = measure_pulses(x, B200_FS, 0.2e-6)
                except RuntimeError as e:
                    last = e
                    time.sleep(0.5)
        finally:
            t.join()
    if "error" in result:
        raise result["error"]
    if m is None:
        raise RuntimeError("no pulses while the bladeRF transmitted (%s)" % last)
    m.update(case="oc_tx", fs=B200_FS, fc=FC, format=fmt, bladerf_rate=rate)
    return judge(m, pw, pri, len(x) / B200_FS, FC)


def tx_fmt(u, fmt, layout, rate):
    """bladeRF transmits a tone in a sample format; the B200mini checks it."""
    result = {}

    def bladerf_tx():
        try:
            result["out"] = run_stream(["tx", "-f", fmt, "-l", layout, "-c", str(BLADERF_CH),
                                        "-r", "%d" % rate, "-F", "%d" % FC, "-g", str(TX_GAIN),
                                        "-T", "%d" % F_OFF, "-t", "9"], 60)
        except Exception as e:
            result["error"] = e

    t = threading.Thread(target=bladerf_tx)
    t.start()
    m, t0, last = None, time.time(), None
    try:
        time.sleep(3.0)
        while m is None and time.time() - t0 < 8.0 and t.is_alive():
            x = rf.b200_capture(u, int(B200_FS * 0.5), FC, B200_FS, rf.B200_RX_GAIN)
            a = rf.analyze(x, B200_FS, F_OFF)
            a.update(fc=FC)
            try:
                rf.tone_found(a)
                m = a
            except RuntimeError as e:
                last = e
                time.sleep(0.5)
        if m is not None:
            # dropouts: the envelope, averaged over 1 us, must never fall more
            # than 6 dB below its median (an underrun sends zeros for at least
            # one buffer, i.e. hundreds of microseconds)
            n = int(B200_FS * 1e-6)
            xs = x[int(B200_FS * 0.01):]        # skip the receiver settling
            env = np.convolve(np.abs(xs), np.ones(n) / n, mode="valid")
            med = float(np.median(env))
            m["env_min_db"] = float(20 * np.log10(env.min() / med))
            m["dropouts"] = int(np.sum(np.diff((env < med / 2).astype(np.int8)) == 1))
    finally:
        t.join()
    if "error" in result:
        raise result["error"]
    if m is None:
        raise RuntimeError("no tone while the bladeRF transmitted (%s)" % last)
    m.update(case="tx_fmt", format=fmt, layout=layout, rate=rate, fs=B200_FS)
    m["pass"] = m["env_min_db"] > -6
    if not m["pass"]:
        m["errors"] = ["%d dropouts (envelope min %.1f dB)" % (m["dropouts"], m["env_min_db"])]
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["oc-rx", "oc-tx", "tx-fmt"])
    ap.add_argument("--out", default="-")
    ap.add_argument("--format", default="sc16")
    ap.add_argument("--layout", default="x1")
    ap.add_argument("--rate", type=float, default=61.44e6)
    ap.add_argument("--oc-rate", type=float, default=OC_RATE)
    ap.add_argument("--oc-format", choices=["sc8", "sc8meta"], default="sc8")
    ap.add_argument("--save", help="directory for the first 50 ms of each capture (.npy)")
    a = ap.parse_args()
    global SAVE_DIR
    SAVE_DIR = a.save
    if os.environ.get("HWTEST_TX") != "1":
        sys.exit("pulse-check.py transmits: set HWTEST_TX=1 only with the cabling "
                 "in rf-check.py (attenuators, no antennas)")
    u = rf.usrp()
    out = sys.stdout if a.out == "-" else open(a.out, "a")
    if a.mode == "oc-rx":
        jobs = [(oc_rx_case, (pw, pri, a.oc_rate)) for pw, pri in OC_RX_CASES]
    elif a.mode == "oc-tx":
        jobs = [(oc_tx_case, (pw, pri, a.oc_rate, a.oc_format))
                for pw, pri in oc_tx_cases(a.oc_rate)]
    else:
        jobs = [(tx_fmt, (a.format, a.layout, a.rate))]
    fails = 0
    for fn, args in jobs:
        errors = []
        for _ in range(TRIES):      # retry captures that dropped samples
            try:
                m = fn(u, *args)
                break
            except Exception as e:
                errors.append(str(e))
        else:
            m = {"case": a.mode, "args": [float(x) if isinstance(x, float) else x for x in args],
                 "pass": False, "errors": errors[-1:]}
        if errors:
            m["retry_errors"] = errors
        m["time"] = time.time()
        if not m.get("pass"):
            fails += 1
        out.write(json.dumps(m, default=float) + "\n")
        out.flush()
        print("%s %s: %s" % (m.get("case"), args, "PASS" if m.get("pass") else
                             "FAIL " + "; ".join(m.get("errors", []))), file=sys.stderr)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
