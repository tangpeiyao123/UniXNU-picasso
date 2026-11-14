#!/usr/bin/env python3
"""wdt 会话「续观察」判据：设备仍在跑某个镜像时，只盯下一次复位。

用途（2026-11-13 14:55）：14:34 的 `observe_boot.py --wdt --baseline 25 --budget 720`
已在 14:48:50 以 `NO_RESET rc=6` 退出，但设备**仍在跑 wdt=1 镜像**，且第 26 会话
14:54 两次取样完全静止（帧/字节/polls 全不变）、出现 127.2 s 停顿。此时重新
`fastboot boot` 会引入新变量并丢掉这个零成本窗口；正确做法是续观察，不碰设备。

本脚本纯读日志文件，不发串口指令、不动设备、不写设备。它与 `observe_boot.py`
的区别：没有「等本次 boot 的 banner」阶段——会话已经在跑了，起点就是现在。

判定（出现第 baseline+1 个 banner 即复位）：
  WDT_RECOVERED      rc=0  复位且前一会话有 watchdog/checkin panic
  PANIC_RESET_OTHER  rc=3  复位且前一会话有其它 panic
  SILENT_RESET       rc=2  复位但无 panic（来源未知，可能是人工断电/硬件）
  NO_RESET           rc=6  budget 内没有新 banner（没测成，不是 wdt 无效）

用法：
  python watch_wdt_reset.py --log <console.log> --baseline 26 --budget 600 --poll 5
"""
import argparse
import os
import re
import sys
import time

BANNER_RE = re.compile(r"(?m)^Darwin Kernel Version")
FRAME_RE = re.compile(r"PICASSO_DPU_FRAME n=(\d+)")
WDT_PANIC_RE = re.compile(r"watchdog|checkin", re.I)

RC = {
    "WDT_RECOVERED": 0,
    "SILENT_RESET": 2,
    "PANIC_RESET_OTHER": 3,
    "NO_RESET": 6,
}
MEAN = {
    "WDT_RECOVERED": "机制生效：卡死后由 watchdog panic 复位",
    "PANIC_RESET_OTHER": "没测成：复位来自别的 panic，不是 watchdog",
    "SILENT_RESET": "没测成：复位无 panic，来源未知",
    "NO_RESET": "没测成：窗口内没有复位（不能写成 wdt 无效）",
}


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def session_text(text, index):
    starts = [m.start() for m in BANNER_RE.finditer(text)]
    if index >= len(starts):
        return None
    end = starts[index + 1] if index + 1 < len(starts) else len(text)
    return text[starts[index]:end]


def now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--baseline", type=int, required=True,
                    help="启动时的 banner 总数；被测会话索引 = baseline-1。"
                         "例：当前共 26 个 banner、正在跑第 26 会话 → --baseline 26")
    ap.add_argument("--budget", type=float, default=600.0)
    ap.add_argument("--poll", type=float, default=5.0)
    ap.add_argument("--record", default="",
                    help="记录文件；默认写到日志同目录 wdt-resume-<时间>.log")
    a = ap.parse_args()

    # baseline 语义 = 启动时的 banner 总数；被测会话索引 = baseline-1
    cur_index = a.baseline - 1
    rec_path = a.record or os.path.join(
        os.path.dirname(a.log),
        "wdt-resume-%s.log" % time.strftime("%Y%m%d-%H%M%S"))
    rec = open(rec_path, "w", encoding="utf-8")

    def emit(s):
        print("[%s] %s" % (now(), s), flush=True)
        rec.write("[%s] %s\n" % (now(), s))
        rec.flush()

    t0 = time.time()
    txt0 = read(a.log)
    n0 = len(BANNER_RE.findall(txt0))
    sess0 = session_text(txt0, cur_index) or ""
    frames0 = len(FRAME_RE.findall(sess0))
    emit("续观察启动：banner=%d 被测会话=#%d 起始帧=%d budget=%.0fs 记录=%s"
         % (n0, a.baseline, frames0, a.budget, os.path.basename(rec_path)))
    if n0 != a.baseline:
        emit("WARN: 实测 banner=%d 与 --baseline %d 不一致，请复核" % (n0, a.baseline))

    verdict = "NO_RESET"
    rc = RC[verdict]
    try:
        while True:
            txt = read(a.log)
            n = len(BANNER_RE.findall(txt))
            cur = session_text(txt, cur_index) or ""
            frames = len(FRAME_RE.findall(cur))
            el = time.time() - t0
            emit("t=%5.0fs banners=%d frames=%-5d (起始 %d)"
                 % (el, n, frames, frames0))

            if n > a.baseline:
                prev = session_text(txt, cur_index) or ""
                pans = [l for l in prev.splitlines() if l.startswith("panic(")]
                wdt_pans = [l for l in pans if WDT_PANIC_RE.search(l)]
                gap = time.time() - t0
                if wdt_pans:
                    verdict = "WDT_RECOVERED"
                elif pans:
                    verdict = "PANIC_RESET_OTHER"
                else:
                    verdict = "SILENT_RESET"
                rc = RC[verdict]
                emit("出现新 banner（复位）：末帧→复位约 %.0fs panic=%d watchdog-panic=%d"
                     % (gap, len(pans), len(wdt_pans)))
                if wdt_pans:
                    emit("  首条: " + wdt_pans[0][:160])
                break
            if el >= a.budget:
                emit("budget 内无复位")
                break
            time.sleep(a.poll)
    finally:
        line = "VERDICT=%s RC=%d  %s  用时 %.0fs" % (
            verdict, rc, MEAN[verdict], time.time() - t0)
        print(line, flush=True)
        rec.write(line + "\n")
        rec.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
