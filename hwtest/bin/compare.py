#!/usr/bin/env python3
"""Compare two or more run-suite result directories (baseline first).

  compare.py <baseline_dir> <candidate_dir> [more candidate dirs...]

Each directory holds summary.jsonl ({"test", "rc", "seconds"} per test) and
optionally rf.jsonl (rf-check.py output). Prints a table per candidate and
flags regressions: a test that passes in the baseline but fails in the
candidate, a test that became much slower, or RF measurements outside the
tolerances below. Exit status 1 if anything is flagged.
"""
import json
import os
import sys

# RF tolerances (candidate minus baseline). The two images run on the same
# board and cabling, so differences beyond run-to-run noise point at the image.
TOL = {
    "tone_dbfs": 1.5,      # dB
    "image_dbc": 6.0,      # dB (image rejection depends on calibration)
    "dc_dbc": 6.0,         # dB (LO leakage depends on calibration)
    "floor_dbfs": 3.0,     # dB
    "freq_err_ppm": 1.0,   # ppm of the center frequency
}
SLOWER = 1.5               # flag tests taking >1.5x the baseline time (and >2 s)


def load_jsonl(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


def tests(d):
    return {t["test"]: t for t in load_jsonl(os.path.join(d, "summary.jsonl"))}


def rf(d):
    out = {}
    for m in load_jsonl(os.path.join(d, "rf.jsonl")):
        out.setdefault((m.get("direction"), m.get("fc")), []).append(m)
    return out


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def tx_mode(d):
    try:
        with open(os.path.join(d, "meta.json")) as f:
            return bool(json.load(f).get("tx", True))   # older runs: TX mode
    except (OSError, ValueError):
        return None


def compare(base, cand):
    flags = []
    tb, tc = tests(base), tests(cand)
    print("\n=== %s  vs  %s" % (base, cand))
    if tx_mode(base) != tx_mode(cand):
        # Receive-only runs leave out the TX tests: compare like with like
        print("WARNING: TX mode differs (baseline tx=%s, candidate tx=%s)"
              % (tx_mode(base), tx_mode(cand)))
        flags.append(("meta", "TX-MODE-DIFFERS"))
    print("%-34s %10s %10s" % ("test", "baseline", "candidate"))
    for name in sorted(set(tb) | set(tc)):
        b, c = tb.get(name), tc.get(name)
        bs = "rc=%d %.0fs" % (b["rc"], b["seconds"]) if b else "-"
        cs = "rc=%d %.0fs" % (c["rc"], c["seconds"]) if c else "-"
        note = ""
        if b and c:
            if b["rc"] == 0 and c["rc"] != 0:
                note = "REGRESSION"
            elif c["seconds"] > 2 and c["seconds"] > SLOWER * max(b["seconds"], 0.1):
                note = "SLOWER"
        elif b and not c:
            note = "MISSING"
        if note:
            flags.append((name, note))
        print("%-34s %10s %10s  %s" % (name, bs, cs, note))

    rb, rc = rf(base), rf(cand)
    if rb or rc:
        print("\n%-11s %10s %9s %9s %9s %9s %9s" %
              ("rf", "fc MHz", "dTone dB", "dImg dB", "dDC dB", "dFlr dB", "dF ppm"))
    for key in sorted(set(rb) | set(rc), key=lambda k: (str(k[0]), k[1] or 0)):
        b, c = rb.get(key, []), rc.get(key, [])
        if not b or not c or any("error" in m for m in c):
            note = "MISSING/ERROR" if (b and not any("error" in m for m in b)) else ""
            print("%-11s %10.2f  %s" % (key[0], (key[1] or 0) / 1e6, note or "n/a"))
            if note:
                flags.append((key, note))
            continue
        fc = key[1]
        d = {}
        for k in ("tone_dbfs", "image_dbc", "dc_dbc", "floor_dbfs"):
            d[k] = mean([m[k] for m in c]) - mean([m[k] for m in b])
        d["freq_err_ppm"] = (mean([m["freq_err_hz"] for m in c]) -
                             mean([m["freq_err_hz"] for m in b])) / fc * 1e6
        bad = [k for k in d if abs(d[k]) > TOL[k]]
        if bad:
            flags.append((key, ",".join(bad)))
        print("%-11s %10.2f %9.2f %9.2f %9.2f %9.2f %9.3f  %s" %
              (key[0], fc / 1e6, d["tone_dbfs"], d["image_dbc"], d["dc_dbc"],
               d["floor_dbfs"], d["freq_err_ppm"], " ".join(bad)))
    return flags


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    allflags = []
    for cand in sys.argv[2:]:
        allflags += compare(sys.argv[1], cand)
    print("\n%d flag(s)" % len(allflags))
    for f in allflags:
        print("  ", f)
    return 1 if allflags else 0


if __name__ == "__main__":
    sys.exit(main())
