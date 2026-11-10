#!/usr/bin/env python3
"""把多份 owner trust bundle 合成一份，解决 --owner-trust-cache 只能有一份的冲突。

背景
----
unixnu/payload.py:121 是 `owner_trust or trust_cache` —— owner trust **整体替换**
UI trust，所以 trust cache 里同时只能存在一份。而我们需要三方的 CDHash 同时生效：

  · Apple 系统库（UI/AMFI 基线）
  · GPU addon 里的 bundle（NativeAdrenoMetal.bundle 等 7 个文件）
  · SpringBoard / mediaspace 注入库（libpicasso-sb.dylib，未来还有音频插件）

格式（从 gpu-trust.bundle 与 scripts/picasso_ui_trust.py 双向反解，非推测）
------------------------------------------------------------------------
bundle:
  u32 count
  u32 offset[count]        # 第一个恒为 4 + 4*count
  module[count]

module:
  u32 type                 # 恒 2
  u8  uuid[16]             # 模块标识（owned 模块用 uuid5 派生）
  u32 entries              # 本模块 CDHash 条数
  entry[entries]           # 每条 24 字节
    u8  cdhash[20]
    u8  hash_type          # 2 = SHA-256
    u8  flags[3]

用法
----
  # 看结构
  python merge_trust.py --show gpu-trust.bundle

  # 合并两个 bundle + 一个 dylib 的 CDHash
  python merge_trust.py --out merged.bundle \\
      --bundle D:/k30i-gpu/gpu-trust.bundle \\
      --bundle out/picasso/ui-trust.bundle \\
      --library out/picasso/libpicasso-sb.dylib

  # 只加库、不加新 bundle
  python merge_trust.py --out merged.bundle --bundle gpu-trust.bundle --library lib.dylib
"""
import argparse
import hashlib
import struct
import sys
import uuid

MODULE_TYPE = 2
HASH_TYPE_SHA256 = 2
ENTRY_SIZE = 24


# ---------------------------------------------------------------- bundle I/O

def parse_bundle(data, name="<bundle>"):
    """返回 [{'type','uuid','entries':[(cdhash, hash_type, flags)]}]"""
    if len(data) < 8:
        raise ValueError(f"{name}: 长度不足 {len(data)}")
    count, = struct.unpack_from("<I", data, 0)
    if count == 0 or count > 64:
        raise ValueError(f"{name}: count={count} 不合理")
    if len(data) < 4 + 4 * count:
        raise ValueError(f"{name}: 头部被截断")
    offsets = struct.unpack_from(f"<{count}I", data, 4)
    if offsets[0] != 4 + 4 * count:
        raise ValueError(f"{name}: 首个 offset {offsets[0]} != {4 + 4 * count}")
    if list(offsets) != sorted(offsets):
        raise ValueError(f"{name}: offsets 非递增")
    bounds = list(offsets) + [len(data)]

    modules = []
    for i in range(count):
        m = data[bounds[i]:bounds[i + 1]]
        if len(m) < 24:
            raise ValueError(f"{name}: module[{i}] 只有 {len(m)} 字节")
        mtype, muuid, n = struct.unpack_from("<I16sI", m, 0)
        if mtype != MODULE_TYPE:
            raise ValueError(f"{name}: module[{i}] type={mtype} != {MODULE_TYPE}")
        body = len(m) - 24
        if n and body % n != 0:
            raise ValueError(f"{name}: module[{i}] body={body} 不能被 {n} 整除")
        per = body // n if n else 0
        if per != ENTRY_SIZE:
            raise ValueError(f"{name}: module[{i}] 每条约 {per} 字节，期望 {ENTRY_SIZE}")
        entries = []
        for k in range(n):
            e = m[24 + k * ENTRY_SIZE: 24 + (k + 1) * ENTRY_SIZE]
            entries.append((bytes(e[:20]), e[20], bytes(e[21:24])))
        modules.append({"type": mtype, "uuid": muuid, "entries": entries})
    return modules


def make_bundle(modules):
    blobs = []
    for mod in modules:
        head = struct.pack("<I16sI", mod["type"], mod["uuid"], len(mod["entries"]))
        body = b"".join(c + bytes([ht]) + fl for c, ht, fl in mod["entries"])
        blobs.append(head + body)

    count = len(blobs)
    cursor = 4 + 4 * count
    offsets = []
    for b in blobs:
        offsets.append(cursor)
        cursor += len(b)
    return struct.pack(f"<I{count}I", count, *offsets) + b"".join(blobs)


def summarize(modules, tag=""):
    total = sum(len(m["entries"]) for m in modules)
    print(f"{tag}{len(modules)} 个模块，共 {total} 条 CDHash")
    for i, m in enumerate(modules):
        print(f"  [{i}] uuid={m['uuid'].hex()[:16]}…  entries={len(m['entries'])}")


# ------------------------------------------------------------- dylib CDHash

