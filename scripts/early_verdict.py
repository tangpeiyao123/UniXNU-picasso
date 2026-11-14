#!/usr/bin/env python3
"""Can a boot be judged from its first N seconds?

The plan assumed each signal carries a timestamp inside the log line, so the
first 30/60/90/120 s could be replayed. Check that assumption before modelling
anything: for every session, count how many of the signal lines actually parse
to a time. If coverage is low the answer is NO_RELIABLE_EARLY_SIGNAL and the
experiment stays offline.

    python early_verdict.py --log <file> [--log <file> ...]

Exit codes: 0 ran (verdict on stdout); 3 no sessions.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_desktop_reach as m

SIGNALS = ("provider=AGXAccelerator", "PICASSO_FBUC call=", "vsync",
           "texk IOSurface attached", "eagl init ok", "PICASSO_DPU_FRAME")

# a line that starts with a clock, e.g. "12:03:44.123 " or "2026-11-14T12:03:44"
CLOCK = re.compile(r"^(\d{2}:\d{2}:\d{2}(\.\d+)?|\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", required=True)
    ap.add_argument("--windows", default="30,60,90,120")
    args = ap.parse_args()

    total = timed = 0
    per_signal = {s: [0, 0] for s in SIGNALS}   # [with timestamp, total]
    for path in args.log:
        sessions, lines = m.load_sessions([path])
        if not sessions:
            print("NO_SESSION %s" % path)
            continue
        for _, seg, _ in sessions:
            for line in seg:
                for s in SIGNALS:
                    if s in line:
                        per_signal[s][1] += 1
                        total += 1
                        if CLOCK.match(line):
                            per_signal[s][0] += 1
                            timed += 1

    print("signal lines: %d, of which carry a clock: %d (%.1f%%)"
          % (total, timed, 100.0 * timed / total if total else 0.0))
    for s, (t, n) in sorted(per_signal.items()):
        print("  %-26s %d/%d" % (s, t, n))

    if total == 0:
        print("VERDICT=NO_SESSIONS")
        return 3
    if timed * 2 < total:
        print("\nVERDICT=NO_RELIABLE_EARLY_SIGNAL")
        print("reason: the kernel prints no per-line clock, so 'first N seconds' "
              "cannot be reconstructed; every window would be a guess.")
        print("gate: accuracy/FP not computable -> instruction 4 stays offline")
        return 0

    print("\nVERDICT=TIMESTAMPS_AVAILABLE (windows %s)" % args.windows)
    print("note: modelling is not implemented; this gate only proves the input exists")
    return 0


if __name__ == "__main__":
    sys.exit(main())
