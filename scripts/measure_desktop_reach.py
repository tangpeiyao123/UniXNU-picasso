#!/usr/bin/env python3
"""Desktop-reachability measurement over serial boot logs.

Splits one or more console logs on the Darwin banner line, then for every
session reports four counters and a PASS/STALL verdict. The verdict is the one
validated on 2026-11-13: every session with vsync >= 17 (a fully armed display
pipeline) also had desktop markers, and every session with vsync <= 2 stalled
before a frame. Read-only; it only parses the log files.

    python measure_desktop_reach.py --log <file> [--log <file> ...] [--require N]

Exit codes: 0 ran and printed the table; 3 no session found; 4 a --require
condition was not met (used by the 5-reboot reproduction loop).

Counters per session:
    vsync     number of 'vsync' substring hits (17 = pipeline armed, 8 of them
              are the 'vsync: kick' prints)
    kick      number of 'vsync: kick' hits (8 = window server submitted frames)
    desktop   sum of backboardd / SpringBoard / WindowServer / frontboard hits
    panic     number of lines starting with 'panic('

Verdict: PASS = vsync >= 17 and desktop > 0; otherwise STALL.
"""
import argparse
import os
import re
import sys

BANNER = "Darwin Kernel Version"
DESKTOP_MARKERS = ("backboardd", "SpringBoard", "WindowServer", "frontboard")


def load_sessions(paths):
    """Concatenate the files in order and split on the banner line.

    A banner in a middle file that continues the previous file's session is not
    deduplicated: the recorder never splits a boot across files in practice, and
    the reproduction test feeds one file at a time. Returns a list of (index,
    list-of-lines)."""
    lines = []
    for p in paths:
        if not os.path.exists(p):
            print("MISSING %s" % p, file=sys.stderr)
            return None
        lines.extend(open(p, encoding="utf-8", errors="replace").read().splitlines())
    starts = [i for i, l in enumerate(lines) if l.startswith(BANNER)]
    if not starts:
        return []
    sessions = []
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else len(lines)
        sessions.append((k + 1, lines[s:e]))
    return sessions


def measure(sessions):
    rows = []
    for idx, seg in sessions:
        body = "\n".join(seg)
        vsync = body.count("vsync")
        kick = body.count("vsync: kick")
        desktop = sum(body.count(m) for m in DESKTOP_MARKERS)
        panic = sum(1 for l in seg if l.startswith("panic("))
        verdict = "PASS" if (vsync >= 17 and desktop > 0) else "STALL"
        last = next((l for l in reversed(seg) if l.strip()), "")
        rows.append(dict(n=idx, vsync=vsync, kick=kick, desktop=desktop,
                         panic=panic, verdict=verdict, last=last[:110]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", required=True,
                    help="console log path; repeat for several files (ordered)")
    ap.add_argument("--require", type=int, default=0,
                    help="require exactly N PASS sessions, exit 4 if not met")
    args = ap.parse_args()

    sessions = load_sessions(args.log)
    if sessions is None:
        return 3
    if not sessions:
        print("NO_SESSION: no Darwin banner in %s" % args.log)
        return 3

    rows = measure(sessions)
    n_pass = sum(1 for r in rows if r["verdict"] == "PASS")
    n_stall = len(rows) - n_pass

    print("sess  vsync  kick  desktop  panic  verdict")
    for r in rows:
        print("%4d  %5d  %4d  %7d  %5d  %-6s" %
              (r["n"], r["vsync"], r["kick"], r["desktop"], r["panic"], r["verdict"]))

    print("\nTOTAL %d sessions  PASS %d  STALL %d  pass-rate %.1f%%"
          % (len(rows), n_pass, n_stall, 100.0 * n_pass / len(rows)))

    # For the reproduction test: show where each STALL stopped.
    for r in rows:
        if r["verdict"] == "STALL":
            print("  STALL #%d last-line: %s" % (r["n"], r["last"]))

    if args.require and n_pass != args.require:
        print("REQUIRE FAIL: expected %d PASS, got %d" % (args.require, n_pass))
        return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
