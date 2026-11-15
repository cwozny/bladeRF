#!/usr/bin/env python3
"""Low-band sweep with rf-check's TX and RX checks on the channel that
HWTEST_RF_CH names (rf-check's gains and caps; transmits: HWTEST_TX=1 only).

  rf-lowband.py OUT.jsonl
"""
import importlib.util
import json
import os
import sys
import time

H = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("rfcheck", os.path.join(H, "rf-check.py"))
rf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rf)

POINTS = [(f * 1e6, 2e6, 250e3) for f in (70, 85, 100, 125, 150, 200, 250, 300, 433)]

if os.environ.get("HWTEST_TX") != "1":
    sys.exit("rf-lowband.py transmits: HWTEST_TX=1 only with the transmit cabling")
rf.check_channel()
u = rf.usrp()
with open(sys.argv[1], "a") as out:
    for point in POINTS:
        for name, fn in (("tx", rf.check_tx), ("rx", rf.check_rx)):
            errors = []
            for _ in range(rf.TRIES):
                try:
                    m = fn(u, point)
                    break
                except Exception as e:
                    errors.append(str(e))
            else:
                m = {"direction": "bladerf_" + name, "fc": point[0], "error": errors[-1]}
            m.update(time=time.time(), bladerf_ch=rf.BLADERF_CH,
                     mode=os.environ.get("BLADERF_DEFAULT_TUNING_MODE", "host"))
            if errors:
                m["retry_errors"] = errors
            out.write(json.dumps(m) + "\n")
            out.flush()
            print("%s %.0f MHz: %s" % (m["direction"], point[0] / 1e6,
                                      m.get("error") or "%.1f dBFS" % m["tone_dbfs"]), flush=True)
