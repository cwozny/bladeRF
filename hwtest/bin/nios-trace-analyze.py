#!/usr/bin/env python3
"""Analyze a lib/nios-trace.c log of test_ctrl's bandwidth test (FPGA tuning
mode).

  nios-trace-analyze.py <trace> [max failures to print]

Pairs every Nios request (OUT 0x02) with the response read after it (IN 0x82)
and flags responses that belong to a different request (different packet
type, target or address), IN time-outs, and bladerf_set_bandwidth() calls
whose "actual" value differs from the bladerf_get_bandwidth() readback.
"""
import collections
import statistics
import sys

RFIC_CMDS = {0: "STATUS", 1: "INIT", 2: "ENABLE", 3: "SAMPLERATE",
             4: "FREQUENCY", 5: "BANDWIDTH", 6: "GAINMODE", 7: "GAIN",
             8: "RSSI", 9: "FILTER", 10: "TXMUTE", 11: "FASTLOCK"}
LIBUSB_TIMEOUT = -7


def decode(b):
    """Short description of a 16-byte Nios packet."""
    if len(b) < 16:
        return "(%d bytes)" % len(b)
    magic = chr(b[0]) if 32 <= b[0] < 127 else "?"
    if magic == "E":    # 16x64: target, flags, addr (16), data (64)
        target, flags = b[1], b[2]
        addr = b[4] | (b[5] << 8)
        data = int.from_bytes(bytes(b[6:14]), "little")
        rw = "W" if flags & 1 else "R"
        ok = " ok" if flags & 2 else ""
        if target == 1:
            cmd = RFIC_CMDS.get(addr & 0xFF, "cmd%d" % (addr & 0xFF))
            ch = (addr >> 8) & 0xF
            what = "RFIC %s ch%d" % (cmd, ch)
            if cmd == "STATUS":
                return "%s %s%s data=0x%x (init=%d wqlen=%d)" % (
                    rw, what, ok, data, data & 1, (data >> 8) & 0xFF)
        else:
            what = "16x64 t%d a0x%x" % (target, addr)
        return "%s %s%s data=%d" % (rw, what, ok, data)
    return "%s-pkt %s" % (magic, " ".join("%02x" % x for x in b[:8]))


def key(b):
    """What a response must echo from its request."""
    if len(b) < 16:
        return None
    if b[0] == ord("E"):
        return (b[0], b[1], b[4], b[5])
    return (b[0],)      # other formats' responses do not echo byte 1


def main():
    path = sys.argv[1]
    max_eps = int(sys.argv[2]) if len(sys.argv) > 2 else 6
    ev = []         # ("U", t, d, ep, n, rc, bytes) or ("B", t, d, op, ch, req, act, rc)
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "U":
                ev.append(("U", float(p[1]), float(p[2]), int(p[3], 16),
                           int(p[5]), int(p[6]), [int(x, 16) for x in p[7:]]))
            elif p[0] == "B":
                ev.append(("B", float(p[1]), float(p[2]), p[3], int(p[4]),
                           int(p[5]), int(p[6]), int(p[7])))

    # Pair requests with the response read after them
    last_req = None
    n_req = n_in = n_tmo = n_mismatch = 0
    pkt_notes = {}      # event index -> note
    for i, e in enumerate(ev):
        if e[0] != "U":
            continue
        _, t, d, ep, n, rc, b = e
        if ep == 0x02:
            n_req += 1
            last_req = b
        elif ep == 0x82:
            n_in += 1
            if rc == LIBUSB_TIMEOUT:
                n_tmo += 1
                pkt_notes[i] = "TIMEOUT"
            elif last_req is not None and key(b) != key(last_req):
                n_mismatch += 1
                pkt_notes[i] = "MISMATCH (reply to another request)"

    print("trace %s" % path)
    print("  requests %d, responses read %d, IN time-outs %d, mismatched responses %d"
          % (n_req, n_in, n_tmo, n_mismatch))

    # bandwidth set/get calls: find set calls whose actual != next get readback
    sets = []           # (index, t, d, ch, req, act, rc)
    fails = []
    for i, e in enumerate(ev):
        if e[0] == "B" and e[3] == "set":
            sets.append((i,) + e[1:3] + e[4:])
    for k, s in enumerate(sets):
        i, t, d, ch, req, act, rc = s
        # the readback is the next "get" on the same channel
        for e in ev[i + 1:]:
            if e[0] == "B" and e[3] == "get" and e[4] == ch:
                if e[6] != act or rc != 0:
                    fails.append((s, e))
                break
            if e[0] == "B" and e[3] == "set":
                break

    # timing: set_bandwidth durations per channel, by requested bandwidth
    print("  set_bandwidth calls %d, readback mismatches %d" % (len(sets), len(fails)))
    by = collections.defaultdict(list)
    tmo_sets = collections.Counter()
    for s in sets:
        i, t, d, ch, req, act, rc = s
        band = "<38.8M" if req < 38.8e6 else ">=38.8M"
        by[(ch, band)].append(d / 1000.0)
    for (ch, band), v in sorted(by.items()):
        v.sort()
        print("    ch%d %-8s n=%5d  median %7.1f ms  p90 %7.1f  max %7.1f  >=250 ms: %d"
              % (ch, band, len(v), statistics.median(v), v[int(0.9 * (len(v) - 1))],
                 v[-1], sum(1 for x in v if x >= 250)))

    # time-outs per set call, by requested bandwidth
    j = 0
    set_of_event = {}
    for s in sets:
        i, t, d, ch, req, act, rc = s
        set_of_event[i] = s
    cur = None
    tmo_bw = []
    for i, e in enumerate(ev):
        if e[0] == "U" and pkt_notes.get(i) == "TIMEOUT":
            # find the set call that contains this time
            for s in sets:
                if s[1] <= e[1] <= s[1] + s[2]:
                    tmo_bw.append((s[3], s[4]))
                    break
            else:
                tmo_bw.append((None, None))
    if tmo_bw:
        c = collections.Counter((ch, "<38.8M" if (req or 0) < 38.8e6 else ">=38.8M")
                                for ch, req in tmo_bw)
        print("  IN time-outs inside set_bandwidth calls (channel, band): %s" % dict(c))
        reqs = sorted(r for ch, r in tmo_bw if r is not None)
        if reqs:
            print("  requested bandwidths with a time-out: min %.3f MHz, max %.3f MHz"
                  % (reqs[0] / 1e6, reqs[-1] / 1e6))

    # print the packet sequence of the first few failures
    for n, (s, g) in enumerate(fails[:max_eps]):
        i, t, d, ch, req, act, rc = s
        print("\n  failure %d: set ch%d %u -> actual %d (rc %d, %.1f ms), readback %d"
              % (n + 1, ch, req, act, rc, d / 1000, g[6]))
        # from the previous set call up to the readback
        k = sets.index(s)
        start_t = sets[k - 1][1] if k > 0 else t
        for m, e in enumerate(ev):
            if e[1] < start_t - 1:
                continue
            if e[1] > g[1] + g[2] + 1:
                break
            if e[0] == "U":
                _, et, ed, ep, en, erc, b = e
                print("    %12.1f %8.1f us  %s rc=%d  %s  %s" % (
                    et, ed, "OUT" if ep == 2 else "IN ", erc,
                    decode(b) if en else "", pkt_notes.get(m, "")))
            else:
                print("    %12.1f %8.1f us  API %s ch%d req=%d -> %d rc=%d" % (
                    e[1], e[2], e[3], e[4], e[5], e[6], e[7]))


if __name__ == "__main__":
    main()
