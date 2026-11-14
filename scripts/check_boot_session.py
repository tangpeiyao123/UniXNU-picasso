#!/usr/bin/env python3
"""按 Darwin 横幅切会话，判定每次 XNU 启动的终止状态。

判据来源（全部取自 D:/k30i-gpu/next/ 的真实日志，不是推测）：
  CLEAN               : PICASSO_DPU_FRAME > 0            (run-M3-retry.log: 385 帧)
  DATA_IDENTITY_PANIC : panic + "offline scratch Data mount failed"
                                                        (run-M2-trust5.log:1344, run-R7-685.log:1480)
  SEED_STALL          : 无 stage=4、无帧、provider=ram/hangcheck
                                                        (run-M4-retry2.log, run-M5-retry3.log)
  GPU_STALL           : 有 stage=4 + SpringBoard、无帧、fence 不推进
                                                        (run-D1-warm.log)
  OTHER_PANIC         : 其它 panic
  UNKNOWN             : 证据不足，不硬判

重要：fence=19 / hangcheck 在**成功启动里也出现**（M3 有 fence=19×18、
hangcheck×486，同时有 385 帧），所以这两个串**不能**单独作为卡死判据，
必须以「有没有帧」为准。这是本脚本存在的理由。

用法：
  python3 check_boot_session.py <log> [<log> ...]
  python3 check_boot_session.py --dir D:/k30i-gpu/next --glob 'run-*.log'

退出码（供门槛脚本直接调用）：
  0 全部会话 CLEAN
  2 出现 DATA_IDENTITY_PANIC
  3 出现 SEED_STALL（无更严重项）
  4 出现 GPU_STALL（无更严重项）
  5 出现 OTHER_PANIC（无更严重项）
  6 出现 UNKNOWN（无更严重项）
  1 参数/文件错误
"""
import argparse
import glob
import os
import re
import sys

# Anchor to the line start: a panic log echoes "Kernel version: Darwin
# Kernel Version ..." on its own line, and a bare substring test counted
# each of those as a new session.
BANNER = "Darwin Kernel Version"
MARKERS = {
    "panic": "panic(",
    "data_panic": "offline scratch Data mount failed",
    "data_err2": "OWNER_DATA error=2",
    "stage4": "NATIVE_OWNER_DATA done stage=4 error=0",
    "frame": "PICASSO_DPU_FRAME",
    "springboard": "SpringBoard started",
    "provider_ram": "provider=ram",
    "seed_retry": "seed retry attempt",
    "hangcheck": "hangcheck",
    "fence": "fence=19",
    "data_init": "PICASSO_DATA_INIT phase=1",
}

FRAME_RE = re.compile(r"PICASSO_DPU_FRAME n=(\d+) period_us=(\d+)")
POLL_RE = re.compile(r"hangcheck: polls=(\d+)")

# 末帧之后累计多少行 hangcheck 才算「末态挂住」。行数代理量，不是秒。
# 实测标定：第 20 会话（220 s 停顿中）截断样本 ≈240 行；
# 第 21 会话（12:59 末帧后停住）150 行；健康出帧样本 0–6 行。
TAIL_HANG_MIN = 120

# 退出码（对外契约，供门槛脚本读取）
RC = {
    "CLEAN": 0,
    "DATA_IDENTITY_PANIC": 2,
    "SEED_STALL": 3,
    "GPU_STALL": 4,
    "GPU_STALL_AFTER_FRAMES": 4,
    "OTHER_PANIC": 5,
    "UNKNOWN": 6,
}
# 严重度单独一张表，与退出码解耦：数字越大越严重。
# 曾经误把 RC 当严重度用 `>` 取最大，结果 UNKNOWN(6) 盖过一切、
# GPU_STALL(4) 盖过 DATA_IDENTITY_PANIC(2)，退出码完全失真，故拆开。
SEVERITY = {
    "CLEAN": 0,
    "UNKNOWN": 1,
    "GPU_STALL": 2,
    "GPU_STALL_AFTER_FRAMES": 2,
    "SEED_STALL": 3,
    "OTHER_PANIC": 4,
    "DATA_IDENTITY_PANIC": 5,
}


def frame_timeline(lines):
    """返回 (帧序列, 末帧之后的 hangcheck 行数)。

    2026-11-13 13:00 修正：**废弃 hangcheck polls 当本地时钟**。
    实测证据：12:52:45→12:53:30 间隔 45 s，`polls/fence` 固定 (217,116) 不变，
    而帧采样 14→33、末 n 420→990 —— hangcheck 会限流/整段停打印，且 DPU_FRAME
    与 hangcheck 是分块输出的，用「上一条 hangcheck 的 polls × 0.5s」给帧打时间戳
    不具物理意义。

    决定性对照（tests/fixtures/）：同一份健康出帧样本，帧内容一字未改，
    仅在末尾追加 6 行 polls 更大的 hangcheck，旧判据就从
    `CLEAN / RC=0` 翻成 `GPU_STALL_AFTER_FRAMES / RC=4`（末帧@108s/450s）。
    即旧判据看的是 polls 数值，不是设备状态。

    现在末态判据改用「末帧之后 hangcheck 行数」这个**位置代理量**（不换算秒）。
    阈值 TAIL_HANG_MIN 见下；帧序列里保留的 t 仅用于展示，不参与任何判定。
    `big_stalls` 仍取内核实测的 period_us，可靠。
    """
    polls = 0
    pts = []
    last_frame_idx = -1
    for i, ln in enumerate(lines):
        m = POLL_RE.search(ln)
        if m:
            polls = int(m.group(1))
            continue
        m = FRAME_RE.search(ln)
        if m:
            pts.append((polls * 0.5, int(m.group(1)), int(m.group(2))))
            last_frame_idx = i
    tail = lines[last_frame_idx + 1:] if last_frame_idx >= 0 else []
    tail_hang = sum(1 for ln in tail if "hangcheck" in ln)
    return pts, tail_hang


