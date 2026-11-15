#!/usr/bin/env python3
"""The start of bladeRF transmissions, seen by the B200mini: written to
reproduce two low 433 MHz TX readings (channel 1, host tuning mode, each right
after an RX check), it showed that every transmission can start with up to
~0.7 s of short bursts, 10-25 dB low and ~1.5 kHz off, on images A and C alike
(TESTING.md, section 13).

  tx-startup.py LABEL N OUT.jsonl SAVE_DIR

N iterations on the image that is loaded. Each: rf-check's RX check (variant S:
300 MHz at 2 Msps, as the low-band sweep; variant R: 100 MHz at 2 Msps, as
rf-check's first point), then the bladeRF transmits rf-check's 433 MHz tone (S:
2 Msps, R: 4 Msps) for 10 s while the B200mini captures 50 ms every ~0.3 s.
Before capture 6 the B200mini's RX LO is tuned 20 MHz away and back: if a low
tone recovers then, the B200mini was the problem, if it stays low, the bladeRF.
The B200mini's RX "lo_locked" sensor is read before each capture. Iterations
with a low capture keep their captures (complex64 .npy) in SAVE_DIR.
Transmits: HWTEST_TX=1 only (rf-check's gains and caps).
"""
import importlib.util
import json
import os
import sys
import tempfile
import threading
import time

import numpy as np

H = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rfcheck", os.path.join(H, "rf-check.py"))
rf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rf)

VARIANTS = {"S": ((300e6, 2e6, 250e3), (433e6, 2e6, 250e3)),
            "R": ((100e6, 2e6, 250e3), (433e6, 4e6, 500e3))}
LOW_DB = -38.0          # a normal 433 MHz tone reads about -33 dBFS
RETUNE_AT = 6


def b200_locked(u):
    try:
        return bool(u.get_rx_sensor("lo_locked", 0).to_bool())
    except Exception as e:      # sensor name differs: record why
        return "?" + str(e)[:40]


def tx_series(u, point):
    import uhd
    fc, fs, f_off = point
    n = int(fs * 0.1)
    series, caps, result = [], [], {}
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "tone.bin")
        rf.to_sc16q11(rf.tone(n, fs, f_off, 0.5)).tofile(path)
        ch = rf.BLADERF_CH
        cmds = ["set frequency tx%d %d" % (ch, fc), "set samplerate tx%d %d" % (ch, fs),
                "set bandwidth tx%d %d" % (ch, min(fs, 56e6)), "set gain tx%d %d" % (ch, rf.BLADERF_TX_GAIN),
                "tx config file=%s format=bin repeat=100 channel=%d" % (path, ch), "tx start", "tx wait 20s"]

        def bladerf_tx():
            try:
                result["out"] = rf.run_cli(cmds)
            except Exception as e:
                result["error"] = str(e)

        t = threading.Thread(target=bladerf_tx)
        t.start()
        t0 = time.time()
        try:
            time.sleep(2.0)
            k = 0
            while t.is_alive() and time.time() - t0 < 13.0:
                if k == RETUNE_AT:
                    u.set_rx_freq(uhd.types.TuneRequest(fc + 20e6), 0)   # b200_capture tunes back
                lock = b200_locked(u)
                x = rf.b200_capture(u, int(fs * 0.05), fc, fs, rf.B200_RX_GAIN)
                m = rf.analyze(x, fs, f_off)
                found = m["tone_dbfs"] > m["floor_dbfs"] + 30 and abs(m["freq_err_hz"]) < 10e-6 * fc + 2000
                series.append({"k": k, "t": round(time.time() - t0, 2), "lock": lock, "found": found,
                               "tone": round(m["tone_dbfs"], 1), "err": round(m["freq_err_hz"]),
                               "rms": round(m["rms_dbfs"], 1), "dc": round(m["dc_dbc"]), "img": round(m["image_dbc"])})
                caps.append(x)
                k += 1
                time.sleep(0.2)
        finally:
            t.join()
    return series, caps, result.get("error")


def main():
    label, n, out, save = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    if os.environ.get("HWTEST_TX") != "1":
        sys.exit("tx-startup.py transmits: HWTEST_TX=1 only with the transmit cabling")
    rf.check_channel()
    os.makedirs(save, exist_ok=True)
    u = rf.usrp()
    with open(out, "a") as f:
        for it in range(n):
            var = "S" if it % 2 == 0 else "R"
            rxp, txp = VARIANTS[var]
            rec = {"label": label, "it": it, "variant": var, "ch": rf.BLADERF_CH,
                   "mode": os.environ.get("BLADERF_DEFAULT_TUNING_MODE", "host"), "time": time.time()}
            try:
                rx = rf.check_rx(u, rxp)
                rec["rx_tone"] = round(rx["tone_dbfs"], 1)
            except Exception as e:
                rec["rx_error"] = str(e)[:200]
            series, caps, err = tx_series(u, txp)
            rec.update(series=series, tx_error=err)
            on = [s for s in series if s["found"]]
            low = [s for s in on if s["tone"] < LOW_DB]
            rec["n_on"], rec["n_low"] = len(on), len(low)
            rec["low_before_retune"] = sum(1 for s in low if s["k"] < RETUNE_AT)
            rec["low_after_retune"] = sum(1 for s in low if s["k"] >= RETUNE_AT)
            if low:
                tag = "%s-it%d-%s" % (label, it, time.strftime("%H%M%S"))
                np.save(os.path.join(save, tag + ".npy"), np.array(caps, dtype=object), allow_pickle=True)
                rec["saved"] = tag
            f.write(json.dumps(rec) + "\n")
            f.flush()
            print("%s it%d %s: rx %s; tx on %d, low %d (before retune %d, after %d); %s" % (
                label, it, var, rec.get("rx_tone", rec.get("rx_error")), len(on), len(low),
                rec["low_before_retune"], rec["low_after_retune"],
                " ".join("%s%.0f" % ("*" if s["found"] and s["tone"] < LOW_DB else "", s["tone"]) for s in series)),
                flush=True)


if __name__ == "__main__":
    main()
