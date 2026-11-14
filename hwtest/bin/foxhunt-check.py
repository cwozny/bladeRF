#!/usr/bin/env python3
"""Checks a foxhunt FPGA image of the bladeRF 2.0 micro against the B200mini.

  foxhunt-check.py RBF [--label NAME] [--seconds 75] [--out results.jsonl]
                       [--save capture.npy]

The foxhunt revision is a standalone transmitter. Once loaded, its own Nios
firmware (hdl/fpga/platforms/bladerf-micro/software/foxhunt) sets up the
AD9361 (915 MHz, 768 ksps, 10 dB TX attenuation) and has the FPGA's tone
generator key a tone in Morse code: 1 s of silence, "PARIS PARIS" at 5 WPM
(100 dot lengths of 240 ms), 1 s of silence, then TX off for 10 s, over and
over. The tone is one sideband, 937.5 Hz above the carrier: the firmware asks
for 1 kHz, and the tone generator's phase step (4096 per turn at 768 ksps)
rounds that to 5, i.e. 937.5 Hz.

That firmware serves no host requests, so the image is loaded with
BLADERF_FORCE_NO_FPGA_PRESENT=1 (libbladeRF does not set up the board before
the load; its set-up after the load times out, which is expected) and is
replaced by the official image afterwards (recover.sh).

The B200mini records SECONDS starting just before the load (RX2 port,
915 MHz - 20 kHz, 100 ksps). A capture passes if
  - the tone is 937.5 Hz from the bladeRF's carrier (its LO leakage), within
    2 Hz (the sign, i.e. the sideband, is reported)
  - every complete message decodes to "PARIS PARIS", and there is one
  - the dot length is 240 ms within 1 %
  - consecutive messages start 35.5-37.5 s apart (when two are complete)
  - the tone is >= 20 dB above the gaps.
The tone level, the carrier's offset from 915 MHz (the firmware writes a
fixed VCTCXO trim, 0x1ec1, not the board's calibration), the carrier leakage
with TX on and off, and the time from the load to the first tone are
reported for comparing images.

Transmits (the FPGA does, on its own, until another image is loaded): only
with HWTEST_TX=1 and the transmit cabling. Nuand's firmware transmits on TX1,
which the bench terminates, and turns on the TX bias tee: do not load it with
anything on a TX port that the bias voltage could harm. On this bench both
images under test are built with ../setup/foxhunt-tx2.patch: TX2 (through
20 dB to the B200mini) and no bias tee.
"""
import argparse
import importlib.util
import json
import os
import subprocess
import sys
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("rfcheck", os.path.join(HERE, "rf-check.py"))
rf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rf)

FC = 915e6              # the firmware's carrier
RX_OFFSET = -20e3       # B200mini tuned 20 kHz below it, away from its own DC
FS = 100e3              # B200mini sample rate
B200_RX_GAIN = 6        # dB: about -22 dBm from TX2 (10 dB attenuation, 20 dB pad)
TONE_HZ = 937.5         # above the carrier
TONE_TOL_HZ = 2.0
UNIT_S = 0.240          # dot length at 5 WPM
UNIT_TOL = 0.01
PERIOD_S = (35.5, 37.5)
ON_OFF_DB = 20
TEXT = "PARIS PARIS"
ENV_MS = 1              # envelope resolution
GLITCH_MS = 20          # shorter on/off runs are merged into their neighbours

MORSE = {
    ".-": "A", "-...": "B", "-.-.": "C", "-..": "D", ".": "E", "..-.": "F",
    "--.": "G", "....": "H", "..": "I", ".---": "J", "-.-": "K", ".-..": "L",
    "--": "M", "-.": "N", "---": "O", ".--.": "P", "--.-": "Q", ".-.": "R",
    "...": "S", "-": "T", "..-": "U", "...-": "V", ".--": "W", "-..-": "X",
    "-.--": "Y", "--..": "Z", "-----": "0", ".----": "1", "..---": "2",
    "...--": "3", "....-": "4", ".....": "5", "-....": "6", "--...": "7",
    "---..": "8", "----.": "9",
}


def peak(psd, freqs, lo, hi):
    """Frequency (interpolated) and power of the strongest bin in [lo, hi]."""
    idx = np.nonzero((freqs >= lo) & (freqs <= hi))[0]
    k = int(idx[np.argmax(psd[idx])])
    f = freqs[k]
    if 0 < k < len(psd) - 1:
        a, b, c = np.log(psd[k - 1:k + 2] + 1e-30)
        den = a - 2 * b + c
        if den != 0:
            f += 0.5 * (a - c) / den * (freqs[1] - freqs[0])
    return float(f), float(psd[k])


