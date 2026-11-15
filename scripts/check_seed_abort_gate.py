#!/usr/bin/env python3
"""Gate for the SEED early abort, using the same online rule the harness runs.

Rule under test: take the FIRST NATIVE_OWNER_DATA line of a segment, look at the
next W lines only, and call it a seed stall when no DATA_INIT is in that window.
A segment shorter than di+W is not judged (online, we simply have not seen it).

The failure that matters is a FALSE KILL: a segment that would have reached a
frame-PASS (the product goal) being aborted. Aborting a segment that was going
to stall anyway costs nothing and saves time, so it is counted separately.

    python check_seed_abort_gate.py --log <file> [--log <file> ...] --window 200

Exit codes: 0 GATE=PASS (false kills 0 and no missed seed); 1 GATE=FAIL.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_desktop_reach as m


def fires(seg, window):
    di = next((k for k, line in enumerate(seg) if "NATIVE_OWNER_DATA" in line), None)
    if di is None or len(seg) <= di + window:
        return False
    return not any("DATA_INIT" in l for l in seg[di + 1: di + 1 + window])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", required=True)
    ap.add_argument("--window", type=int, default=200)
    args = ap.parse_args()

    fired = seed = falsekill = missed = other = 0
    for path in args.log:
        sessions, lines = m.load_sessions([path])
        if not sessions:
            print("NO_SESSION %s" % path)
            continue
        rows = m.measure(sessions, lines, 300.0, 2)
        f = s = fk = ms = ot = 0
        for r in rows:
            hit = fires(r["seg"], args.window)
            is_seed = r["subtype"] == "STALL_SEED"
            would_pass = r["verdict_frame"] == "PASS"
            if hit:
                f += 1
                if is_seed:
                    s += 1
                elif would_pass:
                    fk += 1
                else:
                    ot += 1
            elif is_seed:
                ms += 1
        print("%-38s segs=%3d fire=%2d true_seed=%2d false_kill=%d other_stall=%2d missed_seed=%d"
              % (os.path.basename(path), len(rows), f, s, fk, ot, ms))
        fired += f
        seed += s
        falsekill += fk
        other += ot
        missed += ms

    print("\nwindow=%d  fire=%d  true_seed=%d  false_kill=%d  other_stall=%d  missed_seed=%d"
          % (args.window, fired, seed, falsekill, other, missed))
    ok = falsekill == 0 and missed == 0
    print("GATE=%s  RC=%d" % ("PASS" if ok else "FAIL", 0 if ok else 1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
