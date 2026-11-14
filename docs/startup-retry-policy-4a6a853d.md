# 启动重试策略：能不能靠"多等一会儿再重启"提高可用率

镜像 `4a6a853d`（未改动）。数据：`D:/k30i-gpu/console/console-20261114-111028.log`，
47 段会话，A/B 同文件、同镜像、同窗口、同判据，唯一变量是**判定后到重启之间的静置时长**。

复跑：

```
python scripts/startup-policy-report.py --log D:/k30i-gpu/console/console-20261114-111028.log \
  --a 2-31 --b 33-47
```

## 统计口径（剔除规则写在这里，不静默排除）

- 全量 47 段：`measure` 报 PASS 24 / STALL 21 / TRUNC 2；剔除 TRUNC 后 VALID 45，
  armed 24/45 = 53.3%、frame 18/45 = 40.0%。
- A/B 只取 banner #2–#31 与 #33–#47：#1 是插线前那半段（会话中途开始，无 banner 起点），
  #32 是被主机重启截断的那一段（TRUNC）。两段都不属于任何一组，理由即此。
- 分型（全量，修正后）：SEED 9 / AFTER_DRAWABLE 6 / NO_DRAWABLE 4 / **WATCHDOG 1** / SILENT 1。

## 结论（一句话）

**静置 300 秒没有提高任何指标，三项 Fisher 检验全部不显著。** 现有镜像下，重启策略不是可用率的杠杆。

| 指标 | A 组（静置 5 s，n=30） | B 组（静置 300 s，n=14 有效） | Fisher 双侧 p |
|---|---|---|---|
| frame-PASS（真出帧） | 13/30 = 43.3% | 5/14 = 35.7% | **0.748** |
| SEED panic | 5/30 = 16.7% | 4/14 = 28.6% | **0.434** |
| NO_DRAWABLE | 4/30 = 13.3% | 0/14 = 0% | **0.290** |

p 全部远大于 0.05。**"上一次会话被过早打断、journal 未落盘，因此抬高下一次 SEED 率"这个假设被否掉**。

> 修正（2026-11-14 18:5x）：A 组原本报 6 次 SEED，其中 1 次是 `picasso-watchdog: hardware pet rejected @PicassoWatchdog.cpp:128`，
> 被旧分类器按 `startswith("panic(")` 吞进 SEED。拆分后 **A 组 SEED=5、WATCHDOG=1**，Fisher 由 0.701 变为 **0.434**，结论不变。
NO_DRAWABLE 从 4 次降到 0 次看起来像改善，但 p=0.29，不能写成有效——n=14 撑不起这个结论。

## 明细（脚本原样输出）

```
A  n=30 valid=30  armed=16  frame=13  SEED=6  NO_DRAWABLE=4  AFTER_DRAWABLE=3  SILENT=1  TRUNC=0
   stalls: texk=[1,1,0,0,0,0,0,0,0,0,1,0,0,0] agx=[2,2,0,2,2,0,0,2,0,2,2,2,0,0] fbuc_max=21
B  n=15 valid=14  armed=7   frame=5   SEED=4  NO_DRAWABLE=0  AFTER_DRAWABLE=3  SILENT=0  TRUNC=1
   stalls: texk=[1,0,0,1,1,0,0]               agx=[2,0,0,2,2,0,0]              fbuc_max=21
```

## P3 分层：三类 STALL 有没有随静置变化

| 类别 | A | B | 判读 |
|---|---|---|---|
| SEED | 6 | 4 | 与静置无关（p=0.70） |
| NO_DRAWABLE | 4 | 0 | 表面改善，p=0.29，**不成立** |
| AFTER_DRAWABLE（texk 有、0 帧） | 3 | 3 | 比例 10% → 21%，同样不显著 |

STALL 段里 `agx` 多为 2（PASS 为 3），`fbuc_max` 多在 19–21 —— 但**不是全部**：
`fbuc call=` 的最大值有 34（s01、s43）与 38（s47）的个例，也有 PASS 段只到 34（s04）。
所以只能写"典型值 19–21、与 PASS 的 160+ 量级不同"，不能写"所有"。
`texk` 只在 AFTER_DRAWABLE 里为 1。这三条与之前两次分析一致，说明**断点位置没变，变的只是有没有走到**。

## 那么可用率到底是多少

同一镜像、不改变任何东西、判据为"真的出帧"：

- A 组（当前策略，判出即重启）：**43.3%**（13/30）；
- B 组（多等 5 分钟）：35.7%，更低但不显著。

**建议保持现状（判出即重启）**：等待没有收益，只会把一轮从 5 分钟拉长到 11 分钟。

## 已知代价（不是结论，是事实）

- 这台机器上 `check_rollback_ready.py --device` 的门禁是 `GATE=PASS`（hard_fail=0/device_fail=0），但 **EDL firehose 仍缺**，所以任何让 fastboot 失联的操作都没有救援；
- 想再往上走（NO_DRAWABLE / AFTER_DRAWABLE 合计约 17–24%）必须回答"卡在哪个调用"，而这需要 `threads` 探针进镜像——本轮被 Compose 输入门（`ValueError: No compatible iOS16 Compose input bundle`）挡在构建前，详见 `docs/evidence/ci-run-43541458190.log`。

## 最小必要人工动作（如果决定继续推进）

1. 在 macOS 侧跑 `compose-bootimg.yml`（`create_data_seed=true`）为新树铸输入包 —— 这会改变 Data identity，需先接受；
2. 重跑 Linux CI，确认过 step 8 并真正进编译器（`saved_state` 类型由此回答）；
3. `fastboot boot` 临时引导（不 flash），在 STALL 窗口发 `threads` / `kthreads` 抓栈；
4. 断电回 `4a6a853d`，按诊断文档 §12 记录 panic / UUID / stage / 桌面是否还在。

以上四步都不在"改重启策略"这个变量内，属于另一条线。
