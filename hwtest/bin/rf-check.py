#!/usr/bin/env python3
"""RF checks of the bladeRF against the USRP B200mini, an independent radio.

  rf-check.py tx  [--out results.jsonl]   bladeRF TX -> B200mini RX
  rf-check.py rx  [--out results.jsonl]   B200mini TX -> bladeRF RX
  rf-check.py all [--out results.jsonl]

Expected cabling (fixed for the whole campaign, no antennas; see README.md):
  bladeRF TX2 -> >= 20 dB -> B200mini "RX2" port (UHD's default RX antenna)
  B200mini "TX/RX" port -> >= 20 dB -> bladeRF RX2
Transmit power is capped so that no receiver sees more than -20 dBm: the
bladeRF by lib/tx-cap.c (preloaded with HWTEST_TX=1), the B200mini here
(B200_TX_GAIN_MAX).

A capture fails, and is retried, if the B200mini drops samples (RX overflow,
TX underflow).

For each frequency/sample-rate point one side transmits a CW tone at a known
offset from the tuned frequency and the other side measures it: frequency
error, tone level, image and LO (DC) leakage relative to the tone, and the
noise floor. The numbers are compared between FPGA images (Nios II vs Nios V);
absolute values depend on cabling and are not pass/fail by themselves.

The bladeRF is driven only through bladerf-cli-safe (volatile settings and
streaming; nothing is written to flash).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.join(HERE, "bladerf-cli-safe")

# (center frequency Hz, sample rate Hz, tone offset Hz). Spans the bladeRF 2.0
# RF front-end bands (the Nios firmware switches RF paths by frequency) and
# several AD9361 clock configurations (the Nios firmware programs them).
POINTS = [
    (100e6, 2e6, 250e3),
    (433e6, 4e6, 500e3),
    (915e6, 10e6, 1e6),
    (1575.42e6, 5e6, 400e3),
    (2450e6, 30.72e6, 2e6),
    (3500e6, 20e6, 1.5e6),
    (5800e6, 10e6, 1e6),
]

BLADERF_CH = 2          # bladeRF RX2/TX2 face the B200mini
BLADERF_TX_GAIN = 30    # dB overall (60 ~= 0 dBm): about -30 dBm
BLADERF_RX_GAIN = 30    # dB, manual gain
B200_TX_GAIN = 50       # dB of 89.75 (~+10 dBm at full gain): about -30 dBm
B200_TX_GAIN_MAX = 65   # never more: <= ~-15 dBm out, -35 dBm after 20 dB
B200_RX_GAIN = 30       # dB
TRIES = 3               # captures per point before giving up on dropped samples


def run_cli(cmds, timeout=60):
    args = [CLI]
    for c in cmds:
        args += ["-e", c]
    p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError("bladeRF-cli failed (%d): %s %s"
                           % (p.returncode, p.stdout[-2000:], p.stderr[-2000:]))
    return p.stdout


def tone(n, fs, f_off, amp):
    t = np.arange(n) / fs
    return amp * np.exp(2j * np.pi * f_off * t)


def to_sc16q11(x):
    iq = np.empty(2 * len(x), dtype=np.int16)
    iq[0::2] = np.round(np.real(x) * 2047)
    iq[1::2] = np.round(np.imag(x) * 2047)
    return iq


def from_sc16q11(path):
    iq = np.fromfile(path, dtype=np.int16).astype(np.float32) / 2048.0
    return iq[0::2] + 1j * iq[1::2]


def analyze(x, fs, f_off):
    x = x[len(x) // 8:]           # skip the settling at the start
    n = 1 << int(np.floor(np.log2(len(x))))
    x = x[:n]
    w = np.blackman(n)
    spec = np.fft.fftshift(np.fft.fft(x * w)) / np.sum(w)
    f = np.fft.fftshift(np.fft.fftfreq(n, 1 / fs))
    p = 20 * np.log10(np.abs(spec) + 1e-15)

    def level(fq, bins=4):
        i = int(np.argmin(np.abs(f - fq)))
        return float(np.max(p[max(i - bins, 0):i + bins + 1]))

    k = int(np.argmax(p))
    return {
        "peak_hz": float(f[k]),
        "freq_err_hz": float(f[k] - f_off),
        "tone_dbfs": float(p[k]),
        "image_dbc": level(-f_off) - float(p[k]),
        "dc_dbc": level(0.0) - float(p[k]),
        "floor_dbfs": float(np.median(p)),
        "rms_dbfs": float(10 * np.log10(np.mean(np.abs(x) ** 2) + 1e-15)),
    }


def usrp():
    import uhd
    assert B200_TX_GAIN <= B200_TX_GAIN_MAX
    return uhd.usrp.MultiUSRP("type=b200")


def b200_set_tx_gain(u, gain):
    """Set the B200mini's TX gain, never above B200_TX_GAIN_MAX."""
    if gain > B200_TX_GAIN_MAX:
        raise ValueError("B200mini TX gain %g above the cap" % gain)
    u.set_tx_gain(gain, 0)
    if u.get_tx_gain(0) > B200_TX_GAIN_MAX:
        u.set_tx_gain(0, 0)
        raise RuntimeError("B200mini TX gain did not stay under the cap")


