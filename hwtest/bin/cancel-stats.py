#!/usr/bin/env python3
"""cancel-stats.py <cancel.log>...: the libusb_cancel_transfer() calls that lib/cancel-time.c
logged, grouped into rounds (one process; at most 200 ms from the end of one call to the start
of the next). A stop's cancel_all_transfers() is one round. TESTING.md section 23; ran as
gapfix/stophang/cancelstats.py on the XPS."""
import sys
import time
from collections import Counter, defaultdict

GAP_US = 200000


def rounds_of(calls):
    out = []
    for start, dur in sorted(calls):
        if out and start - (out[-1][-1][0] + out[-1][-1][1]) <= GAP_US:
            out[-1].append((start, dur))
        else:
            out.append([(start, dur)])
    return out


def span(r):
    return r[-1][0] + r[-1][1] - r[0][0]


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else 0


for path in sys.argv[1:]:
    by_pid = defaultdict(list)
    for line in open(path, errors='replace'):
        f = line.split()
        if len(f) == 6 and f[0] == 'C':
            by_pid[f[1]].append((int(f[3]), int(f[4])))
    calls = [c for v in by_pid.values() for c in v]
    durs = [d for _, d in calls]
    rounds = [r for v in by_pid.values() for r in rounds_of(v)]
    print(f'{path.rsplit("/", 1)[-1]}: {len(calls)} calls, {len(by_pid)} processes, '
          f'{len(rounds)} rounds; per call: median {pct(durs, .5)} us, 99th percentile '
          f'{pct(durs, .99)} us, max {max(durs) if durs else 0} us')
    bins = [(0, 1e4, '<10 ms'), (1e4, 1e5, '10-100 ms'), (1e5, 1e6, '0.1-1 s'),
            (1e6, 3e6, '1-3 s'), (3e6, float('inf'), '>=3 s')]
    print('  rounds by length: ' + ', '.join(
        f'{name} {sum(1 for r in rounds if lo <= span(r) < hi)}' for lo, hi, name in bins))
    print('  calls per round: ' + ', '.join(
        f'{k}: {v}' for k, v in sorted(Counter(len(r) for r in rounds).items())))
    for r in sorted(rounds, key=span, reverse=True)[:10]:
        if span(r) < 1e5:
            break
        ds = [d for _, d in r]
        print(f'  round at {time.strftime("%H:%M:%S", time.localtime(r[0][0] / 1e6))}: '
              f'{len(r)} calls in {span(r) / 1e3:.0f} ms; per call median {pct(ds, .5) / 1e3:.1f} '
              f'ms, max {max(ds) / 1e3:.1f} ms; {sum(1 for d in ds if d > 1e4)} calls over 10 ms')
