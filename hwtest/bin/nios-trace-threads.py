#!/usr/bin/env python3
"""Analyze a lib/nios-trace.c log of any libbladeRF program on a bladeRF 2.0
(written for test_ctrl's threads case in FPGA tuning mode).

  nios-trace-threads.py <trace> [episodes to print (default 6)]

Reports
- requests, responses read, IN time-outs, responses that belong to another
  request (different packet type, target or address);
- how long the Nios took to answer (successful IN reads);
- for every RFIC command written (16x64, target RFIC, write): how long until
  a status read showed the Nios's work queue empty, per command;
- every IN time-out with what the Nios had last been given to do, and the
  packet sequence around the first few.
"""
import collections
import statistics
import sys

RFIC_CMDS = {0: "STATUS", 1: "INIT", 2: "ENABLE", 3: "SAMPLERATE",
             4: "FREQUENCY", 5: "BANDWIDTH", 6: "GAINMODE", 7: "GAIN",
             8: "RSSI", 9: "FILTER", 10: "TXMUTE", 11: "FASTLOCK"}
T8x32 = {0: "VERSION", 1: "CONFIG", 2: "ADF4351", 3: "RFFE_CSR",
         4: "ADF400X", 5: "FASTLOCK"}
LIBUSB_TIMEOUT = -7


def rfic(b):
    """(cmd, ch, is_write, ok, data) of a 16x64 RFIC packet, else None."""
    if len(b) < 16 or b[0] != ord("E") or b[1] != 1:
        return None
    addr = b[4] | (b[5] << 8)
    return (RFIC_CMDS.get(addr & 0xFF, "cmd%d" % (addr & 0xFF)),
            (addr >> 8) & 0xF, bool(b[2] & 1), bool(b[2] & 2),
            int.from_bytes(bytes(b[6:14]), "little"))


def name(b):
    """Short name of a request: what kind of access it is."""
    r = rfic(b)
    if r:
        return "RFIC %s %s" % ("W" if r[2] else "R", r[0])
    if len(b) < 16:
        return "(%d bytes)" % len(b)
    m = chr(b[0]) if 32 <= b[0] < 127 else "?"
    if m == "C":
        return "8x32 %s %s" % ("W" if b[2] & 1 else "R",
                               T8x32.get(b[1], "t%d" % b[1]))
    if m in "ABDEK":
        return "%s-pkt t%d %s" % (m, b[1], "W" if b[2] & 1 else "R")
    return "%s-pkt" % m


def decode(b):
    r = rfic(b)
    if r:
        cmd, ch, w, ok, data = r
        s = "%s RFIC %s ch%d%s" % ("W" if w else "R", cmd, ch,
                                   " ok" if ok else "")
        if cmd == "STATUS":
            return s + " (init=%d wqlen=%d)" % (data & 1, (data >> 8) & 0xFF)
        return s + " data=%d" % data
    return name(b) + " " + " ".join("%02x" % x for x in b[:8])


def key(b):
    """What a response must echo from its request."""
    if len(b) < 16:
        return None
    if b[0] == ord("E"):
        return (b[0], b[1], b[4], b[5])
    return (b[0],)


def pct(v, q):
    return v[min(len(v) - 1, int(q * (len(v) - 1)))]