def envelope(x, f):
    """|x| at frequency f, averaged over ENV_MS blocks (1 kHz rate)."""
    n = int(FS * ENV_MS / 1000)
    m = len(x) // n
    bb = x[:m * n] * np.exp(-2j * np.pi * f * np.arange(m * n) / FS)
    return np.abs(bb.reshape(m, n).mean(axis=1))


def runs(on):
    """Run lengths of a boolean array: [(state, start, length)]."""
    edges = np.nonzero(np.diff(on.astype(np.int8)))[0] + 1
    starts = np.concatenate(([0], edges))
    ends = np.concatenate((edges, [len(on)]))
    return [(bool(on[s]), int(s), int(e - s)) for s, e in zip(starts, ends)]


def merge_glitches(rs, shortest):
    out = []
    for st, s, n in rs:
        if out and (n < shortest or out[-1][0] == st):
            pst, ps, pn = out[-1]
            out[-1] = (pst, ps, pn + n)      # absorbed by the previous run
        else:
            out.append((st, s, n))
    return out


def analyze(x):
    m = {}
    # Spectrum averaged over the capture (1.5 Hz bins)
    nfft = 1 << 16
    segs = len(x) // nfft
    win = np.hanning(nfft).astype(np.float32)
    psd = np.zeros(nfft)
    for i in range(segs):
        psd += np.abs(np.fft.fft(x[i * nfft:(i + 1) * nfft] * win)) ** 2
    psd = np.fft.fftshift(psd / max(segs, 1))
    freqs = np.fft.fftshift(np.fft.fftfreq(nfft, 1 / FS))
    f_tone, p_tone = peak(psd, freqs, 2e3, 45e3)
    # The carrier (LO leakage) is TONE_HZ below the tone, or above it if the
    # sideband is the other one: take the stronger
    f_lo, p_lo = max((peak(psd, freqs, f_tone + s * TONE_HZ - 30, f_tone + s * TONE_HZ + 30)
                      for s in (-1, 1)), key=lambda fp: fp[1])
    m["tone_above_carrier_hz"] = round(f_tone - f_lo, 2)
    m["carrier_offset_hz"] = round(f_lo + RX_OFFSET, 1)     # vs 915 MHz, B200mini's clock
    m["carrier_leak_dbc"] = round(10 * np.log10(p_lo / p_tone), 1)

    env = envelope(x, f_tone)
    db = 20 * np.log10(env + 1e-12)
    top, floor = np.percentile(db, 95), np.percentile(db, 20)
    m["tone_dbfs"] = round(float(top), 1)
    m["on_off_db"] = round(float(top - floor), 1)
    on = db > (top + floor) / 2
    rs = merge_glitches(runs(on), GLITCH_MS / ENV_MS)
    ons = [n for st, s, n in rs if st]
    if len(ons) < 5:
        m["error"] = "no keyed tone found"
        return m
    ons_s = np.array(ons) * ENV_MS / 1000
    unit = float(np.median(ons_s[ons_s < 1.8 * np.percentile(ons_s, 10)]))

    # Messages: separated by >= 10 units of silence; complete ones have that
    # silence on both sides inside the capture
    msgs, cur, boundary = [], None, False
    for st, s, n in rs:
        d = n * ENV_MS / 1000 / unit
        if st:
            if cur is None:
                cur = {"start": s * ENV_MS / 1000, "elements": [], "complete": boundary}
            cur["elements"].append(("on", d, n * ENV_MS / 1000))
        elif d >= 10:
            if cur is not None:
                msgs.append(cur)        # closed by a long silence
                cur = None
            boundary = True
        elif cur is not None:
            cur["elements"].append(("off", d, n * ENV_MS / 1000))
        else:
            boundary = False            # the capture began inside a message
    if cur is not None:
        cur["complete"] = False         # cut off by the capture end
        msgs.append(cur)

    texts, dots, dashes, gaps1 = [], [], [], []
    for msg in msgs:
        word, letter, words = [], "", []
        # element statistics from complete messages only (a cut-off one may
        # end in a truncated tone)
        stats = msg["complete"]
        for kind, d, secs in msg["elements"]:
            if kind == "on":
                if d < 2:
                    letter += "."
                    if stats:
                        dots.append(secs)
                elif d < 5:
                    letter += "-"
                    if stats:
                        dashes.append(secs)
                else:
                    letter += "?"
            else:
                if d < 2:
                    if stats:
                        gaps1.append(secs)
                    continue
                word.append(MORSE.get(letter, "?"))
                letter = ""
                if d >= 5:
                    words.append("".join(word))
                    word = []
        if letter:
            word.append(MORSE.get(letter, "?"))
        if word:
            words.append("".join(word))
        msg["text"] = " ".join(words)
        if msg["complete"]:
            texts.append(msg["text"])
    starts = [msg["start"] for msg in msgs if msg["complete"]]
    m["messages"] = texts
    m["message_starts_s"] = [round(s, 3) for s in starts]
    m["periods_s"] = [round(b - a, 3) for a, b in zip(starts, starts[1:])]
    m["dot_s"] = round(float(np.mean(dots)), 4) if dots else None
    m["dash_over_dot"] = round(float(np.mean(dashes) / np.mean(dots)), 3) if dots and dashes else None
    m["gap_s"] = round(float(np.mean(gaps1)), 4) if gaps1 else None
    m["first_tone_s"] = round(rs[0][2] * ENV_MS / 1000, 3) if not rs[0][0] else 0.0

    # Carrier leakage with TX on (silences inside messages) and off (between)
    lo_env = 20 * np.log10(envelope(x, f_lo) + 1e-12)
    inside, between = [], []
    for msg in msgs:
        t = int(msg["start"] * 1000 / ENV_MS)
        inside.append(lo_env[max(0, t - 900):max(0, t - 100)])  # the 1 s of silence before
    for a, b in zip(msgs, msgs[1:]):
        # TX goes off 1.68 s (the closing word gap) + 1 s after the last tone
        # and back on 1 s before the next message's first
        t0 = int((a["start"] + sum(e[2] for e in a["elements"])) * 1000 / ENV_MS)
        t1 = int(b["start"] * 1000 / ENV_MS)
        if t1 - t0 > 7000:
            between.append(lo_env[t0 + 3500:t1 - 2000])
    if inside and sum(len(v) for v in inside):
        m["carrier_leak_tx_on_dbfs"] = round(float(np.median(np.concatenate(inside))), 1)
    if between and sum(len(v) for v in between):
        m["carrier_leak_tx_off_dbfs"] = round(float(np.median(np.concatenate(between))), 1)

    fails = []
    if abs(abs(m["tone_above_carrier_hz"]) - TONE_HZ) > TONE_TOL_HZ:
        fails.append("tone %.2f Hz above the carrier" % m["tone_above_carrier_hz"])
    if not texts:
        fails.append("no complete message")
    if any(t != TEXT for t in texts):
        fails.append("decoded %s" % texts)
    if m["dot_s"] is None or abs(m["dot_s"] - UNIT_S) > UNIT_TOL * UNIT_S:
        fails.append("dot %s s" % m["dot_s"])
    if any(not PERIOD_S[0] <= p <= PERIOD_S[1] for p in m["periods_s"]):
        fails.append("periods %s" % m["periods_s"])
    if m["on_off_db"] < ON_OFF_DB:
        fails.append("on/off %.1f dB" % m["on_off_db"])
    m["pass"] = not fails
    if fails:
        m["fails"] = fails
    return m


