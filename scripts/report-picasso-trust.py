#!/usr/bin/env python3
"""Print the trust modules that actually ended up inside a composed boot image.

A trust bundle that passes host checks can still be merged, reordered or
replaced while the payload is packed. This reads the finished image, so the
report is about the bytes we are going to boot, not about an intermediate file.

    python report-picasso-trust.py out/picasso/boot/boot.img
    python report-picasso-trust.py boot.img --require configs/picasso-trust-compat.json

--require exits non-zero when a listed CDHash is absent, which makes "the
installed library is still trusted" a build-time gate instead of a claim.
"""
import argparse
import json
import struct
import sys

TRU_NAMES = ("KERNEL~1TRU", "KERNEL~1.TRU")  # FAT 8.3 short name, no dot
ENTRY_SIZE = 24


class BootRamdisk:
    """Minimal reader for the Android boot image's FAT32 ramdisk."""

    def __init__(self, path):
        self.img = open(path, "rb").read()
        (magic, ksize, _kaddr, rsize, _raddr, _ssize, _saddr, _taddr,
         page) = struct.unpack_from("<8s8I", self.img, 0)
        if magic != b"ANDROID!":
            raise ValueError("%s: not an Android boot image" % path)
        pages = lambda n: (n + page - 1) // page * page
        off = page + pages(ksize)
        self.rd = self.img[off:off + rsize]
        if len(self.rd) < 512:
            raise ValueError("%s: ramdisk truncated" % path)
        self.bps, self.spc = struct.unpack_from("<HB", self.rd, 11)
        reserved, fats = struct.unpack_from("<HB", self.rd, 14)
        spf = struct.unpack_from("<I", self.rd, 36)[0]
        root = struct.unpack_from("<I", self.rd, 44)[0]
        self.fat_off = reserved * self.bps
        self.data_off = (reserved + fats * spf) * self.bps
        self.cluster = self.bps * self.spc
        self.entries = {}
        self._walk(root, "")

    def _next(self, cluster):
        value, = struct.unpack_from("<I", self.rd, self.fat_off + cluster * 4)
        return value & 0x0FFFFFFF

    def _chain(self, cluster):
        out = []
        while 2 <= cluster < 0x0FFFFFF0 and len(out) < 200000:
            out.append(cluster)
            cluster = self._next(cluster)
        return out

    def _offset(self, cluster):
        return self.data_off + (cluster - 2) * self.cluster

    def _walk(self, cluster, prefix):
        for current in self._chain(cluster):
            base = self._offset(current)
            for index in range(0, self.cluster, 32):
                entry = self.rd[base + index:base + index + 32]
                if len(entry) < 32 or entry[0] == 0xE5:
                    continue
                if entry[0] == 0:
                    return
                if entry[11] == 0x0F:
                    continue
                name = entry[:11].decode("ascii", "replace").strip()
                if name in (".", ".."):
                    continue
                start = struct.unpack_from("<H", entry, 26)[0] | (struct.unpack_from("<H", entry, 20)[0] << 16)
                size = struct.unpack_from("<I", entry, 28)[0]
                full = prefix + name
                if entry[11] & 0x10:
                    if start >= 2:
                        self._walk(start, full + "/")
                elif start >= 2 and size:
                    self.entries[full] = (start, size)

    def read(self, name):
        start, size = self.entries[name]
        data = b"".join(self.rd[self._offset(c):self._offset(c) + self.cluster]
                        for c in self._chain(start))
        return data[:size]


def parse_bundle(blob):
    count, = struct.unpack_from("<I", blob, 0)
    if not 1 <= count <= 64:
        raise ValueError("implausible module count %d" % count)
    offsets = struct.unpack_from("<%dI" % count, blob, 4)
    if offsets[0] != 4 + 4 * count or list(offsets) != sorted(offsets):
        raise ValueError("invalid module offsets")
    bounds = list(offsets) + [len(blob)]
    modules = []
    for index in range(count):
        body = blob[bounds[index]:bounds[index + 1]]
        if len(body) < 24:
            raise ValueError("module %d truncated" % index)
        mtype, muuid, entries = struct.unpack_from("<I16sI", body, 0)
        if (len(body) - 24) != entries * ENTRY_SIZE:
            raise ValueError("module %d entry count mismatch" % index)
        hashes = []
        for slot in range(entries):
            raw = body[24 + slot * ENTRY_SIZE:24 + (slot + 1) * ENTRY_SIZE]
            hashes.append(dict(cdhash=raw[:20].hex(), hash_type=raw[20], flags=raw[21:24].hex()))
        modules.append(dict(type=mtype, uuid=muuid.hex(), entries=hashes))
    return modules


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image")
    parser.add_argument("--require", help="JSON with a cdhashes list that must all be trusted")
    parser.add_argument("--json", help="write the report here")
    args = parser.parse_args()

    ramdisk = BootRamdisk(args.image)
    names = [name for name in ramdisk.entries if name.rsplit("/", 1)[-1] in TRU_NAMES]
    if len(names) != 1:
        raise SystemExit("%s: expected one KERNEL~1TRU, found %r" % (args.image, sorted(ramdisk.entries)))
    blob = ramdisk.read(names[0])
    modules = parse_bundle(blob)
    trusted = {entry["cdhash"] for module in modules for entry in module["entries"]}

    print("%s: %s %d bytes, %d modules, %d trusted CDHashes"
          % (args.image, names[0], len(blob), len(modules), len(trusted)))
    for index, module in enumerate(modules):
        print("  [%d] uuid=%s entries=%d" % (index, module["uuid"][:16] + "…", len(module["entries"])))
        if len(module["entries"]) <= 2:
            for entry in module["entries"]:
                print("        cdhash=%s hash_type=%d flags=%s"
                      % (entry["cdhash"], entry["hash_type"], entry["flags"]))

    report = dict(image=args.image, tru_bytes=len(blob),
                  modules=[dict(uuid=m["uuid"], entries=len(m["entries"]),
                                cdhash=[e["cdhash"] for e in m["entries"]]) for m in modules],
                  trusted=sorted(trusted))

    status = 0
    if args.require:
        required = json.load(open(args.require, encoding="utf-8"))["cdhashes"]
        missing = [value.lower() for value in required if value.lower() not in trusted]
        for value in required:
            print("  require %s: %s" % (value, "present" if value.lower() in trusted else "MISSING"))
        report["required"] = [value.lower() for value in required]
        report["missing"] = missing
        if missing:
            print("FAIL: %d required CDHash(es) are not trusted" % len(missing))
            status = 5

    if args.json:
        open(args.json, "w", encoding="utf-8").write(json.dumps(report, indent=2) + "\n")
        print("wrote", args.json)
    if status == 0:
        print("PASS: trust table contains every required CDHash")
    return status


if __name__ == "__main__":
    sys.exit(main())
