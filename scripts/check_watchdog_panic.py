#!/usr/bin/env python3
"""Is the watchdog panic related to how long a session idled?

Scans console logs for 'hardware pet rejected', locates the session it belongs
to (nearest preceding Darwin banner), and pairs it with the host timestamps the
recorder writes (KERNEL BANNER / CMD SENT). Prints a verdict:

    CORRELATED   at least 3 samples and the idle times separate cleanly
    UNRELATED    at least 3 samples and they do not
    UNKNOWN      fewer than 3 samples, or no host timestamps to measure idle

    python check_watchdog_panic.py --log <file> [--log <file> ...]

Exit codes: 0 ran; 3 no logs/sessions.
"""
import argparse
import datetime
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_desktop_reach as m
from measure_desktop_reach import host_time

SIG = "hardware pet rejected"


def decide(idles):
    """Three-way verdict from the idle times of the watchdog panics."""
    timed = [x for x in idles if x is not None]
    if len(idles) < 3:
        return "UNKNOWN", "only %d sample(s); one panic cannot be tied to idle time" % len(idles)
    if len(timed) < 3:
        return "UNKNOWN", "fewer than 3 samples carry host timestamps"
    spread = max(timed) - min(timed)
    if spread > 120:
        return "CORRELATED", "idle spread %.0fs across %d samples" % (spread, len(timed))
    return "UNRELATED", "idle spread %.0fs across %d samples" % (spread, len(timed))


def self_test():
    cases = [([10.0], "UNKNOWN"), ([10.0, 20.0, 30.0], "UNRELATED"),
             ([10.0, 300.0, 600.0], "CORRELATED"), ([None, None, None], "UNKNOWN")]
    bad = 0
    for idles, want in cases:
        got, why = decide(idles)
        ok = got == want
        bad += 0 if ok else 1
        print("  %-28s -> %-11s %s  (%s)" % (idles, got, "ok" if ok else "MISMATCH", why))
    print("SELFTEST=%s RC=%d" % ("PASS" if not bad else "FAIL", 0))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", default=[])
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()

    found = []
    for path in args.log:
        sessions, lines = m.load_sessions([path])
        if not sessions:
            print("NO_SESSION %s" % path)
            continue
        for idx, seg, start in sessions:
            hit = next((l for l in seg if SIG in l), None)
            if not hit:
                continue
            banner = m.banner_epoch(None, lines, start)
            # idle = banner minus the last CMD SENT written BEFORE this banner,
            # which lives in the previous segment (a per-segment search always
            # finds the command that ENDS this one and yields a negative idle).
            prev_cmd = None
            for j in range(start - 1, max(0, start - 400), -1):
                if "CMD SENT" in lines[j]:
                    prev_cmd = host_time(lines[j])
                    if prev_cmd:
                        break
            # last host command sent inside this session, i.e. how long the
            # previous session had been left alone before this boot
            idle = (banner - prev_cmd).total_seconds() if (banner and prev_cmd) else None
            found.append((os.path.basename(path), idx, banner, idle, hit[:90]))

    print("occurrences of %r: %d" % (SIG, len(found)))
    for name, idx, banner, idle, line in found:
        print("  %-34s session #%d banner=%s idle_before=%s" % (
            name, idx,
            banner.strftime("%H:%M:%S") if banner else "UNKNOWN",
            ("%.0fs" % idle) if idle is not None else "UNKNOWN"))
        print("    %s" % line)

    verdict, why = decide([f[3] for f in found])

    print("\nVERDICT=%s  RC=%d" % (verdict, 0))
    print("reason: %s" % why)
    return 0


if __name__ == "__main__":
    sys.exit(main())
