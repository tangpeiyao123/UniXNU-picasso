#!/usr/bin/env python3
"""按 Darwin banner 切会话，按三档口径报 iOS 启动成功率。

为什么要有这个脚本：`check_boot_session.py` 的 CLEAN 判据是「出过帧 + 末态仍在出帧」，
它把「只出 4 条帧采样」和「中间停了 220 秒」的会话都算成功。这里不改动那个判据，
只把同一份日志按**可配置口径**重报，让「成功率」这个数字始终带着分母和定义。

口径（全部按行首 `Darwin Kernel Version` 切会话，不含 panic log 里的 `Kernel version:`）：
  LAX       : panic=0 且 stage4>=1 且 SpringBoard>=1 且 出过帧
              —— 等价于现版 check_boot_session.py 的 CLEAN 口径
  STRICT    : LAX 且 帧采样数 >= --min-samples 且 无 > --max-stall-s 秒的停顿
              —— 用于「能进桌面且可用」的验收
  AVAILABLE : STRICT 且 无 >1 秒停顿（更严，用于观察长期掉帧）

失败分类（优先级从高到低）：
  SEED_PANIC   : panic + offline scratch Data mount failed / stage=0 error=2
  GPU_NO_FRAME : stage4 + SpringBoard 但 0 帧
  STALL        : 出过帧但有 > --max-stall-s 秒停顿
  TOO_SHORT    : 出过帧但采样数不足
  OTHER        : 其余

退出码（取全量中最严重会话，不挑窗口）：
  0 全部 STRICT/AVAILABLE
  2 存在 SEED_PANIC
  3 存在 GPU_NO_FRAME
  4 存在 STALL
  5 存在 TOO_SHORT
  6 没有任何可判定会话（空日志 / 只含 banner）

用法:
  python check_boot_sla.py <log> [--min-samples 60] [--max-stall-s 60] [--json]
"""
import argparse
import json
import os
import re
import sys

BANNER = "Darwin Kernel Version"
# 末帧之后累计多少行 hangcheck 才算「末态挂住」（行数代理量，不是秒）。
# 2026-11-13 13:00 与 check_boot_session.py 同步：hangcheck polls 会限流停打印，
# 不能用 polls×0.5s 当时钟；标定见 check_boot_session.py 的 frame_timeline docstring。
TAIL_HANG_MIN = 120

RC = {
    "OK": 0, "SEED_PANIC": 2, "GPU_NO_FRAME": 3,
    "STALL": 4, "STALLED_END": 4, "TOO_SHORT": 5, "EMPTY": 6, "OTHER": 7,
}
SEVERITY = {"OK": 0, "TOO_SHORT": 1, "STALL": 2, "STALLED_END": 2,
            "GPU_NO_FRAME": 3, "SEED_PANIC": 4, "OTHER": 5, "EMPTY": 6}