def library_identity(data, name="<library>"):
    """从已签名的 Mach-O 里取 CodeDirectory 的 cdhash（前 20 字节 SHA-256）。"""
    if len(data) < 32 or struct.unpack_from("<I", data)[0] != 0xFEEDFACF:
        raise ValueError(f"{name}: 不是 MH_MAGIC_64 Mach-O")
    cpu, _sub, kind, ncmds, _szcmds, _flags = struct.unpack_from("<iiIIII", data, 4)
    if cpu != 0x0100000C:
        raise ValueError(f"{name}: 不是 arm64（cputype={cpu:#x}）")
    if kind not in (6, 8):
        raise ValueError(f"{name}: 不是 dylib/bundle（filetype={kind}）")

    off = 32
    code_sig = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x1D:  # LC_CODE_SIGNATURE
            code_sig = struct.unpack_from("<II", data, off + 8)
        off += cmdsize
        if cmdsize < 8:
            raise ValueError(f"{name}: load command 尺寸异常 {cmdsize}")
    if not code_sig:
        raise ValueError(f"{name}: 没有 LC_CODE_SIGNATURE（未签名？）")

    sig_off, sig_len = code_sig
    sig = data[sig_off:sig_off + sig_len]
    magic, _extent, nblobs = struct.unpack_from(">3I", sig, 0)
    if magic != 0xFADE0CC0:
        raise ValueError(f"{name}: 签名不是 SuperBlob（{magic:#x}）")

    for i in range(nblobs):
        blob_type, blob_off = struct.unpack_from(">2I", sig, 12 + i * 8)
        if blob_type != 0:
            continue
        bmagic, bsize = struct.unpack_from(">2I", sig, blob_off)
        if bmagic != 0xFADE0C02:
            raise ValueError(f"{name}: blob 不是 CodeDirectory（{bmagic:#x}）")
        directory = sig[blob_off:blob_off + bsize]
        return dict(cdhash=hashlib.sha256(directory).digest()[:20],
                    sha256=hashlib.sha256(data).hexdigest())

    raise ValueError(f"{name}: SuperBlob 里没有 CodeDirectory")


def module_for_library(label, cdhash):
    """生成一个 owned module（与 picasso_ui_trust.make_bundle 的 uuid5 规则一致）。"""
    ident = uuid.uuid5(uuid.NAMESPACE_URL, "picasso:trust:" + label + ":" + cdhash.hex())
    return {"type": MODULE_TYPE, "uuid": ident.bytes,
            "entries": [(cdhash, HASH_TYPE_SHA256, bytes([0, 0, 0]))]}


# -------------------------------------------------------------------- merge

def merge(bundles, libraries):
    """bundles: [(name, data)]，libraries: [(label, data)]"""
    seen = set()          # (module_uuid, cdhash) 去重
    merged = []

    def absorb(mods, tag):
        for mod in mods:
            key_mod = mod["uuid"]
            fresh = []
            for cdhash, ht, fl in mod["entries"]:
                k = (key_mod, cdhash)
                if k in seen:
                    continue
                seen.add(k)
                fresh.append((cdhash, ht, fl))
            if not fresh:
                print(f"  {tag}: module {key_mod.hex()[:12]}… 全部重复，跳过")
                continue
            merged.append({"type": mod["type"], "uuid": key_mod, "entries": fresh})

    for name, data in bundles:
        mods = parse_bundle(data, name)
        print(f"吸收 {name}: {len(mods)} 模块 / "
              f"{sum(len(m['entries']) for m in mods)} 条")
        absorb(mods, name)

    for label, data in libraries:
        ident = library_identity(data, label)
        print(f"吸收库 {label}: cdhash={ident['cdhash'].hex()}")
        absorb([module_for_library(label, ident["cdhash"])], label)

    return merged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundle", action="append", default=[], metavar="PATH")
    ap.add_argument("--library", action="append", default=[], metavar="PATH")
    ap.add_argument("--library-label", action="append", default=[],
                    help="库的标识名（默认用文件名）")
    ap.add_argument("--out", default=None)
    ap.add_argument("--show", nargs="+", metavar="PATH")
    args = ap.parse_args()

    if args.show:
        for path in args.show:
            data = open(path, "rb").read()
            print(f"=== {path}  ({len(data):,} bytes, "
                  f"sha256 {hashlib.sha256(data).hexdigest()[:16]}…)")
            summarize(parse_bundle(data, path))
        return 0

    if not args.bundle and not args.library:
        ap.print_help()
        return 2
    if not args.out:
        print("!! 需要 --out")
        return 2

    bundles = [(p, open(p, "rb").read()) for p in args.bundle]
    labels = args.library_label + [None] * (len(args.library) - len(args.library_label))
    libraries = []
    for path, label in zip(args.library, labels):
        libraries.append((label or path.rsplit("/", 1)[-1], open(path, "rb").read()))

    print("--- 输入 ---")
    merged = merge(bundles, libraries)

    print("\n--- 合并结果 ---")
    summarize(merged, tag="")

    blob = make_bundle(merged)
    open(args.out, "wb").write(blob)
    print(f"\n已写出 {args.out}  {len(blob):,} bytes  "
          f"sha256 {hashlib.sha256(blob).hexdigest()[:16]}…")

    # 回读校验：能重新解析、条数一致、每个输入库的 cdhash 都在
    check = parse_bundle(blob, args.out)
    ok = (sum(len(m["entries"]) for m in check) == sum(len(m["entries"]) for m in merged))
    for label, data in libraries:
        cd = library_identity(data, label)["cdhash"]
        present = any(cd == c for m in check for c, _t, _f in m["entries"])
        print(f"  库 {label} 的 cdhash 在合并结果中: {'是' if present else '否'}")
        ok = ok and present
    print("回读校验: " + ("通过" if ok else "失败"))
    return 0 if ok else 5


if __name__ == "__main__":
    sys.exit(main())
