#!/usr/bin/env python3
"""Controlled desktop-reachability reproduction over N power-cycles.

Watches the live console log. The operator power-cycles the phone (the
recorder keeps the log file growing); this script detects each new Darwin
banner and, for that session, waits for a verdict:

    PASS        vsync >= 17 and desktop > 0  (desktop reached, stop early)
    STALL_SEED  a line starts with 'panic('  (seed hang, auto-reboots)
    STALL_GPU   300 s with no frame and no panic (GPU stall, needs power cut)

The freeze rules are respected: this script never talks to the phone, never
flashes, never touches a serial port. It only reads the log file the recorder
writes.

Usage:
    python reproduce_desktop.py --log <file> --cycles 5 --window 300
      [--start-banner N]   # number of banners already in the file

Exit code: 0 ran to completion; 2 wrong argument; 3 no new banner in the wait
window (the operator did not power-cycle). The pass-rate is printed, never
encoded in the exit code.
"""
import argparse
import os
import re
import sys
import time

BANNER = "Darwin Kernel Version"
DESKTOP = ("backboardd", "SpringBoard", "WindowServer", "frontboard")


def count_banners(path):
    n = 0
    if os.path.exists(path):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith(BANNER):
                    n += 1
    return n


def read_session(path, index):
    """Return the lines of the index-th banner session (0-based)."""
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    starts = [i for i, l in enumerate(lines) if l.startswith(BANNER)]
    if index >= len(starts):
        return None
    s = starts[index]
    e = starts[index + 1] if index + 1 < len(starts) else len(lines)
    return lines[s:e]


def judge(seg, min_frames=0):
    """Verdict for one boot.

    min_frames counts PICASSO_DPU_FRAME lines, and the driver prints one line per
    30 flips, so min_frames=2 means >=60 flips. The default 0 keeps the old
    display-armed rule; a desktop claim needs --min-frames 2, because the armed
    state alone was measured on sessions that never rendered (2026-11-13)."""
    body = "\n".join(seg)
    vsync = body.count("vsync")
    desktop = sum(body.count(m) for m in DESKTOP)
    panic = sum(1 for l in seg if l.startswith("panic("))
    frames = body.count("PICASSO_DPU_FRAME")
    if vsync >= 17 and desktop > 0 and frames >= min_frames:
        return "PASS", vsync, desktop, panic
    if panic:
        return "STALL_SEED", vsync, desktop, panic
    return None, vsync, desktop, panic


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--cycles", type=int, default=5)
    ap.add_argument("--window", type=float, default=300.0)
    ap.add_argument("--start-banner", type=int, default=-1)
    ap.add_argument("--wait-cycle", type=float, default=900.0,
                    help="max seconds to wait for the next power-cycle before giving up")
    ap.add_argument("--cmd-file", default=None,
                    help="when set, PASS and STALL_GPU sessions are soft-rebooted by "
                         "writing 'reboot-fastboot' here (the recorder forwards it)")
    ap.add_argument("--min-frames", type=int, default=0,
                    help="extra PASS condition: PICASSO_DPU_FRAME lines seen (one per 30 "
                         "flips). 0 keeps the armed-only rule; use 2 for a desktop claim, "
                         "otherwise the run stops before the first frame is drawn")
    args = ap.parse_args()

    # One instance per command file. Two loops sharing it rebooted a boot that
    # was still in progress on 2026-11-13 22:04:38 and 22:07:14 (both commands
    # predate this script's 22:08:15 start), which cost two sessions.
    lock = None
    if args.cmd_file:
        lock_path = args.cmd_file + ".lock"
        try:
            if os.path.exists(lock_path) and time.time() - os.path.getmtime(lock_path) > 6 * 3600:
                os.remove(lock_path)          # stale lock from a killed run
            fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            lock = lock_path
        except FileExistsError:
            print("[%s] another reproduction owns %s (lock %s); refusing to start"
                  % (time.strftime("%H:%M:%S"), args.cmd_file, lock_path), flush=True)
            return 2

    def send_reboot():
        """Soft reboot via the kernel's CDC 'reboot-fastboot' command."""
        if not args.cmd_file:
            return
        with open(args.cmd_file, "w", encoding="utf-8") as f:
            f.write("reboot-fastboot\n")
        print("[%s]   -> sent reboot-fastboot" % time.strftime("%H:%M:%S"), flush=True)

    base = count_banners(args.log) if args.start_banner < 0 else args.start_banner
    print("[%s] reproduction start: %d banners already in file, %d cycles, window %.0fs"
          % (time.strftime("%H:%M:%S"), base, args.cycles, args.window), flush=True)

    results = []
    for cycle in range(1, args.cycles + 1):
        target = base + cycle          # wait for the next banner
        t0 = time.time()
        while count_banners(args.log) < target:
            if time.time() - t0 > args.wait_cycle:
                print("[%s] cycle %d: no new banner in %.0fs, aborting"
                      % (time.strftime("%H:%M:%S"), cycle, args.wait_cycle), flush=True)
                if lock:
                    os.remove(lock)
                return 3
            time.sleep(2)

        seg0 = read_session(args.log, target - 1)
        seg0 = seg0 if seg0 is not None else []
        print("[%s] cycle %d: session #%d started" %
              (time.strftime("%H:%M:%S"), cycle, target), flush=True)

        verdict = None
        t0 = time.time()
        while verdict is None:
            seg = read_session(args.log, target - 1)
            if seg is None:
                time.sleep(2)
                continue
            v, vsync, desktop, panic = judge(seg, args.min_frames)
            if v:
                verdict = v
                break
            if time.time() - t0 > args.window:
                verdict = "STALL_GPU"
                v, vsync, desktop, panic = judge(seg, args.min_frames)
                break
            time.sleep(3)

        results.append(verdict)
        print("[%s] cycle %d: %s  (vsync=%s desktop=%s panic=%s)"
              % (time.strftime("%H:%M:%S"), cycle, verdict, vsync, desktop, panic), flush=True)

        # STALL_SEED panics and reboots on its own. PASS and STALL_GPU need the
        # soft reboot; both respond because the USB poller keeps running even
        # while the GPU fence is frozen.
        if verdict in ("PASS", "STALL_GPU"):
            send_reboot()
            time.sleep(5)

    n_pass = sum(1 for r in results if r == "PASS")
    print("\n=== RESULT ===")
    for i, r in enumerate(results, 1):
        print("  cycle %d: %s" % (i, r))
    print("pass-rate %d/%d = %.1f%%" % (n_pass, len(results), 100.0 * n_pass / len(results)))
    if lock:
        os.remove(lock)
    return 0


if __name__ == "__main__":
    sys.exit(main())