def split_sessions(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    idx = [i for i, ln in enumerate(lines) if ln.lstrip("\x00").startswith(BANNER)]
    out = []
    for n, s in enumerate(idx):
        e = idx[n + 1] if n + 1 < len(idx) else len(lines)
        out.append(lines[s:e])
    return lines, out


def classify(seg, min_samples, max_stall_s):
    b = "\n".join(seg)
    periods = [int(x) for x in re.findall(r"DPU_FRAME n=\d+ period_us=(\d+)", b)]
    panic = len(re.findall(r"panic\(", b))
    data_bad = len(re.findall(r"offline scratch Data mount failed|stage=0 error=2", b))
    stage4 = len(re.findall(r"NATIVE_OWNER_DATA done stage=4 error=0", b))
    sb = len(re.findall(r"SpringBoard started", b))
    big = len([p for p in periods if p > 1_000_000])
    long_ = len([p for p in periods if p > max_stall_s * 1_000_000])
    max_p = max(periods) if periods else 0

    if panic and data_bad:
        verdict, why = "SEED_PANIC", "panic=%d data_bad=%d" % (panic, data_bad)
    elif panic:
        verdict, why = "OTHER", "panic=%d" % panic
    elif not periods:
        if stage4 and sb:
            verdict, why = "GPU_NO_FRAME", "stage4+SB but 0 frames"
        else:
            verdict, why = "OTHER", "no frame, stage4=%d SB=%d" % (stage4, sb)
    elif long_:
        verdict, why = "STALL", "%d stall(s) > %ds, max=%.1fs" % (long_, max_stall_s, max_p / 1e6)
    elif len(periods) < min_samples:
        verdict, why = "TOO_SHORT", "samples=%d < %d" % (len(periods), min_samples)
    else:
        verdict, why = "OK", ""

    # 末态：最后一条帧之后还在刷 hangcheck ⇒ 出过帧但已停住，不能算「可用桌面」。
    last_frame = -1
    for i, ln in enumerate(seg):
        if "PICASSO_DPU_FRAME" in ln:
            last_frame = i
    tail_hang = (sum(1 for ln in seg[last_frame + 1:] if "hangcheck" in ln)
                 if last_frame >= 0 else 0)
    stalled_end = tail_hang >= TAIL_HANG_MIN
    if stalled_end and why:
        why += "; 末帧后hang=%d" % tail_hang
    elif stalled_end:
        why = "末帧后hang=%d（末态停住）" % tail_hang

    # 分类标签与验收口径对齐：末态停住的会话不许仍叫 OK，否则报表自相矛盾。
    if stalled_end and verdict == "OK":
        verdict = "STALLED_END"

    lax = bool(panic == 0 and stage4 and sb and periods)
    strict = bool(lax and not long_ and not stalled_end
                  and len(periods) >= min_samples)
    avail = bool(strict and big == 0)
    return {
        "verdict": verdict, "why": why, "panic": panic, "stage4": stage4,
        "springboard": sb, "samples": len(periods), "big_stalls": big,
        "long_stalls": long_, "max_period_s": round(max_p / 1e6, 2),
        "tail_hang": tail_hang, "stalled_end": stalled_end,
        "lax": lax, "strict": strict, "available": avail,
        "lines": len(seg),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--min-samples", type=int, default=60)
    ap.add_argument("--max-stall-s", type=int, default=60)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    if not os.path.isfile(a.log):
        print("ERROR: missing %s" % a.log, file=sys.stderr)
        return 6
    _lines, segs = split_sessions(a.log)
    recs = []
    for i, seg in enumerate(segs, 1):
        if not seg or not seg[0].lstrip("\x00").startswith(BANNER):
            continue
        r = classify(seg, a.min_samples, a.max_stall_s)
        r["session"] = i
        r["line"] = _line_of(_lines, seg[0], i)
        recs.append(r)

    if not recs:
        print("EMPTY: no banner-prefixed session")
        print("OVERALL_RC=6")
        return 6

    n = len(recs)
    lax = [r["session"] for r in recs if r["lax"]]
    strict = [r["session"] for r in recs if r["strict"]]
    avail = [r["session"] for r in recs if r["available"]]
    worst = max(recs, key=lambda r: SEVERITY[r["verdict"]])["verdict"]
    rc = RC[worst]

    if a.json:
        print(json.dumps({"sessions": recs, "lax": lax, "strict": strict,
                          "available": avail, "rc": rc}, ensure_ascii=False, indent=2))
        return rc

    print("=== %s ===" % os.path.basename(a.log))
    print("sessions=%d  min_samples=%d  max_stall_s=%ds" % (n, a.min_samples, a.max_stall_s))
    print("%-4s %-9s %-15s %-8s %-7s %-7s %-7s %-8s %-9s %s" %
          ("#", "line", "verdict", "samples", ">1s", ">60s", "max_s",
           "末hang", "lax/str/av", "why"))
    for r in recs:
        print("%-4d %-9d %-15s %-8d %-7d %-7d %-7s %-8d %-9s %s" %
              (r["session"], r["line"], r["verdict"], r["samples"],
               r["big_stalls"], r["long_stalls"], r["max_period_s"],
               r["tail_hang"],
               "%d/%d/%d" % (r["lax"], r["strict"], r["available"]), r["why"]))
    print()
    print("LAX      (出过帧+SB+stage4)        : %2d/%d = %5.1f%%  %s" %
          (len(lax), n, 100.0 * len(lax) / n, lax))
    print("STRICT   (+采样>=%d, 无>%ds停顿) : %2d/%d = %5.1f%%  %s" %
          (a.min_samples, a.max_stall_s, len(strict), n, 100.0 * len(strict) / n, strict))
    print("AVAILABLE(+无>1s停顿)             : %2d/%d = %5.1f%%  %s" %
          (len(avail), n, 100.0 * len(avail) / n, avail))
    print()
    from collections import Counter
    print("failure classes:", dict(Counter(r["verdict"] for r in recs)))
    print("OVERALL_VERDICT=%s  OVERALL_RC=%d" % (worst, rc))
    return rc


def _line_of(all_lines, first, n):
    # 找第 n 个行首 banner 的绝对行号
    seen = 0
    for i, ln in enumerate(all_lines, 1):
        if ln.lstrip("\x00").startswith(BANNER):
            seen += 1
            if seen == n:
                return i
    return -1


if __name__ == "__main__":
    sys.exit(main())