def main():
    path = sys.argv[1]
    max_eps = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    ev = []     # (t, d, ep, rc, bytes)
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 7 and p[0] == "U":
                ev.append((float(p[1]), float(p[2]), int(p[3], 16), int(p[6]),
                           [int(x, 16) for x in p[7:]]))

    n_req = n_in = 0
    tmo, mism = [], []          # event indices
    resp_ms = []                # successful IN durations
    last_req = None
    last_req_i = None
    # RFIC command completion: write -> first matched STATUS read with wqlen 0
    pending = None              # (cmd, t of the write's OUT, event index)
    done = collections.defaultdict(list)
    open_at_tmo = []            # (event index of the time-out, pending command)
    before = None               # what was pending before the last request
    for i, (t, d, ep, rc, b) in enumerate(ev):
        if ep == 0x02:
            n_req += 1
            last_req, last_req_i = b, i
            before = pending
            r = rfic(b)
            if r and r[2]:
                pending = (r[0], t, i)
        elif ep == 0x82:
            n_in += 1
            if rc == LIBUSB_TIMEOUT:
                tmo.append(i)
                open_at_tmo.append((i, last_req_i, before))
                continue
            if rc != 0:
                continue
            resp_ms.append(d / 1000.0)
            if last_req is not None and key(b) != key(last_req):
                mism.append(i)
                continue
            r = rfic(b)
            if r and r[0] == "STATUS" and not r[2] and pending is not None:
                if (r[4] >> 8) & 0xFF == 0:
                    done[pending[0]].append((t + d - pending[1]) / 1000.0)
                    pending = None

    print("trace %s" % path)
    print("  requests %d, responses read %d, IN time-outs %d, "
          "responses belonging to another request %d"
          % (n_req, n_in, len(tmo), len(mism)))
    if resp_ms:
        v = sorted(resp_ms)
        print("  Nios answer time (successful IN reads): median %.2f ms, "
              "p99 %.1f, p99.9 %.1f, max %.1f; >100 ms: %d, >200 ms: %d"
              % (statistics.median(v), pct(v, 0.99), pct(v, 0.999), v[-1],
                 sum(1 for x in v if x > 100), sum(1 for x in v if x > 200)))

    print("  RFIC commands, write to empty work queue (ms):")
    for cmd, v in sorted(done.items(), key=lambda kv: -len(kv[1])):
        v.sort()
        print("    %-10s n=%6d  median %7.1f  p90 %7.1f  p99 %7.1f  max %7.1f"
              "  >=250 ms: %d"
              % (cmd, len(v), statistics.median(v), pct(v, 0.9), pct(v, 0.99),
                 v[-1], sum(1 for x in v if x >= 250)))

    # How long the Nios stayed silent: from the timed-out request to the end
    # of the next successful response.
    tmo_set, mism_set = set(tmo), set(mism)
    c = collections.Counter()
    spans = []
    for i, ri, pend in open_at_tmo:
        req = name(ev[ri][4]) if ri is not None else "?"
        busy = ("no RFIC command pending" if pend is None
                else "while W %s pending" % pend[0])
        c[(req, busy)] += 1
        for m in range(i + 1, len(ev)):
            if ev[m][2] == 0x82 and ev[m][3] == 0:
                spans.append((ev[m][0] + ev[m][1] - ev[ri][0]) / 1000.0)
                break
    if c:
        print("  IN time-outs by request (and the RFIC command written before "
              "it whose completion was not yet seen):")
        for (req, busy), n in c.most_common():
            print("    %4d  %-24s %s" % (n, req, busy))
        spans.sort()
        if spans:
            print("  Nios silent after a timed-out request (until the next "
                  "answer): median %.0f ms, max %.0f ms"
                  % (statistics.median(spans), spans[-1]))

    for n, (i, ri, pend) in enumerate(open_at_tmo[:max_eps]):
        start = pend[2] if pend is not None else max(0, i - 8)
        start = max(start, i - 40)
        since = ""
        if pend is not None and ri is not None:
            since = ", %.1f ms after W %s" % ((ev[ri][0] - pend[1]) / 1000.0,
                                              pend[0])
        print("\n  time-out %d at %.3f s (request: %s%s):"
              % (n + 1, ev[i][0] / 1e6,
                 name(ev[ri][4]) if ri is not None else "?", since))
        for m in range(start, min(len(ev), i + 7)):
            t, d, ep, rc, b = ev[m]
            note = ""
            if m in tmo_set:
                note = "TIME-OUT"
            elif ep == 0x82 and m in mism_set:
                note = "BELONGS TO ANOTHER REQUEST"
            print("    %11.1f ms %9.1f us  %s rc=%d  %s  %s" % (
                t / 1000.0, d, "OUT" if ep == 2 else "IN ", rc,
                decode(b) if b else "", note))


if __name__ == "__main__":
    main()
