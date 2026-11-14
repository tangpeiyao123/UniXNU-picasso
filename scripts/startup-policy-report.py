#!/usr/bin/env python3
"""Compare two restart policies on one console log.

A and B are banner-index ranges of the same log (the A/B harness writes both
into one file), so the only difference between the groups is how long the
harness idled before rebooting. Prints the class table and the Fisher exact
tests; writes nothing.

    python startup-policy-report.py --log <file> --a 2-31 --b 33-47

Exit codes: 0 ran; 2 bad range; 3 no sessions.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_desktop_reach as m
from boot_state_transitions import fisher_two_sided


def parse_range(text):
    a, _, b = text.partition("-")
    lo, hi = int(a), int(b or a)
    if lo > hi:
        raise ValueError(text)
    return lo, hi


def tally(group):
    c = collections.Counter()
    for r in group:
        c[r["verdict"]] += 1
        if r["verdict"] == "STALL":
            c["S_" + r["subtype"].replace("STALL_", "")] += 1
    return c


def show(name, group):
    c = tally(group)
    n = len(group)
    valid = n - c["TRUNC"]
    fp = sum(1 for r in group if r["verdict_frame"] == "PASS")
    armed = sum(1 for r in group if r["verdict"] == "PASS")
    print("%s  n=%d valid=%d  armed=%d  frame=%d  SEED=%d  NO_DRAWABLE=%d  "
          "AFTER_DRAWABLE=%d  NO_GL=%d  SILENT=%d  TRUNC=%d"
          % (name, n, valid, armed, fp, c["S_SEED"], c["S_NO_DRAWABLE"],
             c["S_AFTER_DRAWABLE"], c["S_NO_GL"], c["S_SILENT"], c["TRUNC"]))
    stalls = [r for r in group if r["verdict"] == "STALL"]
    if stalls:
        agx = [sum(1 for l in r["seg"] if "provider=AGXAccelerator" in l) for r in stalls]
        texk = [sum(1 for l in r["seg"] if "texk IOSurface attached" in l) for r in stalls]
        fbuc = [max([int(x) for x in re.findall(r"PICASSO_FBUC call=(\d+)", "\n".join(r["seg"]))] or [0])
                for r in stalls]
        print("   stalls: texk=%s agx=%s fbuc_max=%d" % (texk, agx, max(fbuc)))
    return dict(n=valid, frame=fp, seed=c["S_SEED"], nd=c["S_NO_DRAWABLE"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--a", default="2-31")
    ap.add_argument("--b", default="33-47")
    ap.add_argument("--window", type=float, default=300.0)
    ap.add_argument("--min-frames", type=int, default=2)
    args = ap.parse_args()

    try:
        a = parse_range(args.a)
        b = parse_range(args.b)
    except ValueError:
        print("bad range")
        return 2

    sessions, lines = m.load_sessions([args.log])
    if not sessions:
        print("NO_SESSION")
        return 3
    rows = m.measure(sessions, lines, args.window, args.min_frames)

    ga = [r for r in rows if a[0] <= r["n"] <= a[1]]
    gb = [r for r in rows if b[0] <= r["n"] <= b[1]]
    print("A(%s) idles 5 s" % args.a)
    ra = show("A", ga)
    print("B(%s) idles 300 s" % args.b)
    rb = show("B", gb)

    print("\nFisher two-sided (valid sessions only, TRUNC excluded):")
    print("  frame-PASS  A %d/%d  B %d/%d  p=%.3f" %
          (ra["frame"], ra["n"], rb["frame"], rb["n"],
           fisher_two_sided(ra["frame"], ra["n"] - ra["frame"],
                            rb["frame"], rb["n"] - rb["frame"])))
    print("  SEED        A %d/%d  B %d/%d  p=%.3f" %
          (ra["seed"], ra["n"], rb["seed"], rb["n"],
           fisher_two_sided(ra["seed"], ra["n"] - ra["seed"],
                            rb["seed"], rb["n"] - rb["seed"])))
    print("  NO_DRAWABLE A %d/%d  B %d/%d  p=%.3f" %
          (ra["nd"], ra["n"], rb["nd"], rb["n"],
           fisher_two_sided(ra["nd"], ra["n"] - ra["nd"],
                            rb["nd"], rb["n"] - rb["nd"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
