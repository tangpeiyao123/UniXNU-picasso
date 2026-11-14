#!/usr/bin/env python3
"""Per-session timing anchors, all read from the log's own host stamps.

    python session_timing.py --log <file> [--log <file> ...]

Columns: banner index, banner host time, the CMD SENT that preceded it (what
ended the previous session), the gap that closed inside it (if any), the span
between banner and the last host stamp in the segment, and the line count.
Exit codes: 0 ran; 3 no sessions.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_desktop_reach as m


def fmt(t):
    return t.strftime("%H:%M:%S") if t else "-"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", required=True)
    args = ap.parse_args()

    print("%-6s %-9s %-9s %-9s %7s %7s" %
          ("sess", "banner", "prev_cmd", "gap_end", "span_s", "lines"))
    total = 0
    for path in args.log:
        sessions, lines = m.load_sessions([path])
        if not sessions:
            print("NO_SESSION %s" % path)
            continue
        print("# %s" % os.path.basename(path))
        # running "last command sent before this point", so a session reports
        # the command that ended the PREVIOUS one (which is what idle time
        # means); per-segment search would always look after the banner.
        last_cmd = None
        pos = 0
        for idx, seg, start in sessions:
            while pos < start:
                if "CMD SENT" in lines[pos]:
                    t = m.host_time(lines[pos])
                    if t:
                        last_cmd = t
                pos += 1
            total += 1
            banner = m.banner_epoch(None, lines, start)
            prev = last_cmd
            gaps = [m.host_time(l) for l in seg if "CAPTURE GAP END" in l]
            gaps = [g for g in gaps if g]
            stamps = [m.host_time(l) for l in seg if "host=" in l]
            stamps = [s for s in stamps if s]
            span = (max(stamps) - banner).total_seconds() if (banner and stamps) else None
            print("%-6d %-9s %-9s %-9s %7s %7d" %
                  (idx, fmt(banner), fmt(prev), fmt(gaps[0] if gaps else None),
                   ("%.0f" % span) if span is not None else "-", len(seg)))
    print("\nsessions=%d  RC=0" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
