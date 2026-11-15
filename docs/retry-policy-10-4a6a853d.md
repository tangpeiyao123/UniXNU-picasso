# 连续重试值不值：10 轮干净真机 A/B

镜像 `4a6a853d`，2026-11-15 09:47–12:10，单变量＝**判出 STALL 后是否立刻再试**，最多 5 次/轮。
`--quiesce 0 --charge-pause 0 --window 300 --min-frames 2`，逐 attempt 实时落盘。

复跑：

```
python3 scripts/reproduce_desktop.py --log <new.log> --cycles 10 --retry-until-pass 5 \
  --window 300 --min-frames 2 --quiesce 0 --charge-pause 0 \
  --cmd-file D:/k30i-gpu/console/send-cmd.txt \
  --out-csv docs/evidence/retry-b-clean-20261115.csv
```

## 结论

**连续重试成立，但它买到的是"轮级成功率"，不是"单次成功率"。**

| 指标 | A 组（单次，47 段） | B 组（最多 5 次/轮，10 轮） |
|---|---|---|
| 单次 attempt 成功率 | 18/47 = **38.3%** | 9/26 = **34.6%**（Fisher p=0.805） |
| 轮级成功率 | 38.3%（一次定生死） | **9/10 = 90%** |
| 到桌面的墙钟 | 单次 ≤300s | 中位 **618s**，Q1 202s，Q3 890s，max 1685s |

单次成功率两组无差异（p=0.805）⇒ **变量没被污染**，90% 的轮级成功率不是"把单次变容易了"得来的，
而是"允许重试"这一件事本身的收益。

## 逐轮明细

| 轮 | 次数 | 到 PASS 墙钟 |
|---|---|---|
| 1 | 2 | 618s |
| 2 | 1 | 159s |
| 3 | 5 | 1685s |
| 4 | 3 | 890s |
| 5 | 1 | 202s |
| 6 | 3 | 919s |
| 7 | 1 | 188s |
| 8 | 2 | 479s |
| 9 | 3 | 798s |
| 10 | 5 | **未成功**（5 次用尽，末次 STALL_GPU @1653s） |

## 代价与边界

- **时间成本明显**：中位 10.3 分钟才见桌面，最坏一轮 28 分钟且仍失败；
- **"5 次必成 92%"不成立**：那是独立同分布的算术期望，实测 9/10（n=10，二项 95% 区间约 55–99.7%），
  第 10 轮就是反例；
- 失败的那一轮 5 次里有 3 次 SEED——连续重试不能绕过 240 秒的 seed 等待，只能"再抽一次签"。

## 建议

对**无人值守**场景用重试（90% 比 38% 值这 10 分钟）；对**人在旁边**场景保持单次（300 秒内就有结论，失败再手动）。

## 已知缺陷（不影响上面结论）

CSV 的 `frames` 列恒为 0——写行时硬编码了，没取实际值。`verdict` 由 `judge()` 给出（PASS 即 frames≥2），
所以结论不受影响，但该列不可用，修好前不要引用。


---

## 附录 C｜SEED 早停：实现取错锚点，判据本身可用（2026-11-15 17:0x 更正）

首版实现取 **最后一条** `NATIVE_OWNER_DATA` 并看到段尾，导致 17/17 次尝试全部 `STALL_SEED_EARLY`，
该批数据作废（`docs/evidence/retry-seedabort-INVALID_HOST_BUG.csv`）。

**这是实现错误，不是判据错误。** 正确规则是：**第一条** `NATIVE_OWNER_DATA` 之后的 **200 行窗口**内
没有 `DATA_INIT`。闸门脚本 `scripts/check_seed_abort_gate.py` 在三份日志（47+28+20 段）实跑：

```
111028  segs=47 fire=10 true_seed=9 false_kill=0 other_stall=1 missed_seed=0
094709  segs=28 fire=10 true_seed=10 false_kill=0 other_stall=0 missed_seed=0
140252  segs=20 fire= 7 true_seed= 1 false_kill=0 other_stall=6 missed_seed=0
GATE=PASS  RC=0
```

误杀口径按产品定义：**只有"本会 frame-PASS 却被早停"才算误杀**；早停一个本来也会 STALL 的段不损失可用率。
`other_stall`（非 SEED 的失败段被早停）单列，不计入误杀。
