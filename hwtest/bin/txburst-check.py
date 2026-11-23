#!/usr/bin/env python3
"""txburst-check.py: timed TX bursts on one or two channels through the Python binding, checked
through the AD9361's internal loopback (BLADERF_LB_RFIC_BIST), every received sample included.

Usage: txburst-check.py [-f sc8|sc16] [-m auto|off|on] [-c 1|2] [--x2] [--no-1r1t] [-r RATE]
       [-k BURSTS] [-l LENGTH] [-p PERIOD] [-b TX_BUFFER] [--tx-buffers N] [--cont]
       [--zero-first] [-d DEVICE] [-s FILE.npy]
Run with PYTHONPATH at a Python binding that has the #1100 format values (and the 1R1T calls of
pr/1r1t unless --no-1r1t), with `python3 -P` when another binding sits next to this file.
Transmits: TX<ch> feeds RX<ch>, and every TX gain goes to the minimum of its range before TX is
enabled (no TX cap applies to Python's cffi calls); use loads, no antennas.

K numbered bursts of L samples, P samples apart, are queued with sync_tx (BURST_START with a
timestamp, then BURST_END with 2 zero samples, as in test_1r1t_txsched), then one scheduled
sync_rx read covers them all. Burst sample n carries w = n mod 2^15 (--x2: channel 2 carries the
numbers of channel 1 plus 0x4000). Pass, on every channel: every burst found (by its samples 0 and
1), the same delay (RX timestamp minus TX timestamp) for every burst, spacing exactly P, every
burst sample unchanged, every other sample zero, the RX read complete and without overrun.
--cont: one continuous timed burst of K * L samples instead (BURST_START once, K writes of L
samples, then BURST_END): the stream must stay whole. TX and RX together must fit in USB 3
(about 245 MB/s each way overruns the RX). --zero-first: every burst starts with one zero sample
before the numbered samples (the workaround for Nuand/bladeRF fifo_reader's early first sample).
Written for the fifo_reader timed-TX faults (TESTING.md section 22)."""
import argparse
import math
import os
import sys

import numpy as np

os.environ.setdefault("BLADERF_DEFAULT_TUNING_MODE", "host")

import bladerf  # noqa: E402
from bladerf import _bladerf  # noqa: E402

ffi = _bladerf.ffi
lib = _bladerf.libbladeRF

FLAG_TX_BURST_START = 1 << 0
FLAG_TX_BURST_END = 1 << 1
STATUS_OVERRUN = 1 << 0
TIMEOUT_MS = 5000
LEAD_S = 0.5
CH2_OFFSET = 0x4000


def encode(w, sc16):
    """Interleaved I/Q for the numbers w (test_1r1t_txsched's put_sample)."""
    if sc16:
        s = np.empty(2 * len(w), dtype=np.int16)
        s[0::2] = w & 0x7FF
        s[1::2] = -2048 + (w >> 11)
    else:
        s = np.empty(2 * len(w), dtype=np.uint8)
        s[0::2] = w & 0xFF
        s[1::2] = 0x80 | (w >> 8)
        s = s.view(np.int8)
    return s


def decode(s, sc16):
    """The numbers the samples carry and whether each sample is nonzero."""
    i = s[0::2].astype(np.int32)
    q = s[1::2].astype(np.int32)
    if sc16:
        w = (((q + 2048) & 0xF) << 11) | (i & 0x7FF)
    else:
        w = ((q & 0x7F) << 8) | (i & 0xFF)
    return w, (i != 0) | (q != 0)


