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

SIG = "hardware pet rejected"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", required=True)
    args = ap.parse_args()

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
            # last host command sent inside this session, i.e. how long the
            # previous session had been left alone before this boot
            cmds = [m.host_time(l) for l in seg if "CMD SENT" in l]
            # only commands before this banner describe the previous session
            cmds = [c for c in cmds if c and banner and c < banner]
            idle = (banner - cmds[-1]).total_seconds() if (banner and cmds) else None
            found.append((os.path.basename(path), idx, banner, idle, hit[:90]))

    print("occurrences of %r: %d" % (SIG, len(found)))
    for name, idx, banner, idle, line in found:
        print("  %-34s session #%d banner=%s idle_before=%s" % (
            name, idx,
            banner.strftime("%H:%M:%S") if banner else "UNKNOWN",
            ("%.0fs" % idle) if idle is not None else "UNKNOWN"))
        print("    %s" % line)

    timed = [f for f in found if f[3] is not None]
    if len(found) < 3:
        verdict = "UNKNOWN"
        why = "only %d sample(s); a single watchdog panic cannot be tied to idle time" % len(found)
    elif len(timed) < 3:
        verdict = "UNKNOWN"
        why = "fewer than 3 samples carry host timestamps"
    else:
        idles = sorted(f[3] for f in timed)
        spread = idles[-1] - idles[0]
        verdict = "CORRELATED" if spread > 120 else "UNRELATED"
        why = "idle spread %.0fs across %d samples" % (spread, len(timed))

    print("\nVERDICT=%s  RC=%d" % (verdict, 0))
    print("reason: %s" % why)
    return 0


if __name__ == "__main__":
    sys.exit(main())
