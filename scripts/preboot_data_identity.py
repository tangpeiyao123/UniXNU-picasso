#!/usr/bin/env python3
"""fastboot boot 之前的数据身份门禁（pre-boot Data identity gate）。

为什么需要它
------------
report-hw-firmware.md §4.8/§9 已记录：新内核在启动时会按自己的 seedHash
算法重建 Data 卷，之后 boot 分区里的旧内核认不出这块盘，
`NATIVE_OWNER_DATA stage=0 error=2` →
`offline scratch Data mount failed: 2 @kern_exec.c:9908` → panic。
且切换是**单向**的（新内核建的盘旧内核也认不出）。

`fastboot boot` 不写 boot 分区，但会写 Data 卷，因此「只 fastboot boot、
没刷机」**不等于**没改设备。2026-11-12 下午到夜里反复 fastboot boot 了
ca349c0c / 4c199744 等新内核，D1-warm(:1082) 与 M3(:?) 都走到
`NATIVE_OWNER_DATA done stage=4 error=0`，即确实接管并重建了 Data。

本脚本做什么
------------
对比「待引导镜像」与「内部已刷稳定 boot（基准镜像）」的四项指纹：
  1. 内核横幅（Darwin Kernel Version ...）
  2. KERNEL~1.MAC 的 sha256
  3. SCRATC（Data 种子）的 sha256
  4. 内核 ASCII UUID 与种子内二进制 UUID 副本数

关键发现（2026-11-13 实测，勿凭直觉跳过第 2 项）：
  boot-680537f5 / boot-ca349c0c / boot-4c199744 三份镜像的 **seed sha256
  完全相同**(9220bb17…) 且 **UUID 完全一致**(0EBB496E…×7)，
  但 **内核横幅与 kernel sha256 各不相同**。
  → 所以「seed 相同 / UUID 相同」**不能**证明身份安全；
    实际风险来自内核侧 seedHash 算法变化，判据必须是 kernel 指纹。

重要限制（不要越界下结论）
--------------------------
主机侧**无法读取设备当前 Data 卷的身份**。本脚本只比较两个镜像，
输出的是「身份不一致风险」，不是「设备现在一定 panic」。
设备现状必须用串口日志配合 check_boot_session.py 判定。

用法：
  python3 preboot_data_identity.py <candidate.img> --baseline <internal-boot.img>
  python3 preboot_data_identity.py <candidate.img> --baseline <img> \
      --payload-helper <dir-of-patch_payload_file.py>

退出码：
  0 两份镜像内核指纹一致 —— 可安全交替引导（仍需看串口日志）
  3 内核指纹不同        —— 高风险：会重建 Data 身份，重启回基准内核会 panic
  4 内核 ASCII UUID 与种子 UUID 不一致 —— 更严重：Compromised Data，内核根本不启
  1 参数/读取错误
"""
import argparse
import hashlib
import os
import re
import sys
import uuid

DEFAULT_HELPER = "C:/Users/Administrator/WorkBuddy/2026-11-08-19-26-16/k30i-ios"
UUID_RE = re.compile(rb"[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}")
ANCHOR = b"NATIVE_OWNER_DATA provider=ram uuid=%s"


def load_payload(helper_dir):
    sys.path.insert(0, helper_dir)
    try:
        from patch_payload_file import Payload
        return Payload
    except Exception as e:
        print("ERROR: cannot import Payload from %s: %s" % (helper_dir, e),
              file=sys.stderr)
        print("       pass --payload-helper <dir>", file=sys.stderr)
        sys.exit(1)


def fingerprint(path, Payload):
    p = Payload(path)
    _, mac = p.read("KERNEL~1.MAC")
    _, seed = p.read("SCRATC")
    m = re.search(rb"Darwin Kernel Version[^\x00\n]{0,90}", mac)
    banner = m.group(0).decode("latin1", "replace") if m else "<none>"
    i = mac.find(ANCHOR)
    ku = None
    if i >= 0:
        mm = UUID_RE.search(mac, i, i + len(ANCHOR) + 64)
        if mm:
            ku = mm.group().decode()
    # 种子里的卷 UUID：按内核值查二进制副本数（沿用 fix_data_uuid 的做法，不做统计猜测）
    n = 0
    su = None
    if ku:
        try:
            b = uuid.UUID(ku).bytes
            n = seed.count(b)
            su = ku if n else None
        except ValueError:
            pass
    return {
        "path": path,
        "banner": banner,
        "kernel_sha": hashlib.sha256(mac).hexdigest(),
        "seed_sha": hashlib.sha256(seed).hexdigest(),
        "kernel_uuid": ku,
        "seed_uuid": su,
        "seed_uuid_copies": n,
        "kernel_len": len(mac),
        "seed_len": len(seed),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("candidate")
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--payload-helper", default=DEFAULT_HELPER)
    a = ap.parse_args()

    for f in (a.candidate, a.baseline):
        if not os.path.isfile(f):
            print("ERROR: missing %s" % f, file=sys.stderr)
            return 1
    Payload = load_payload(a.payload_helper)

    try:
        cand = fingerprint(a.candidate, Payload)
        base = fingerprint(a.baseline, Payload)
    except Exception as e:
        print("ERROR: fingerprint failed: %s" % e, file=sys.stderr)
        return 1

    for tag, d in (("CANDIDATE", cand), ("BASELINE ", base)):
        print("%s %s" % (tag, os.path.basename(d["path"])))
        print("   banner    : %s" % d["banner"])
        print("   kernel_sha: %s  (len=%d)" % (d["kernel_sha"][:16], d["kernel_len"]))
        print("   seed_sha  : %s  (len=%d)" % (d["seed_sha"][:16], d["seed_len"]))
        print("   kernel UUID: %s | seed UUID copies=%d" %
              (d["kernel_uuid"], d["seed_uuid_copies"]))

    uuid_ok = all(d["kernel_uuid"] and d["seed_uuid"] for d in (cand, base))
    same_kernel = (cand["kernel_sha"] == base["kernel_sha"])
    same_seed = (cand["seed_sha"] == base["seed_sha"])
    same_uuid = (cand["kernel_uuid"] == base["kernel_uuid"])

    print("\n--- verdict ---")
    print("  kernel identical : %s" % same_kernel)
    print("  seed identical   : %s" % same_seed)
    print("  uuid identical   : %s" % same_uuid)

    if not uuid_ok:
        print("  => RC=4 Compromised Data: kernel/seed UUID 自相矛盾，内核不会启动")
        return 4
    if not same_kernel:
        print("  => RC=3 内核指纹不同（即使 seed/UUID 相同）：该镜像会按自己的")
        print("     seedHash 算法重建 Data 卷身份；重启回 BASELINE 内核会")
        print("     panic(offline scratch Data mount failed: 2)。")
        print("     处置二选一：① 抹存储头+初始化日志让 BASELINE 自重建 Data；")
        print("                ② 把 BASELINE 刷成与 CANDIDATE 同一内核后再重启。")
        print("     主机侧读不到设备当前 Data 身份，设备现状请用串口日志判定。")
        return 3
    print("  => RC=0 内核指纹一致：Data 身份不因本次引导而改变（仍需看串口日志）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