def check(w, nz, k_n, length, period, ts_rx, ts0, offset, label):
    """Finds the bursts of one channel; returns (ok, delays, found, stray, wrong)."""
    n = len(w)
    starts, delays, wrong = [], [], 0
    in_burst = np.zeros(n, dtype=bool)
    pos = 0
    for k in range(k_n):
        v0 = (k * length + offset) & 0x7FFF
        v1 = (k * length + 1 + offset) & 0x7FFF
        hit = np.flatnonzero((w[pos:-1] == v0) & (w[pos + 1:] == v1)
                             & nz[pos:-1] & nz[pos + 1:])
        if len(hit) == 0:
            print("  %s burst %d not found" % (label, k))
            starts.append(None)
            continue
        i0 = pos + int(hit[0])
        starts.append(i0)
        delays.append(ts_rx + i0 - (ts0 + k * period))
        exp = (k * length + np.arange(length) + offset) & 0x7FFF
        seg = w[i0:i0 + length]
        bad = int(np.count_nonzero(seg != exp[:len(seg)])) + (length - len(seg))
        wrong += bad
        if bad:
            print("  %s burst %d: %d wrong samples" % (label, k, bad))
        in_burst[i0:i0 + length] = True
        pos = i0 + length
    stray = np.flatnonzero(nz & ~in_burst)
    found = [s for s in starts if s is not None]
    spacing = sorted({found[j + 1] - found[j] for j in range(len(found) - 1)})
    print("%s: TX->RX delays: %s" % (
        label, ", ".join("%d x%d" % (d, delays.count(d)) for d in sorted(set(delays)))))
    print("%s: bursts found: %d of %d; RX spacing %s (expected %d)" % (
        label, len(found), k_n, spacing, period))
    held = ""
    if len(stray):
        vals = sorted({int(w[j]) for j in stray[:2000]})
        held = " (first at RX index %s; values %s)" % (stray[:3].tolist(), vals[:4])
    print("%s: wrong burst samples: %d; nonzero samples outside the bursts: %d%s" % (
        label, wrong, len(stray), held))
    ok = (len(found) == k_n and len(set(delays)) == 1 and (k_n == 1 or spacing == [period])
          and wrong == 0 and len(stray) == 0)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-f", "--format", choices=("sc8", "sc16"), default="sc16")
    ap.add_argument("-m", "--mode", choices=("auto", "off", "on"), default="auto")
    ap.add_argument("-c", "--channel", type=int, choices=(1, 2), default=1)
    ap.add_argument("--x2", action="store_true", help="both channels (TX_X2, RX_X2)")
    ap.add_argument("--no-1r1t", action="store_true", help="no 1R1T calls (Nuand's library)")
    ap.add_argument("-r", "--rate", type=float, default=0)
    ap.add_argument("-k", "--bursts", type=int, default=16)
    ap.add_argument("-l", "--length", type=int, default=4096)
    ap.add_argument("-p", "--period", type=int, default=0)
    ap.add_argument("-b", "--tx-buffer", type=int, default=8192,
                    help="TX buffer size in samples (default 8192)")
    ap.add_argument("-s", "--save", help="save the RX samples (.npy) here")
    ap.add_argument("--cont", action="store_true",
                    help="one continuous burst of K * L samples")
    ap.add_argument("-d", "--device", default=None, help="device string, e.g. *:serial=...")
    ap.add_argument("--tx-buffers", type=int, default=256,
                    help="TX buffers (default 256: the writes do not block, so RX keeps up)")
    ap.add_argument("--zero-first", action="store_true",
                    help="every burst starts with one zero sample (the payload follows it)")
    a = ap.parse_args()

    sc16 = a.format == "sc16"
    nch = 2 if a.x2 else 1
    rate = a.rate or (30.72e6 if a.x2 else (61.44e6 if sc16 else 122.88e6))
    period = a.period or int(rate / 1000)
    k_n, length = a.bursts, a.length
    fmt = _bladerf.Format.SC16_Q11_META if sc16 else _bladerf.Format.SC8_Q7_META
    chans = [0, 1] if a.x2 else [a.channel - 1]
    assert 2 <= length <= 32767 and (a.cont or period >= 2 * length)

    print("libbladeRF %s" % bladerf.version().describe)
    dev = bladerf.BladeRF(a.device) if a.device else bladerf.BladeRF()
    result = "ERROR"
    try:
        tm = ffi.new("bladerf_tuning_mode *")
        lib.bladerf_get_tuning_mode(dev.dev[0], tm)
        print("tuning mode %s (BLADERF_DEFAULT_TUNING_MODE=%s)" % (
            "host" if tm[0] == lib.BLADERF_TUNING_MODE_HOST else "fpga",
            os.environ["BLADERF_DEFAULT_TUNING_MODE"]))
        if not sc16:
            dev.enable_feature(_bladerf.Feature.Oversample, True)
        if not a.no_1r1t:
            dev.set_rfic_1r1t_mode({"auto": _bladerf.RFIC1R1TMode.Auto,
                                    "off": _bladerf.RFIC1R1TMode.Off,
                                    "on": _bladerf.RFIC1R1TMode.On}[a.mode])
        for c in chans:
            dev.set_sample_rate(bladerf.CHANNEL_RX(c), int(rate))
            dev.set_sample_rate(bladerf.CHANNEL_TX(c), int(rate))
        dev.set_rx_mux(_bladerf.RXMux.Baseband)
        dev.set_loopback(_bladerf.Loopback.RFIC_BIST)

        gmin = {}
        for c in chans:
            tx = bladerf.CHANNEL_TX(c)
            gr = dev.get_gain_range(tx)
            gmin[c] = gr.min * gr.scale
            g = math.ceil(gmin[c])
            dev.set_gain(tx, g)
            print("TX%d gain range %.2f..%.2f dB; set to %d dB, reads back %d dB" % (
                c + 1, gr.min * gr.scale, gr.max * gr.scale, g, dev.get_gain(tx)))

        lay_rx = _bladerf.ChannelLayout.RX_X2 if a.x2 else _bladerf.ChannelLayout.RX_X1
        lay_tx = _bladerf.ChannelLayout.TX_X2 if a.x2 else _bladerf.ChannelLayout.TX_X1
        dev.sync_config(lay_rx, fmt, 256, 32768, 32, TIMEOUT_MS)
        dev.sync_config(lay_tx, fmt, a.tx_buffers, a.tx_buffer, 16, TIMEOUT_MS)
        for c in chans:
            dev.enable_module(bladerf.CHANNEL_TX(c), True)
        for c in chans:
            dev.enable_module(bladerf.CHANNEL_RX(c), True)
        for c in chans:
            g_on = dev.get_gain(bladerf.CHANNEL_TX(c))
            print("TX%d gain with TX enabled: %d dB" % (c + 1, g_on))
            if g_on > gmin[c] + 1.0:
                print("the TX gain is not at its minimum: stopping")
                return 2

        r1t = False if a.no_1r1t else dev.get_rfic_1r1t()
        print("%s, %s, %.2f Msps, %s mode: %d bursts of %d samples every %d" % (
            "TX1+TX2 to RX1+RX2" if a.x2 else "TX%d to RX%d" % (a.channel, a.channel),
            "SC16_Q11_META" if sc16 else "SC8_Q7_META, OVERSAMPLE", rate / 1e6,
            "1R1T" if r1t else "2R2T", k_n, length, period))

        now = ffi.new("uint64_t *")
        ret = lib.bladerf_get_timestamp(dev.dev[0], lib.BLADERF_TX, now)
        assert ret == 0, ret
        ts0 = now[0] + int(LEAD_S * rate)

        meta = ffi.new("struct bladerf_metadata *")
        zeros = np.zeros(4 * nch, dtype=np.int16 if sc16 else np.int8)
        for k in range(k_n):
            w = (k * length + np.arange(length)) & 0x7FFF
            if a.x2:
                s1 = encode(w, sc16).reshape(-1, 2)
                s2 = encode((w + CH2_OFFSET) & 0x7FFF, sc16).reshape(-1, 2)
                burst = np.hstack((s1, s2)).reshape(-1)
            else:
                burst = encode(w, sc16)
            if a.zero_first:
                # the zero sample takes the burst's timestamp; the numbered payload follows it
                burst = np.concatenate((np.zeros(2 * nch, dtype=burst.dtype), burst[:-2 * nch]))
            if a.cont:
                meta.flags = FLAG_TX_BURST_START if k == 0 else 0
                meta.timestamp = ts0
            else:
                meta.flags = FLAG_TX_BURST_START
                meta.timestamp = ts0 + k * period
            dev.sync_tx(burst, length * nch, TIMEOUT_MS, meta)
            if not a.cont or k == k_n - 1:
                meta.flags = FLAG_TX_BURST_END
                dev.sync_tx(zeros, 2 * nch, TIMEOUT_MS, meta)

        ts_rx = ts0 - period
        n = (k_n + 2) * period if not a.cont else k_n * length + 2 * period
        buf = np.zeros(2 * n * nch, dtype=np.int16 if sc16 else np.int8)
        rmeta = ffi.new("struct bladerf_metadata *")
        rmeta.flags = 0
        rmeta.timestamp = ts_rx
        dev.sync_rx(buf, n * nch, 2 * TIMEOUT_MS, rmeta)
        print("RX read: %d of %d samples from timestamp %d (asked %d), status 0x%x"
              % (rmeta.actual_count, n * nch, rmeta.timestamp, ts_rx, rmeta.status))
        if a.save:
            np.save(a.save, buf)
            print("RX samples saved in %s (index 0 = timestamp %d; TX timestamps "
                  "%d + k * %d)" % (a.save, ts_rx, ts0, period))

        oks = []
        per = buf.reshape(-1, 2 * nch)
        for j, c in enumerate(chans):
            w, nz = decode(per[:, 2 * j:2 * j + 2].reshape(-1), sc16)
            if a.cont:
                oks.append(check(w, nz, 1, k_n * length, period, ts_rx, ts0,
                                 CH2_OFFSET if (a.x2 and j == 1) else 0, "ch%d" % (c + 1)))
            else:
                oks.append(check(w, nz, k_n, length, period, ts_rx, ts0,
                                 CH2_OFFSET if (a.x2 and j == 1) else 0, "ch%d" % (c + 1)))
        ok = (all(oks) and rmeta.actual_count == n * nch
              and not rmeta.status & STATUS_OVERRUN)
        result = "PASS" if ok else "FAIL"
    finally:
        try:
            for c in chans:
                dev.enable_module(bladerf.CHANNEL_RX(c), False)
                dev.enable_module(bladerf.CHANNEL_TX(c), False)
            dev.set_loopback(_bladerf.Loopback.Disabled)
        finally:
            dev.close()
        print("RESULT: %s" % result)
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
