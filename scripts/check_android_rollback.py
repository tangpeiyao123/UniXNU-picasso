#!/usr/bin/env python3
"""Read-only readiness check for rolling the device back to stock Android.

This script NEVER touches the device: no fastboot, no serial port, no writes.
It only verifies that every host-side artifact required by BACKUP-RESTORE.md
exists and matches its recorded size/sha256.

Exit codes
  0 = READY     all hard assets present and verified, no warnings
  2 = BLOCKED   at least one hard asset missing or checksum mismatch
  3 = DEGRADED  hard assets OK but a rescue-tier item is missing (see WARN)
"""
import hashlib
import json
import os
import sys

BACKUP = "D:/k30i-backup"
FW = "D:/k30i-firmware"
IOS_DIR = "D:/k30i-ios"
NEXT = "D:/k30i-gpu/next"
K30I = "C:/Users/Administrator/WorkBuddy/2026-11-08-19-26-16/k30i-ios"

BOOT_STOCK_SHA = os.environ.get(
    "EXPECT_BOOT_STOCK_SHA",
    "0614705392e38dad65da0d9ddc3db367fee2e758aecf5acde2c7fa288c7738b9",
)

results = []


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(4 * 1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def item(level, name, ok, detail):
    results.append((level, name, ok, detail))


def check_file(level, name, path, expect_size=None, expect_sha=None):
    if not os.path.isfile(path):
        item(level, name, False, "MISSING " + path)
        return
    size = os.path.getsize(path)
    detail = "%d B" % size
    ok = True
    if expect_size is not None and size != expect_size:
        ok = False
        detail += " size!=%d" % expect_size
    if expect_sha is not None:
        got = sha256(path)
        detail += " sha=%s" % got[:16]
        if got != expect_sha:
            ok = False
            detail += " MISMATCH expected=%s" % expect_sha[:16]
    item(level, name, ok, detail)


def main():
    # --- Tier L2: stock Android boot image ---------------------------------
    boot = os.path.join(BACKUP, "boot-stock.img")
    check_file("HARD", "stock boot", boot, 134217728, BOOT_STOCK_SHA)
    if os.path.isfile(boot):
        with open(boot, "rb") as f:
            magic = f.read(8)
        item("HARD", "stock boot magic", magic == b"ANDROID!", magic.decode("ascii", "replace"))

    # --- Tier L2: calibration partitions (TWRP live dump) ------------------
    manifest_path = os.path.join(FW, "firmware-manifest.json")
    if not os.path.isfile(manifest_path):
        item("HARD", "firmware manifest", False, "MISSING " + manifest_path)
    else:
        with open(manifest_path, encoding="utf-8") as f:
            man = json.load(f)
        parts = man.get("partitions", {})
        item("HARD", "firmware manifest", True,
             "source=%s partitions=%d" % (man.get("source", "?"), len(parts)))
        for name, meta in parts.items():
            check_file("HARD", "calib " + name,
                       os.path.join(FW, name),
                       meta.get("size"), meta.get("sha256"))

    # --- Tier L1/L2 docs and iOS rebuild assets ----------------------------
    check_file("HARD", "rollback SOP",
               os.path.join(K30I, "BACKUP-RESTORE.md"))
    check_file("HARD", "iOS userdata sparse",
               os.path.join(IOS_DIR, "userdata-root.sparse.img"))
    check_file("HARD", "iOS baseline 685",
               os.path.join(NEXT, "boot-next-685-pscireset.img"))
    check_file("HARD", "iOS current trustcompat2h",
               os.path.join(NEXT, "boot-685-pscireset+trustcompat2h-4a6a853d.img"))

    # --- WARN tier: device snapshot and EDL rescue ------------------------
    info = os.path.join(K30I, "device-info.txt")
    item("WARN", "getvar all snapshot", os.path.isfile(info),
         info if os.path.isfile(info) else "MISSING (current-slot / partition table not archived)")
    edl = []
    for root, _dirs, files in os.walk(FW):
        for fn in files:
            low = fn.lower()
            if low.startswith("prog_") or "firehose" in low or low.startswith("rawprogram") or low.startswith("flash_all"):
                edl.append(os.path.join(root, fn))
    item("WARN", "EDL 9008 firehose", bool(edl),
         "%d file(s)" % len(edl) if edl else "MISSING (no fastboot-less rescue path)")
    chunks = os.path.join(BACKUP, "chunks")
    n_chunks = len([x for x in os.listdir(chunks) if os.path.isfile(os.path.join(chunks, x))]) if os.path.isdir(chunks) else 0
    item("WARN", "userdata dump chunks", n_chunks >= 200, "%d chunks" % n_chunks)

    hard_fail = [r for r in results if r[0] == "HARD" and not r[2]]
    warns = [r for r in results if r[0] == "WARN" and not r[2]]

    for level, name, ok, detail in results:
        print("[%s] %-28s %s  %s" % (level, name, "PASS" if ok else "FAIL", detail))
    verdict = "BLOCKED" if hard_fail else ("DEGRADED" if warns else "READY")
    print("--- verdict ---")
    print("hard_fail=%d warn=%d" % (len(hard_fail), len(warns)))
    print("ANDROID_ROLLBACK=%s" % verdict)
    print("NOTE: DEGRADED means L2 (fastboot -> stock Android) is usable,"
          " but L3 (EDL 9008 when fastboot is unreachable) is NOT prepared.")
    return 2 if hard_fail else (3 if warns else 0)


if __name__ == "__main__":
    sys.exit(main())