def b200_capture(u, n, fc, fs, gain):
    """Receive n samples with the B200mini; fails on any dropped samples."""
    import uhd
    u.set_rx_rate(fs, 0)
    u.set_rx_freq(uhd.types.TuneRequest(fc), 0)
    u.set_rx_gain(gain, 0)
    st = u.get_rx_stream(uhd.usrp.StreamArgs("fc32", "sc16"))
    md = uhd.types.RXMetadata()
    buf = np.zeros((1, st.get_max_num_samps()), dtype=np.complex64)
    out = np.empty(n, dtype=np.complex64)
    errors = []
    cmd = uhd.types.StreamCMD(uhd.types.StreamMode.start_cont)
    cmd.stream_now = True
    st.issue_stream_cmd(cmd)
    got = 0
    while got < n:
        k = st.recv(buf, md, 1.0)
        if md.error_code != uhd.types.RXMetadataErrorCode.none:
            errors.append(md.strerror())
            if md.error_code == uhd.types.RXMetadataErrorCode.timeout:
                break
        k = min(k, n - got)
        out[got:got + k] = buf[0, :k]
        got += k
    st.issue_stream_cmd(uhd.types.StreamCMD(uhd.types.StreamMode.stop_cont))
    while st.recv(buf, md, 0.1):
        pass
    if errors or got < n:
        raise RuntimeError("B200mini RX dropped samples: %s" % "; ".join(errors[:3]))
    return out


def b200_transmit(u, proto, fc, fs, gain, stop, result):
    """Transmit proto repeatedly until stop is set; counts underflows."""
    import uhd
    u.set_tx_rate(fs, 0)
    u.set_tx_freq(uhd.types.TuneRequest(fc), 0)
    b200_set_tx_gain(u, gain)
    st = u.get_tx_stream(uhd.usrp.StreamArgs("fc32", "sc16"))
    md = uhd.types.TXMetadata()
    amd = uhd.types.TXAsyncMetadata()
    buf = np.tile(proto, int(np.ceil(st.get_max_num_samps() * 8 / len(proto))))
    buf = buf.reshape(1, -1).astype(np.complex64)
    result["underflows"] = []
    t0 = time.time()
    while not stop.is_set():
        st.send(buf, md)
        while st.recv_async_msg(amd, 0.0):
            if amd.event_code in (uhd.types.TXMetadataEventCode.underflow,
                                  uhd.types.TXMetadataEventCode.underflow_in_packet):
                result["underflows"].append(time.time() - t0)
    md.end_of_burst = True
    st.send(np.zeros((1, 1), dtype=np.complex64), md)


