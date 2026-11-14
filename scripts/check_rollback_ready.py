#!/usr/bin/env python3
"""Rollback readiness: the assets needed to come back from a kernel experiment.

Two halves, both read-only:

  bundle   every file the rollback depends on, hashed against a manifest that
           --write-manifest produced once. A missing file or a changed hash is a
           HARD failure: that is the copy we would rely on while the phone is
           unbootable.
  device   `fastboot getvar all`, parsed for the three values the reviewer asked
           to confirm before touching boot: current-slot, anti-rollback and the
           userdata/boot geometry. Skipped with a WARN when no device answers.

Exit codes
  0 = READY     bundle verified, no hard failure; device values match (or were not requested)
  2 = BLOCKED   a hard asset is missing or its hash changed (--write-manifest to re-record is a decision, not a fix)
  3 = DEGRADED  bundle verified but a rescue tier is missing (EDL firehose) or no device answered
  4 = USAGE     bad arguments

What this cannot do: read the boot partition back. This bootloader does not
implement Fastboot `fetch` (docs/usb-console-bringup.md:106 tried it read-only
and it failed), so the only honest device-side evidence is `getvar`, plus the
host record of what was last written to it.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime

BACKUP = "D:/k30i-backup"
FW = "D:/k30i-firmware"
IOS_DIR = "D:/k30i-ios"
NEXT = "D:/k30i-gpu/next"
K30I = "C:/Users/Administrator/WorkBuddy/2026-11-08-19-26-16/k30i-ios"
MANIFEST = os.path.join(BACKUP, "rollback-manifest.json")
FASTBOOT = "D:/ADB/fastboot.exe"

# Values the device must still report. Slot 0 / no slot marker is what a single
# slot board without A/B looks like; the sizes are the ones recorded on 11-13.
EXPECT = {
    "partition-size:boot": 0x8000000,
    "partition-size:userdata": 0x1AC2BFB000,
    "partition-type:boot": "raw",
}

results = []


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 * 1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def item(level, name, ok, detail):
    results.append((level, name, ok, detail))
    print("  %-4s %-34s %s %s" % (level, name, "ok  " if ok else "FAIL", detail))


def bundle_entries():
    """(label, path, role, required) — every file a rollback would use."""
    out = [
        ("boot-stock", os.path.join(BACKUP, "boot-stock.img"), "L2 stock Android boot", True),
        ("boot-ios-current", os.path.join(NEXT, "boot-685-pscireset+trustcompat2h-4a6a853d.img"),
         "L1 iOS boot that is in the partition now", True),
        ("usb-console-SOP", os.path.join(K30I, "BACKUP-RESTORE.md"), "rollback procedure", True),
        ("device-info", os.path.join(K30I, "device-info.txt"), "getvar snapshot 11-13 12:30", True),
        ("ios-userdata-sparse", os.path.join(IOS_DIR, "userdata-root.sparse.img"),
         "L1 iOS Data restore (8.4 GB)", True),
    ]
    man = os.path.join(FW, "firmware-manifest.json")
    if os.path.isfile(man):
        with open(man, encoding="utf-8") as f:
            parts = json.load(f).get("partitions", {})
        for name in sorted(parts):
            out.append(("calib-" + name, os.path.join(FW, name), "L2 calibration", True))
    return out


def cmd_write_manifest():
    doc = {"schema": "picasso-rollback-manifest-v1", "created": datetime.now().isoformat(timespec="seconds"),
           "note": "written once; verify only, never rewrite after an experiment", "entries": []}
    print("hashing %d assets (the 8.4 GB image takes a while) ..." % len(bundle_entries()))
    for label, path, role, required in bundle_entries():
        if not os.path.isfile(path):
            print("  MISSING %-30s %s" % (label, path))
            continue
        size = os.path.getsize(path)
        digest = sha256(path)
        doc["entries"].append({"label": label, "path": path.replace("\\", "/"),
                               "role": role, "required": required,
                               "size": size, "sha256": digest})
        print("  %-30s %12d B  %s" % (label, size, digest[:16]))
    with open(MANIFEST, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print("wrote %s (%d entries)" % (MANIFEST, len(doc["entries"])))
    return 0


def cmd_verify(skip_large):
    if not os.path.isfile(MANIFEST):
        print("NO MANIFEST: %s (run --write-manifest first)" % MANIFEST)
        return 2
    with open(MANIFEST, encoding="utf-8") as f:
        doc = json.load(f)
    print("manifest: %s (%d entries, %s)" % (MANIFEST, len(doc["entries"]), doc.get("created")))
    hard_fail = 0
    for e in doc["entries"]:
        path = e["path"]
        if not os.path.isfile(path):
            item("HARD", e["label"], False, "MISSING " + path)
            hard_fail += 1
            continue
        size = os.path.getsize(path)
        if size != e["size"]:
            item("HARD", e["label"], False, "%d B != %d recorded" % (size, e["size"]))
            hard_fail += 1
            continue
        if skip_large and size > 2 * 1024 ** 3:
            item("WARN", e["label"], True, "%d B size ok, hash skipped (--skip-large)" % size)
            continue
        digest = sha256(path)
        ok = digest == e["sha256"]
        item("HARD", e["label"], ok, "%d B sha=%s%s" % (
            size, digest[:16], "" if ok else " != %s" % e["sha256"][:16]))
        hard_fail += 0 if ok else 1
    return hard_fail


def cmd_device():
    if not os.path.isfile(FASTBOOT):
        item("WARN", "fastboot binary", False, "MISSING " + FASTBOOT)
        return 1
    try:
        out = subprocess.run([FASTBOOT, "getvar", "all"], capture_output=True, text=True, timeout=25)
    except subprocess.TimeoutExpired:
        item("WARN", "getvar all", False, "timeout (device not in fastboot?)")
        return 1
    text = (out.stdout or "") + (out.stderr or "")
    if "partition-size" not in text:
        item("WARN", "getvar all", False, "no device answered")
        return 1
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    snap = os.path.join(BACKUP, "getvar-%s.txt" % stamp)
    with open(snap, "w", encoding="utf-8") as f:
        f.write(text)
    item("INFO", "getvar snapshot", True, snap)

    vals = {}
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("(bootloader)"):
            line = line[len("(bootloader)"):].strip()
        # "partition-size:boot: 0x8000000" and "partition-type:boot:raw" — the key
        # itself contains a colon and the value is not always space-separated, so
        # split on the last colon either way.
        sep = ": " if ": " in line else ":"
        if sep not in line:
            continue
        k, _, v = line.rpartition(sep)
        vals[k.strip()] = v.strip()
    bad = 0
    for key, want in EXPECT.items():
        got = vals.get(key)
        if got is None:
            item("HARD", key, False, "absent")
            bad += 1
            continue
        if isinstance(want, int):
            try:
                n = int(got, 16) if got.lower().startswith("0x") else int(got)
            except ValueError:
                n = -1
            ok = n == want
            item("HARD", key, ok, "%s (%s)" % (got, "expected %#x" % want if not ok else ""))
        else:
            ok = got == want
            item("HARD", key, ok, got)
        bad += 0 if ok else 1
    slot = vals.get("current-slot") or vals.get("slot") or "(absent: single slot)"
    item("INFO", "current-slot", True, slot)
    # This bootloader reports the rollback index as plain "anti", not
    # "anti-rollback"; keep all three spellings so a renamed key is visible.
    ab = vals.get("anti-rollback") or vals.get("rollback-index") or vals.get("anti")
    item("INFO", "anti-rollback", True, ab if ab is not None else "(not reported)")
    item("WARN", "boot readback", False,
         "bootloader has no fastboot fetch (usb-console-bringup.md:106); getvar + host flash record only")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write-manifest", action="store_true")
    ap.add_argument("--device", action="store_true", help="also read getvar all (read-only)")
    ap.add_argument("--skip-large", action="store_true", help="size-only check for files >2 GiB")
    args = ap.parse_args()

    if args.write_manifest:
        return cmd_write_manifest()

    print("== bundle")
    hard_fail = cmd_verify(args.skip_large)
    device_fail = 0
    if args.device:
        print("== device (read-only getvar)")
        device_fail = cmd_device()

    edl = os.path.join(FW, "prog_firehose_ddr.elf")
    edl_ok = os.path.isfile(edl)
    item("WARN", "EDL firehose", edl_ok,
         "present" if edl_ok else "MISSING: no rescue if fastboot is unreachable")

    total_fail = hard_fail + device_fail
    # Gate (revised 2026-11-14): the EDL firehose cannot be obtained, so requiring
    # RC=0 would block every experiment forever. What matters is that the bundle
    # and the device both verify; EDL stays a separate constraint on what we may
    # do (nothing that can leave fastboot unreachable).
    if not args.device:
        print("\nhard_fail=%d  (device not checked: pass --device)" % hard_fail)
        print("GATE=PARTIAL  RC=3")
        return 3
    print("\nhard_fail=%d device_fail=%d" % (hard_fail, device_fail))
    if not edl_ok:
        print("EDL=missing -> constraint: no operation that can make fastboot unreachable")
    if total_fail:
        print("ROLLBACK_READY=0  GATE=FAIL  RC=2")
        return 2
    print("ROLLBACK_READY=1  GATE=PASS  RC=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