def load(rbf):
    """Load the image without libbladeRF talking to the Nios first; the
    set-up after the load fails against the foxhunt firmware (expected)."""
    env = dict(os.environ, BLADERF_FORCE_NO_FPGA_PRESENT="1")
    t = time.time()
    p = subprocess.run([rf.CLI, "-l", rbf], capture_output=True, text=True,
                       timeout=180, env=env)
    return {"load_rc": p.returncode, "load_s": round(time.time() - t, 2),
            "load_output": (p.stdout + p.stderr)[-1500:]}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rbf")
    ap.add_argument("--label", default="")
    ap.add_argument("--seconds", type=float, default=75)
    ap.add_argument("--out", default="-")
    ap.add_argument("--save", help="keep the capture (complex64 .npy)")
    a = ap.parse_args()
    if os.environ.get("HWTEST_TX") != "1":
        sys.exit("foxhunt-check.py: a foxhunt image transmits on its own: set "
                 "HWTEST_TX=1 only with the transmit cabling (see the help)")
    u = rf.usrp()
    n = int(a.seconds * FS)
    cap = {}

    def capture():
        try:
            cap["x"] = rf.b200_capture(u, n, FC + RX_OFFSET, FS, B200_RX_GAIN)
        except Exception as e:     # reported below
            cap["error"] = str(e)

    th = threading.Thread(target=capture)
    t0 = time.time()
    th.start()
    time.sleep(2.0)
    m = {"label": a.label, "rbf": a.rbf, "time": t0}
    m.update(load(a.rbf))
    m["load_started_s"] = 2.0
    th.join()
    if "error" in cap:
        m["error"] = cap["error"]
        m["pass"] = False
    else:
        x = cap["x"]
        if a.save:
            np.save(a.save, x)
        m.update(analyze(x))
        if "first_tone_s" in m:
            m["first_tone_after_load_start_s"] = round(m["first_tone_s"] - 2.0, 3)
    out = sys.stdout if a.out == "-" else open(a.out, "a")
    out.write(json.dumps(m) + "\n")
    out.flush()
    return 0 if m.get("pass") else 1


if __name__ == "__main__":
    sys.exit(main())