def check_tx(u, point):
    """bladeRF transmits, B200mini measures."""
    fc, fs, f_off = point
    n = int(fs * 0.1)  # 100 ms of tone per file pass
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "tone.bin")
        to_sc16q11(tone(n, fs, f_off, 0.5)).tofile(path)
        cmds = [
            "set frequency tx%d %d" % (BLADERF_CH, fc),
            "set samplerate tx%d %d" % (BLADERF_CH, fs),
            "set bandwidth tx%d %d" % (BLADERF_CH, min(fs, 56e6)),
            "set gain tx%d %d" % (BLADERF_CH, BLADERF_TX_GAIN),
            "tx config file=%s format=bin repeat=40 channel=%d" % (path, BLADERF_CH),
            "tx start",
            "tx wait 10s",
        ]
        result = {}
        t = threading.Thread(target=lambda: result.update(out=run_cli(cmds)))
        t.start()
        time.sleep(2.0)   # bladeRF tuned and streaming
        try:
            rx = b200_capture(u, int(fs * 0.05), fc, fs, B200_RX_GAIN)
        finally:
            t.join()
    m = analyze(rx, fs, f_off)
    m.update(direction="bladerf_tx", fc=fc, fs=fs, f_off=f_off)
    return m


def check_rx(u, point):
    """B200mini transmits, bladeRF measures."""
    fc, fs, f_off = point
    n = int(fs * 0.05)
    proto = tone(int(fs * 0.01), fs, f_off, 0.5).astype(np.complex64)
    stop = threading.Event()
    txr = {}
    t = threading.Thread(target=b200_transmit,
                         args=(u, proto, fc, fs, B200_TX_GAIN, stop, txr))
    t.start()
    time.sleep(1.5)
    try:
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "rx.bin")
            run_cli([
                "set frequency rx%d %d" % (BLADERF_CH, fc),
                "set samplerate rx%d %d" % (BLADERF_CH, fs),
                "set bandwidth rx%d %d" % (BLADERF_CH, min(fs, 56e6)),
                "set agc off",
                "set gain rx%d %d" % (BLADERF_CH, BLADERF_RX_GAIN),
                "rx config file=%s format=bin n=%d channel=%d" % (path, n, BLADERF_CH),
                "rx start",
                "rx wait 10s",
            ])
            x = from_sc16q11(path)
    finally:
        stop.set()
        t.join()
    # underflows in the first second are the stream starting up
    late = [x for x in txr.get("underflows", []) if x > 1.0]
    if "underflows" not in txr:
        raise RuntimeError("B200mini transmit thread failed")
    if late:
        raise RuntimeError("B200mini TX underflowed %d times while the bladeRF "
                           "was receiving" % len(late))
    m = analyze(x, fs, f_off)
    m.update(direction="bladerf_rx", fc=fc, fs=fs, f_off=f_off)
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["tx", "rx", "all"])
    ap.add_argument("--out", default="-")
    ap.add_argument("--points", type=int, default=len(POINTS),
                    help="only the first N frequency points")
    a = ap.parse_args()
    if os.environ.get("HWTEST_TX") != "1":
        # Both directions transmit (the bladeRF or the B200mini)
        sys.exit("rf-check.py transmits: set HWTEST_TX=1 only with the cabling "
                 "above (attenuators, no antennas)")
    u = usrp()
    out = sys.stdout if a.out == "-" else open(a.out, "a")
    fails = 0
    for point in POINTS[:a.points]:
        for mode, fn in (("tx", check_tx), ("rx", check_rx)):
            if a.mode not in (mode, "all"):
                continue
            errors = []
            for _ in range(TRIES):  # retry captures that dropped samples
                try:
                    m = fn(u, point)
                    break
                except Exception as e:  # keep going; record the failure
                    errors.append(str(e))
            else:
                m = {"direction": "bladerf_" + mode, "fc": point[0], "error": errors[-1]}
                fails += 1
            m["retries"] = len(errors) if "error" not in m else TRIES - 1
            if errors:
                m["retry_errors"] = errors
            m["time"] = time.time()
            out.write(json.dumps(m) + "\n")
            out.flush()
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