def split_sessions(lines):
    """按首个 Darwin Kernel Version 横幅切会话；横幅前的行归第 0 会话。"""
    idx = [i for i, ln in enumerate(lines) if ln.lstrip("\x00").startswith(BANNER)]
    if not idx:
        return [lines] if lines else []
    sessions = []
    if idx[0] > 0:
        sessions.append(lines[:idx[0]])
    for n, start in enumerate(idx):
        end = idx[n + 1] if n + 1 < len(idx) else len(lines)
        sessions.append(lines[start:end])
    return sessions


def classify(lines):
    c = {k: 0 for k in MARKERS}
    for ln in lines:
        for k, needle in MARKERS.items():
            if needle in ln:
                c[k] += 1
    banner = ""
    for ln in lines:
        if ln.lstrip("\x00").startswith(BANNER):
            m = re.search(r"Darwin Kernel Version[^\n]{0,90}", ln)
            banner = m.group(0).strip() if m else ""
            break

    pts, tail_hang = frame_timeline(lines)
    # 末态判定：出过帧、但末帧之后还在持续刷 hangcheck（>= TAIL_HANG_MIN 行）
    # ⇒ 判定为「跑了一阵又挂住」。这是行数代理量，不是秒；不依赖 polls 数值。
    c["frames"] = len(pts)
    c["last_frame_t"] = pts[-1][0] if pts else 0.0   # 仅展示，非真实秒
    c["end_t"] = 0.0                                # 废弃：polls 时钟不可用
    c["tail_hang"] = tail_hang
    c["big_stalls"] = sum(1 for _t, _n, p in pts if p > 1_000_000)
    stalled_end = bool(pts) and tail_hang >= TAIL_HANG_MIN

    if c["panic"]:
        verdict = "DATA_IDENTITY_PANIC" if (c["data_panic"] or c["data_err2"]) else "OTHER_PANIC"
    elif pts and not stalled_end:
        verdict = "CLEAN"                      # 出过帧，且末态仍在出帧
    elif pts and stalled_end:
        verdict = "GPU_STALL_AFTER_FRAMES"     # 跑过一段后挂住（第 20 会话型）
    elif c["stage4"] and c["springboard"]:
        verdict = "GPU_STALL"                  # DATA 挂上了、SB 起了，但一帧都没有
    elif not c["stage4"] and (c["provider_ram"] or c["seed_retry"] or c["hangcheck"]):
        verdict = "SEED_STALL"                 # DATA 没挂上就卡住
    else:
        verdict = "UNKNOWN"
    return verdict, c, banner


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--dir")
    ap.add_argument("--glob", default="run-*.log")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    files = list(a.logs)
    if a.dir:
        files += sorted(glob.glob(os.path.join(a.dir, a.glob)))
    if not files:
        print("ERROR: no log given", file=sys.stderr)
        return 1

    overall_verdict = "CLEAN"
    counted_sessions = 0
    out = []
    for path in files:
        if not os.path.isfile(path):
            print("ERROR: missing %s" % path, file=sys.stderr)
            return 1
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        sessions = split_sessions(lines)
        rec = []
        for n, seg in enumerate(sessions):
            v, c, banner = classify(seg)
            rec.append({"session": n, "verdict": v, "lines": len(seg),
                        "banner": banner, "markers": c})
            # 记录器 RECORDER START / 横幅前导段不是内核会话：保留在明细里，
            # 但不参与整体判据，否则 UNKNOWN(1) 会盖掉 CLEAN(0)。
            if v == "UNKNOWN" and not banner:
                continue
            counted_sessions += 1
            if SEVERITY[v] > SEVERITY[overall_verdict]:
                overall_verdict = v
        out.append({"file": path, "sessions": rec})
    if counted_sessions == 0:
        overall_verdict = "UNKNOWN"

    for entry in out:
        print("=== %s ===" % os.path.basename(entry["file"]))
        print("  sessions=%d" % len(entry["sessions"]))
        for r in entry["sessions"]:
            c = r["markers"]
            print("  [%d] %-22s frames=%-5d 末帧后hang=%-4d 大停%-2d stage4=%d panic=%d" %
                  (r["session"], r["verdict"], c.get("frames", 0),
                   c.get("tail_hang", 0), c.get("big_stalls", 0),
                   c["stage4"], c["panic"]))
            if r["banner"]:
                print("      banner: %s" % r["banner"][:88])
    overall = RC[overall_verdict]
    print("\nOVERALL_VERDICT=%s  OVERALL_RC=%d" % (overall_verdict, overall))
    if a.json:
        import json
        print(json.dumps(out, ensure_ascii=False, indent=2))
    return overall


if __name__ == "__main__":
    sys.exit(main())
