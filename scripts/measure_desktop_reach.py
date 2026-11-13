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
    frames    number of 'PICASSO_DPU_FRAME' lines (one per 30 flips)
    eagl      'eagl init gl=' hits: the GL client created a context
    texk      'texk IOSurface attached' hits: the first drawable was bound

Verdict: PASS = vsync >= 17 and desktop > 0; otherwise STALL.
A session the host rebooted before the judgement window (`CMD SENT ...
reboot-fastboot` within --window seconds of its banner) never had a chance to
reach a verdict, so it is reported as TRUNC and excluded from the valid rate.
Its own numbers stay in the table.
"""
import argparse
import datetime
import os
import re
import sys

BANNER = "Darwin Kernel Version"
DESKTOP_MARKERS = ("backboardd", "SpringBoard", "WindowServer", "frontboard")
HOST_STAMP = re.compile(r"host=(\S+)")


def host_time(line):
    m = HOST_STAMP.search(line)
    if not m:
        return None
    try:
        return datetime.datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%S.%f")
    except ValueError:
        return None


def banner_epoch(seg_before, lines, start):
    """Host time of the KERNEL BANNER marker that announced this session."""
    for j in range(start - 1, max(0, start - 40), -1):
        if "KERNEL BANNER" in lines[j]:
            return host_time(lines[j])
    return None


def reboot_after_banner(seg):
    """Host time of the last 'reboot-fastboot' this session was sent."""
    last = None
    for line in seg:
        if "CMD SENT" in line and "reboot-fastboot" in line:
            t = host_time(line)
            if t:
                last = t
    return last


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
    # startswith, not `in`: a panic log echoes "Kernel version: Darwin Kernel
    # Version ..." on its own line, and matching the substring counts each of
    # those as a fresh session (34 real banners vs 43 substring hits in the
    # 2026-11-13 log). Anchor to the line start so only a real boot counts.
    starts = [i for i, l in enumerate(lines) if l.startswith(BANNER)]
    if not starts:
        return [], lines
    sessions = []
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else len(lines)
        sessions.append((k + 1, lines[s:e], s))
    return sessions, lines


def stall_subtype(seg):
    """Classify a STALL session by the stage it actually reached.

    The drawable attach is the first point where the render path has a surface
    to draw into: sessions that print it go on to submit frames, sessions that
    do not never render (11 sessions of 2026-11-13: 3 attached -> all PASS,
    8 did not -> all STALL). Report it instead of guessing "GPU stall"."""
    body = "\n".join(seg)
    pan = next((l for l in seg if l.startswith("panic(")), None)
    if pan:
        return "STALL_SEED", pan
    if "iBoot didn't extract panic log" in body:
        return "STALL_CRASH_RECOVERY", \
            "iBoot didn't extract panic log from previous session crash"
    if "texk IOSurface attached" in body:
        return "STALL_AFTER_DRAWABLE", \
            "drawable attached but no frame reached the display"
    if "eagl init gl=" in body:
        return "STALL_NO_DRAWABLE", \
            "EAGL context created, first IOSurface was never attached"
    desktop = sum(body.count(m) for m in DESKTOP_MARKERS)
    if desktop > 0:
        return "STALL_NO_GL", "UI markers but the GL client never started"
    return "STALL_SILENT", "no UI marker, no panic: boot stopped before SpringBoard"


def measure(sessions, all_lines, window=300.0):
    rows = []
    for idx, seg, start in sessions:
        body = "\n".join(seg)
        vsync = body.count("vsync")
        kick = body.count("vsync: kick")
        desktop = sum(body.count(m) for m in DESKTOP_MARKERS)
        panic = sum(1 for l in seg if l.startswith("panic("))
        frames = body.count("PICASSO_DPU_FRAME")
        eagl = body.count("eagl init gl=")
        texk = body.count("texk IOSurface attached")
        verdict = "PASS" if (vsync >= 17 and desktop > 0) else "STALL"
        # A reboot the host sent inside the judgement window ended this boot
        # before it could reach a verdict: that is not evidence about the boot.
        boot_t = banner_epoch(None, all_lines, start)
        rb_t = reboot_after_banner(seg)
        age = (rb_t - boot_t).total_seconds() if (boot_t and rb_t) else None
        truncated = bool(verdict == "STALL" and age is not None and age < window)
        last = next((l for l in reversed(seg) if l.strip()), "")
        subtype = detail = ""
        if verdict == "STALL":
            subtype, detail = stall_subtype(seg)
        if truncated:
            verdict = "TRUNC"
            subtype = "TRUNCATED_BY_HOST"
            detail = "host reboot %.0f s after the banner, before the %.0f s window" % (
                age, window)
        elif age is not None:
            detail = (detail + " | " if detail else "") + "rebooted at %.0f s" % age
        rows.append(dict(n=idx, vsync=vsync, kick=kick, desktop=desktop,
                         panic=panic, frames=frames, eagl=eagl, texk=texk,
                         verdict=verdict, last=last[:110], age=age,
                         subtype=subtype, detail=detail, seg=seg))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", action="append", required=True,
                    help="console log path; repeat for several files (ordered)")
    ap.add_argument("--require", type=int, default=0,
                    help="require exactly N PASS sessions, exit 4 if not met")
    ap.add_argument("--stall-dir", default="logs",
                    help="directory to dump each STALL session (default logs/)")
    ap.add_argument("--window", type=float, default=300.0,
                    help="judgement window: a host reboot earlier than this after "
                         "the banner marks the session TRUNC (default 300 s)")
    args = ap.parse_args()

    sessions, lines = load_sessions(args.log)
    if not sessions:
        print("NO_SESSION: no Darwin banner in %s" % args.log)
        return 3

    rows = measure(sessions, lines, args.window)
    n_pass = sum(1 for r in rows if r["verdict"] == "PASS")
    n_trunc = sum(1 for r in rows if r["verdict"] == "TRUNC")
    n_stall = len(rows) - n_pass - n_trunc

    print("sess  vsync  kick  desktop  panic  frames  eagl  texk  verdict  subtype")
    for r in rows:
        print("%4d  %5d  %4d  %7d  %5d  %6d  %4d  %4d  %-6s  %s" %
              (r["n"], r["vsync"], r["kick"], r["desktop"], r["panic"],
               r["frames"], r["eagl"], r["texk"], r["verdict"],
               r["subtype"] or "-"))

    print("\nTOTAL %d sessions  PASS %d  STALL %d  TRUNC %d  pass-rate %.1f%%"
          % (len(rows), n_pass, n_stall, n_trunc, 100.0 * n_pass / len(rows)))
    valid = len(rows) - n_trunc
    if n_trunc:
        print("VALID %d sessions (TRUNC excluded)  PASS %d  STALL %d  pass-rate %.1f%%"
              % (valid, n_pass, n_stall, 100.0 * n_pass / valid))
    print("PASS = vsync>=17 and desktop>0 (display armed + UI up). It is NOT a"
          " frame count: PASS sessions that were rebooted early show frames=0.")
    n_frames = sum(1 for r in rows if r["frames"])
    print("sessions with >=1 DPU_FRAME: %d/%d" % (n_frames, len(rows)))

    # Dump every STALL session and print its diagnostic, so the failure can be
    # split into repairable root causes instead of one "STALL" bucket.
    os.makedirs(args.stall_dir, exist_ok=True)
    from collections import Counter
    subtype_count = Counter()
    for r in rows:
        if r["verdict"] == "TRUNC":
            print("  TRUNC #%d [%s] %s" % (r["n"], r["subtype"], r["detail"]))
        if r["verdict"] == "STALL":
            subtype_count[r["subtype"]] += 1
            fn = os.path.join(args.stall_dir, "stall-%02d.txt" % r["n"])
            with open(fn, "w", encoding="utf-8") as f:
                f.write("\n".join(r["seg"]))
            print("  STALL #%d [%s] %s -> %s"
                  % (r["n"], r["subtype"], r["detail"][:80], fn))

    print("\nsubtype histogram:")
    for k, v in subtype_count.most_common():
        print("  %-24s %d" % (k, v))

    if args.require and n_pass != args.require:
        print("REQUIRE FAIL: expected %d PASS, got %d" % (args.require, n_pass))
        return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
