# iOS 启动失败处理方向（2026-11-13 12:40，可直接转发给 WorkBuddy 执行）

> 用户指令：**开始处理 iOS 启动失败的问题**；前置要求：给方向、不盲目测试、不创造新 bug。
> 本文所有数字都来自实跑或真实日志，口径与分母一并给出；**纯主机侧部分已完成，不需要再问批准**。

---

## 0. 先落一条我方认错（评审侧，必须写进文件）

12:09:05 / 12:10:02 两次取样看到 `DPU_FRAME n=360` 不动、`fence=1056` 冻结，我把第 20 会话写成
「确认 GPU hang / 永久挂死」。**这是错的**：12:38 独立复核，该会话已到 `n=51120`、末帧
`period_us=15293`（≈65 fps），它在 `n=360 → n=390` 之间停了 `220,122,189 us`（220 s）后自恢复，
恢复时没有新 banner、没有新 `stage=4`、`fence` 从 1056 继续推进。WorkBuddy 12:22 的更正成立。

- 错因：单次快照 + 未取趋势。已在本文 §7 写成流程约束。
- 但更正口径也要补一句：恢复后**不是**「一路 60 fps」。#20 共 13 次 >1 s 停顿，其中 1 次 220 s，
  另有 12 次 `period_us≈3.0 s`，固定每 **720 帧**出现一次（n=11310/12030/…/19230）。
  WorkBuddy 正文「一路 60 fps」与其表格「13 次大停顿」口径不一致，应改为
  「中位数 16.58 ms，但有周期性 3 s 卡顿」。

---

## 1. 当前基线（12:38 实测，只读）

| 项 | 值 | 证据 |
|---|---|---|
| 设备 | `bed01658  fastboot`（停在 fastboot） | `fastboot devices` 只读枚举 |
| 串口日志 | 停写于 12:28:57，10,732,535 B / 141,793 行 / 20 banner | `console-20261113-070546.log` |
| boot 分区 | `boot-685-pscireset+trustcompat2h-4a6a853d.img`（09:43:47 **flash**，非 `fastboot boot`） | `Writing 'boot' OKAY`，之后无 flash/boot |
| Data | UUID `0EBB496E-FB09-4223-991E-22007B6FACD1` / `stage=4 error=0` / 20 GiB | 第 20 会话日志 |
| 安卓回退 | `ANDROID_ROLLBACK=DEGRADED`，`RC=3`；14 项 HARD 全 PASS，仅 EDL 火种缺 | `check_android_rollback.py` 实跑 |
| 冻结令 | `incident-20261113-lessons.md` §十一 **仍生效**（mtime 11:20:14）：禁止重编内核 / CI 出包 | 原文 |

`device-info.txt`（12:30:47，`getvar all` 只读，240 行）已落盘；`slot` 出现 0 次、
`partition-type:boot:raw`、`partition-size:boot=0x8000000`（128 MiB，与原厂 boot 134,217,728 B 吻合）。
「单槽机型」是**高置信推断**而非硬件证明，⏳ 别写成定论；但足以确定 L2 回退不需要 `--set-active`。

---

## 2. 启动失败的证据分层（全量 20 会话，不挑窗口）

实跑 `python scripts/check_boot_sla.py D:/k30i-gpu/console/console-20261113-070546.log`
（`py_compile RC=0`，`OVERALL_RC=2`）：

| 类别 | 会话 | 特征 |
|---|---|---|
| OK | #1, #12 | 683 / 1000 条帧采样、无 >1 s 停顿 |
| SEED_PANIC | #4,6,8,10,15,16（6） | `panic` + `offline scratch Data mount failed: 2` / `stage=0 error=2`，等 210–240 s 后自重启 |
| GPU_NO_FRAME | #5,7,11,13,19（5） | `stage=4` + `SpringBoard started` + `PICASSO_DATASTORE`×4，**0 帧**；`hangcheck polls` 一直涨到 357–874；观察 178–437 s 后被断电（末尾都是下一次 `PORT OPEN` / `KERNEL BANNER`） |
| TOO_SHORT | #2,3,9,14,17,18（6） | 出帧但只有 4–57 条采样（#17 仅 4 条） |
| STALL | #20（1） | 220 s 停顿后自恢复 + 12 次周期性 3 s 停顿 |

**三档成功率（分母都是 20，阈值敏感性已实跑）**

```
LAX      (出过帧 + SB + stage4)              :  9/20 = 45.0%   [1,2,3,9,12,14,17,18,20]
STRICT   (+采样>=60, 无>60s停顿)             :  2/20 = 10.0%   [1,12]
AVAILABLE(+无>1s停顿)                        :  2/20 = 10.0%   [1,12]
min_samples=30 → STRICT 25% / AVAILABLE 20%；=60 或 120 → 10%
```

WorkBuddy 12:24 报的 9/20=45% **口径正确、可复现**，但它只是 LAX。用户要的「启动成功率
100%」必须按 **STRICT** 验收，否则把「4 帧就算成功」和「中间停 220 s」都算达标。
另：WorkBuddy 新加的「末态」判据在当前 20 会话上**没有改变任何分类**（仍是 9/6/5），
它是防御未来的；我用合成样本做了它没有做的负向对照：
`sample-stall-120s.log` → `GPU_STALL_AFTER_FRAMES / RC=4`（停顿持续 >120 s 时正确拦截），
完整 #20 → `CLEAN / RC=0`。**判据有效，可继续用**。

### 可证伪假设 H1（本轮最值钱的一条）

> 5 个 `GPU_NO_FRAME` 会话与 #20 的 220 s 停顿，**是同一现象的不同观察窗口**，不是两类独立故障。

依据：#20 恢复全程无新 banner / 无新 stage / `fence` 从 1056 继续 / `hangcheck polls`
357→740 一直涨；而 5 个 0 帧会话同样 polls 持续涨（357–874），观察时长 178–437 s，
与「恢复需要 220 s」同一量级，且都是被人长按断电结束的。

若 H1 成立，**GPU 类故障不需要改内核，只需要等**——这就把「两类故障分别立项」的成本
降为一次零变量实验。H1 必须被证伪才允许进入内核改动。

---

## 3. 阶段 A：立即开工（零改动、单变量、用户已授权「开始处理」）

**实验名 `observe-no-power-cut`；唯一变量 = 观察时长。**

设备此刻已在 fastboot，从分区启动即可，不写任何分区：

```bash
/d/ADB/fastboot.exe reboot
```

规则（写死，不许临场变通）：

1. 记录器实例保持运行不动；每次启动落**独立日志**，按内容命名：
   `D:/k30i-gpu/console/boot-observe-<YYYYMMDD-HHMMSS>-<sha8>.log`，禁止覆盖旧文件。
2. **出现 0 帧时不要长按断电**。持续观察 **≥600 s**（历史最大观察 437 s，必须超过它），
   每 60 s 记一次三项：`hangcheck polls` / `fence` / 最新 `DPU_FRAME n`。
3. 判据（写进脚本并真跑，命令不接管道，取真实 `$?`）：
   - 600 s 内出现 `DPU_FRAME` 且 fence 推进 → **H1 支持**；记录从 0 帧到首帧的实际秒数。
   - 600 s 仍 0 帧但 polls 持续涨 → **H1 不支持**，GPU 类独立成立，才允许立项查驱动。
   - 出现 `panic` → 归入 seed 类，按其既有自重启处理，本轮不算 H1 样本。
4. 停止条件：出帧后稳定 60 s，或 600 s 到期，或 panic。**任一命中即停，不改参数再试、不换镜像**。
5. 每次会话结束后两个判据都跑，全量输出留档：
   ```bash
   cd /d/k30i-gpu/repo-work
   python scripts/check_boot_session.py <该次日志> ; echo "SESSION_RC=$?"
   python scripts/check_boot_sla.py <该次日志> --min-samples 60 --max-stall-s 60 ; echo "SLA_RC=$?"
   ```
   `check_boot_session.py` 负责**进行中**的末态判定，`check_boot_sla.py` 负责**事后全量验收**，
   两者互补，不要互相替代。

禁止清单：改 boot-args、换镜像、`fastboot boot`、`flash boot`、`erase userdata`、
重编内核 / CI 出包、动 PnP、停记录器、加日志探针、改判据阈值。

---

## 4. 阶段 B：纯主机侧，与上机并行（不碰设备，也不需要批准）

1. **周期性 3 s 停顿归因**：取 #20 的 n=11310/12030/…/19230 前后各 50 行，找共同事件
   （`submits live` / watchdog / `IOService` / `IOLock` / vsync）。固定 720 帧间隔像定时器，
   这是目前唯一有规律的线索，先做分层统计，不下结论。
2. **首帧延迟分布**：全量 20 会话的 banner→首帧时间（#20 是 104 s），看是否与 seed/Data 路径相关。
3. **seed 类 240 s 链路**：`picasso-native-ram-block-mode1-guard.patch` 至少三条等待
   `:208 waited<120 × IOSleep(500)` = 60 s、`:303 waited++<2400 × IOSleep(100)` = 240 s、
   `:312 waitForMatchingService(...,240000000000ULL)`；`picasso-seed-diag-ramblock.patch:19`
   还有第二份 `<2400`。**只列证据，不改数字**（「改一个数字就能 4 分钟→30 秒」已被推翻）。
4. 产物：`docs/boot-failure-evidence-20261113.md`，只写实测行号与计数，推测单列「未验证」。

---

## 5. 阶段 C：冻结令未解除前禁止（需用户显式批准）

只有当 H1 被证伪、且确认需要内核态日志时才走。前置条件全部满足才可提：

- L3 补齐 EDL 火种（`prog_firehose*.elf` / `rawprogram*.xml`），或用户明确接受 L2 代价：
  `fastboot -w` 会清掉 Data `0EBB496E` 与当前桌面，回 iOS 需重刷 `userdata-root.sparse.img`
  8.38 GB（实测 218 s）——**不是重启就回来**。
- 新内核门禁 `RC=0 → RC=3`；出包后 TRU 字节比对 + CI 是否沿用 `--compat-file` 已验证。
- 可回滚镜像路径、Data 基线记录（UUID / stage / geometry）已落盘。
- **禁止**用「刷完 Data UUID 仍是 0EBB496E / `DATA_INIT phase=2`」证明身份不变
  （06:52 已确认：UUID/seed 相同都不算安全，风险在内核侧 seedHash）。

EDL 原厂 ROM 下载可作为**后台并行任务**（不碰设备），但它不是阶段 A 的阻塞项，不要抢主线。

---

## 6. 「启动成功率 100%」的验收定义（今天定死）

1. 固定变量：同一内核 `trustcompat2h`、**全部走分区启动**（不再 `fastboot boot`）、同一 Data。
2. 样本：连续 **≥10 次纯重启**，每次独立日志，逐次报全量，不许挑窗口。
3. 判据：`check_boot_sla.py --min-samples 60 --max-stall-s 60` 全量
   `STRICT = 10/10` 且 `OVERALL_RC=0`；`SEED_PANIC=0`、`GPU_NO_FRAME=0`、`STALL=0`。
4. 对外口径：达标前只说「LAX 45% / STRICT 10%（2/20）」，**两个数字一起报**，不写「稳定」。
5. 两类故障分别消除，各自单变量对照；seed 类与 GPU 类不得混在一版改动里。

---

## 7. 纪律（违反即停）

- 状态判断必须**两次取样 + 带时间戳**，格式「截至 HH:MM:SS」；一次读数不许下「挂死/掉线/停帧」结论。
- 一次只改一个变量；没有对照就写「未验证」，不写「已坐实」。
- 判据写进脚本并真跑，命令不接管道，给真实退出码。
- 统计报全量；换口径必须同时给分母与阈值敏感性。
- 产物按内容命名，不覆盖。
- 认错落在文件/代码里，不只在对话里说。

---

## 8. 本轮已实跑、可直接复现的命令

```bash
cd /d/k30i-gpu/repo-work
python -m py_compile scripts/check_boot_sla.py                       # RC=0
python scripts/check_boot_sla.py D:/k30i-gpu/console/console-20261113-070546.log   # RC=2, LAX 45% / STRICT 10%
python scripts/check_boot_sla.py D:/k30i-gpu/console/console-20261113-070546.log --min-samples 30  # STRICT 25%
python scripts/check_boot_session.py D:/k30i-gpu/console/console-20261113-070546.log  # RC=2, 9/6/5
python scripts/check_android_rollback.py                            # RC=3 DEGRADED
```

负向对照（合成样本放在 `tests/fixtures/`，已移出 `console/`，**非真实串口日志，勿计入统计**）：
`tests/fixtures/sample-gpu-noframe.log` → `0/1 = 0%`、`RC=3 GPU_NO_FRAME`；
`tests/fixtures/sample-stall-120s.log` → `check_boot_session.py RC=4 GPU_STALL_AFTER_FRAMES`；
`tests/fixtures/sample-session20-full.log` → `RC=0 CLEAN`。

---

## 9. 13:00 判据修订（评审侧，已实跑；请 WorkBuddy 统一改用新版）

WorkBuddy 12:53 自己发现 `hangcheck polls` 会限流停打印（12:52:45→12:53:30 间隔 45 s，
`polls/fence` 固定 (217,116)，帧采样 14→33、末 n 420→990）。这个发现**反噬了两条正在用的判据**：

**决定性对照（单变量，帧内容一字未改）**：取「健康出帧」样本
`tests/fixtures/sample-live-frames-pollstall.log`，仅在末尾追加 6 行 polls 更大的
hangcheck → 旧 `check_boot_session.py` 从 `CLEAN / RC=0` 翻成
`GPU_STALL_AFTER_FRAMES / RC=4`（末帧@108s/450s）。即旧判据看的是 polls 数值，不是设备状态。

已修（改在代码里，不是口头）：

| 脚本 | 修复 |
|---|---|
| `scripts/check_boot_session.py` | 废弃 `polls × 0.5s` 时钟；末态改判「末帧之后 hangcheck 行数 ≥ `TAIL_HANG_MIN=120`」（位置代理量，不换算秒）；`big_stalls` 仍用内核实测 `period_us` |
| `scripts/check_boot_sla.py` | `STRICT` / `AVAILABLE` 增加「末态未停住」条件；新增 `STALLED_END` 分类，报表标签与验收口径对齐 |

`py_compile RC=0`；9 组回归对照全部通过：
- 健康样本 / 加 6 行样本 → `CLEAN RC=0`（不再被 6 行骗）
- 真停顿样本（末帧后 220 行 hangcheck）→ `GPU_STALL_AFTER_FRAMES RC=4`
- 完整 #20 → `CLEAN RC=0`；0 帧 → `GPU_STALL RC=4`；空日志 → `UNKNOWN RC=6`

**全量 21 会话（新版口径）**：
```
LAX 10/21 = 47.6%   STRICT 2/21 = 9.5%   AVAILABLE 2/21 = 9.5%
failure classes: OK 2 / TOO_SHORT 6 / SEED_PANIC 6 / GPU_NO_FRAME 5 / STALL 1 / STALLED_END 1
check_boot_sla.py OVERALL_RC=2   check_boot_session.py OVERALL_RC=2
```
第 21 会话被正确剔除出 STRICT：`frames=125`、末 n=3750，**末帧之后 512 行无新帧、全是 hangcheck**。

**给 WorkBuddy 的三条待改**：
1. `observe_boot.py` 的 `H1_SUPPORTED`（要求 fence 推进）与 `H1_FALSIFIED`（要求 `polls>0`）
   都建立在已证伪的 polls/fence 时钟上；改用帧数 / 末 n 增长 + 脚本自身的 wall clock。
2. 该脚本 12:52:34 判定 `H1_SUPPORTED` 即 `return 0` 退出，**没盯到 #21 后来又停住**；
   应持续观察到 deadline 或改为「判定后继续记录末态」。
3. §3 要求「每次启动落独立日志、按内容命名」未落实——仍用主日志 + baseline banner 计数；
   21 个会话混在一个文件里，切会话本身已是风险源。

**H1 结论口径**：第 21 会话 181 s 自出帧是「H1 获得支持」的一个实例；历史 5 个 0 帧会话
观察 178–437 s（且按 WorkBuddy 自己的口径修正，这些只是下限），其中 4 个 ≥350 s，
远超 181 s。单次样本**不足以**把 GPU 类并进 seed 类，仍需按 §3 继续取样。

---

## 10. 13:30 评审：`vsync: kick` 可用，但「出帧 ≠ 进桌面」（本轮最重）

### 独立复核（按行首 banner 切，25 会话，不挑窗口）

`PICASSO_DISPLAY vsync: kick` 与「是否**出过帧**」的相关性**确实零例外**：
13 个有帧会话 `kick=8`（打印上限），12 个 0 帧会话 `kick=0`。
第 25 会话是 `kick` 先到、`帧` 后到（先 kick=8/0 帧，随后 frames=1），**不是反例**，
反而说明 kick 是良好的**预测标志**。`metrics installed` 确实不能当判据（多个 0 帧会话 =1）。

但 WorkBuddy 13:28 的表格只列 12 + 6 = 18 个会话，**漏掉 6 个 SEED_PANIC 会话**
（#4,6,8,10,15,16，均 kick=0 / 0 帧）；13:27 说「进不去的 5 个」、13:28 列 6 个编号，
且只报 24 会话（实测已 25）。报表不完整、分母不透明——这正是 10:27 因「把 panic 会话
混进 kick 相关性」被撤回过的同类口径问题。相关性结论不受影响，但**必须报全量**。

### 三处硬伤（都在 `observe_boot.py` 13:27 版）

1. **把「出过第一帧」当成「进桌面」**。`DESKTOP` 判据是「帧数增长 + `kick>0` → 立即 `rc=0`」。
   全量 25 会话中 #22（6 帧后停）、#23（1 帧）、#25（21 帧后停）都是 `kick=8`、出过帧、
   判 `DESKTOP`，但用户看到的是**静止画面、没进桌面**。用户 13:20 问的是「是否进入桌面」，
   判据却只判「出过第一帧」。全量 `STRICT 2/25 = 8.0%`（LAX 13/25 = 52.0%）。
   → 折中：`DESKTOP` = `kick>0` + **连续出帧 ≥60 s**（或 ≥N 帧）且期间无 >60 s 停顿；
   `kick+首帧` 只作 `EARLY_FRAME` 进度标志。总用时约 60–90 s，仍远优于 660 s。
2. **`NO_DRAW` 计时起点错**。代码 `sb_seen = time.time()` 取的是「脚本启动后首次看到
   SB 字符串的时刻」，`--current` 模式下恒为 t=0。`boot-observe-20261113-132730` 里
   t=0 时 `SB=1` 已成立（会话 13:17 起、脚本 13:27 才启动），47 s 后判 `NO_DRAW`——
   所谓「SpringBoard 起来后 45 秒」实际是「脚本启动后 47 秒」，SB 事件已过去约 10 分钟。
   且 `vsync: kick` 行格式为 `PICASSO_DISPLAY vsync: kick wanted=0 armed=0`，**没有时间戳**，
   45 s 阈值无任何日志证据。「24 样本零例外」证明的是有/无相关，不是「kick 出现在 45 s 内」。
3. **31 秒就判 `H1_FALSIFIED`**。`boot-observe-20261113-132302` 在 t=31 s 返回
   `VERDICT=H1_FALSIFIED RC=4`，而 H1 的证伪条件是「0 帧撑过 600 s 仍不出帧」
   （WorkBuddy 12:53 自己写的）。把「没等到」写成「假设被证伪」，是逻辑倒退。
   → 改名为 `NO_FRAME_YET` / `BUDGET_EXCEEDED`；只有 ≥600 s 才允许写 FALSIFIED。

### 其它核验

- **独立日志已落实**（5 份 `boot-observe-<时间>-<sha8>.log`），但 `131010`（99 B / 1 行）
  与 `132130`（12 行 / 到 57 s 断）**都没有 `VERDICT`/`RC`**，5 份里只有 3 份有结论；
  与上轮 06:31 僵死实例同类问题重演，异常退出必须 `try/finally` 落退出码。
- **开机文字 = `-v`**：实跑 `patch_args.py ... --show` `RC=0`，首条确为
  `-v debug=0x14e`，`picasso-boot-logo-hold=1` 也在；两份 `picasso-quiet-boot*-xnu.patch`
  与 `PE_parse_boot_argn("picasso-probes", ...)` 真实存在。归因正确、风险提示（去掉 `-v`
  会丢掉串口不稳时唯一诊断通道）合理；「省几秒」无测量，⏳。
- ⚠️ **但 `picasso-probes=0` 加不进 boot-args**：`patch_args.py` `LIMIT=1000`，当前
  `995 B / 52 tokens`；新增 ` picasso-probes=0` 约 17 B → 1012 B 超限，即使删 `-v`
  省 3 B 仍 1009 B。且该参数依赖 quiet-boot 补丁是否已编入当前 685 内核（未验证）。
  两项都属改 boot-args，冻结令 §十一 明令禁止，优先级应低于启动失败。
- **「连挂三次 12.5% 不算离谱」用错分母**：LAX 50% 把 #22/#23 算成功；按 STRICT 8%
  失败率 92%，连挂三次 ≈78%，结论方向相反。
- 13:00 后**零** fastboot / flash / erase / CI / 改 boot-args 命令（全部 CALL 逐条核对），
  冻结令未破；13:07 后的多次启动均为用户手动，WorkBuddy 在 13:26 用户澄清后正确撤回
  「每 5 分钟自己复位」。

### 当前状态（13:34 两次取样）

第 25 会话 `kick=8`、`frames=21`、末 `n=630`，13:34:29 与 13:34:34 两次取样日志字节数
完全相同、帧不增长 ⇒ **又是「出帧后停住」，设备不在可用桌面**。全量 25 会话：
`LAX 13/25 = 52.0%`、`STRICT 2/25 = 8.0%`、`AVAILABLE 2/25 = 8.0%`；
`check_boot_sla RC=2`、`check_boot_session RC=2`（取最严重会话，非当前）。
冻结令 §十一 仍生效，L1 门禁 `RC=0`（内核未变），恢复 SOP 继续禁用。

---

## 11. 13:52 评审：`wdt=1` 单变量镜像合格，但判据窗口用错了时钟

### ✅ 已独立复核成立的部分

- **判据四处修复真落地**（`observe_boot.py` 13:41 版）：`DESKTOP` = `kick>0` + 连续出帧 ≥60 s
  且无 >60 s 停顿；`NO_DRAW` 如实标注「脚本观察窗口，非日志时间」；`H1_FALSIFIED`
  收窄为 `budget>=600` 才允许，否则 `NO_FRAME_YET`；`try/finally` 必落 `VERDICT/RC`。
  实跑 #25 → `VERDICT=STALLED_END RC=4 用时 62s`（改前会误判 `DESKTOP rc=0`）。
  四条口径（漏 6 个 panic 会话、SB 后 45 s、连挂三次分母 ≈78%）均已认。
- **watchdog 语义读到源码级证据**：`package/PicassoWatchdog.cpp:142-143` 原文
  「Its `wdt` boot flags control userspace checkins; `wdt=0` disables those deadlines.
  A panic never reaches the following hardware pet」；:145 驱动在 1 s poll 里自己
  `petWatchdogHardware()`。日志实测 `picasso-watchdog` 75 次 / `APSS watchdog armed` 25 次 /
  `hardware=15s` 25 次 / `userspace policy follows` 25 次 / **`watchdog timeout` 0 次**
  —— 与 `wdt=0` 禁用 deadlines 一致。
- **单变量镜像干净（我实跑）**：
  ```
  patch_args.py --show 两镜像：995 B / 52 tokens；唯一差异 wdt=0 → wdt=1
  picasso_usb_reset=psci 两边一致（params 文件里的 pshold 漂移已被它规避）
  preboot_data_identity.py → GATE_RC=0（kernel/seed/UUID identical: True）
  全盘差异：仅 1 个 512 B 块 @0x4a5bc00（TOM），内核/TRU 字节未动
  sha8=85253d3e，大小 86,077,440 B
  ```
  它发现 `params/picasso.args` 与镜像现值漂移后改用「从镜像 995 B 原样导出」，
  避免一次动两个变量 —— 单变量意识正确。
- 13:28 后**零** fastboot / flash / erase / CI（全部 CALL 逐条核对），仅主机侧
  `apply_params.py` / `patch_args.py`；最后询问「要不要现在就做」属应当请示的设备动作。

### ❌ 最重：判据窗口按 15 s 硬件时钟设计，但启用的是 382 s userspace deadline

`wdt=1` 打开的是 **userspace checkin deadlines**；
`docs/backboard-watchdog-investigation.md:23-27` 明确记录
「382 seconds without a successful backboardd checkin」后 `panic` + `PSHOLD reset`。
硬件 `hardware=15s poll=1s` 由驱动自己在 1 s poll 里喂（源码 :145），只要驱动线程
活着就不咬；GPU 停住时 `hangcheck polls` 持续涨，正是驱动线程仍活着的证据。

而它给的三条判据：
```
卡死后 ≤30 秒出现新 banner      -> WDT_RECOVERED
正常启动后 60 秒内 ≥2 次 banner -> WDT_LOOP
15 秒后既不复位也没新 banner    -> WDT_INERT
```
两个窗口都基于 15 s 硬件时钟 ⇒ **几乎必然把有效方案误判为 `WDT_INERT`**，
也会把 382 s 后的循环重启误判成「正常」。

**修正**：先不设固定阈值，改为记录「最后一次 `DPU_FRAME` → 下一次 banner」的**实际秒数**，
观察窗口 ≥600 s（至少 ≥420 s 覆盖 382 s + 余量），并在日志里区分
「panic 后复位」与「无 panic 直接断」（后者才是 WDT 咬的证据）。拿到分布再定阈值。

### ❌ 回滚路径写错：`fastboot boot` 不需要「刷回」

`fastboot boot` **不写 boot 分区**；冻结令 §十二 明确 boot 分区仍是
`boot-685-pscireset+trustcompat2h-4a6a853d.img`（09:43:47 flash）。
所以 WDT 循环重启时，**长按电源断电 → 从分区启动即回基线**，根本不需要
`fastboot flash boot`。它写「长按电源 → 进 fastboot → 刷回当前镜像」多引入一次
写操作，且每轮自动复位时能否稳定进 fastboot 本身就没保证。

`fastboot boot` 确实会写持久 Data 卷，但门禁 `RC=0`（内核指纹一致）保证身份不变。
所以「需要你点头」的理由应写准为：**会写 Data + 改 boot-args（冻结令范围）+
`wdt=1` 可能让设备长时间不可用**，而不是 Data 身份风险。它把「接管 Data 卷」和
「Data 身份不受影响」放在同一段，容易被读成自相矛盾。

### ⏳ 风险清单漏了一项 / 产物命名

- `backboard-watchdog-investigation.md` 记录 backboardd / audiomxd / CommCenter
  **无成功 checkin**；`watchdogd-trap-investigation.md` 有 watchdogd 与 IOWatchdog
  匹配问题。若 `wdt=1` 启用 deadline 而 checkin 路径未修，**正常桌面也可能在 ~382 s
  后 panic 复位** —— 比它列的「每 15 秒循环重启」更现实，且 60 s 窗口完全看不到。
  未验证，但必须列入风险。
- `boot-685-trustcompat-wdt1-test.img` 文件名**未带 sha8 `85253d3e`**、带固定 `test`，
  违反「按内容命名、不覆盖」。`params/` 下 `picasso-wdt1.args`（2728 B，由旧文件改、
  含 `pshold` 漂移）与 `picasso-wdt1-only.args`（1137 B，真正用于出镜像）并存，
  易误用，应把前者标记作废。
- 冻结令 §十一 原文 4 条**没有**「禁止改 boot-args」这一条（它自称需批准，属更保守，✅）；
  但第 3 条禁止「把『真出问题刷回来』当回滚方案」，其句式应改为「断电重启」。

### 当前状态（13:55 两次取样）—— **已由 14:06 复核部分推翻**

13:55:03 与 13:55:08 两次取样 `frames=226` / 末 `n=4140` 不增长，当时判「出帧后停住」。
**14:06 复核**：同一第 25 会话已到 `frames=507`（末 `n=12570`），14:07 探针副本里又到
`frames=549`（末 `n=13830`）⇒ 它只是**间歇出帧 / 周期性停顿**，不是永久停住。
但 14:06:09 与 14:06:14 两次取样字节数完全相同（14699863 B / 191717 行），
说明 5 秒窗口内也无输出 ⇒ **不能据此断言「稳定」或「停住」**，只能说「间歇出帧」。
`check_boot_sla.py` 全量 25 会话：`LAX 13/25=52%` / `STRICT 3/25=12%` /
`AVAILABLE 3/25=12%`、`RC=2`；`check_boot_session.py` 判 #25 `CLEAN`（末帧后 hang=0）、
全量 `RC=2 DATA_IDENTITY_PANIC`。⚠️ 后者存在盲区：末态判据靠「末帧后 hangcheck 行数」，
而 hangcheck 会限流停打印（12:53 已证），所以 `CLEAN` 不等于画面在推进。冻结令 §十一 生效。

---

## 12. 能否放行 `fastboot boot wdt1`？（14:04 用户提问，14:09 复核）

**结论：⏳ 暂缓放行。镜像与回滚侧全部合格，但观察器判据有决定性缺陷，**
**现在进 fastboot 会白跑 600 s 并得到假成功。补完 4 项纯主机侧修复后即可放行。**

### 12.1 放行前置条件已满足（全部实跑，非转述）

| 项 | 实跑结果 |
|---|---|
| 镜像单变量 | `patch_args.py --show`：两镜像均 `995 B / 52 tokens`，唯一差异 `wdt=0 → wdt=1`；`picasso_usb_reset=psci` 两边一致；`DIFF_RC=0`（差异仅首行文件名） |
| Data 身份门禁 | `preboot_data_identity.py <wdt1> --baseline <trustcompat2h>` → **`GATE_RC=0`**，`kernel/seed/uuid identical = True`；不接管道取真实 `$?` |
| 字节差异 | 仅 1 个 512 B 块（TOM）不同，内核与 TRU 字节未动；`86077440 B` / sha8 `85253d3e` |
| 产物命名 | 已改名 `boot-685-trustcompat-wdt1-85253d3e.img`；漂移参数标 `.VOID-drifted`，正式 `picasso-wdt1-85253d3e.args`（1137 B） |
| 回滚路径 | `fastboot boot` 不写 boot 分区 → 断电即从分区回 trustcompat2h；冻结令 §十一 第 3 条已按场景修正（14:02:24） |
| 零设备动作 | 13:49 后无任何 `fastboot boot/flash/erase/reboot`、无 CI 出包（两条匹配只是 Edit 文本里含 "fastboot boot" 字样） |

### 12.2 判据三个硬缺陷（合成样本实跑，非推测）

样本：`tests/fixtures/wdt-probes/`（**合成，勿计入真实日志统计**），从真实日志复制一份静态基线
（`25` 个 banner），仅追加 banner 行，其余一字未改。

| 对照 | 命令要点 | 实跑结果 |
|---|---|---|
| A | `--current --wdt --budget 8`，不追加 | `NO_RESET RC=6`（WorkBuddy 14:01 空跑的同一分支） |
| **B2** | `--current --wdt --stall-s 2`，2.5 s 后追加**本次 boot 的第 26 个 banner** | **`WDT_RECOVERED RC=0`**，且打印 `userspace-watchdog-panic=False` |
| **C2** | 不带 `--current`（`index=total`，等新会话），1 s 给 #26、4 s 后给 #27 | **`HEALTHY_RESET RC=4`**（启动时就触发） |

**B2 是决定性反例**：`--current` 使 `index = total-1 = 24`，脚本盯的是**旧会话 #25**；
`run_wdt` 里 `n_now > index+1` 即 `26 > 25` 成立，本次 `fastboot boot` 自己的第一条 banner
就被当成「卡死后 watchdog 复位」。而 `last_growth` 在第一次采样被重置为脚本启动时刻
（旧会话已有 549 帧），所以 gap 从脚本启动算、不是从真实末帧算 —— 与 13:28 那轮
「`NO_DRAW` 计时起点错」同类错误在 wdt 模式复现。**实验一开始就会假成功。**

**C2 也不安全**：不带 `--current` 时 `wait_session` 每 5 s poll 一次，若新会话 banner 与
复位 banner 落在同一 poll 窗口，`run_wdt` 第一次采样就直接判 `HEALTHY_RESET`。

另两处：`wdtpanic` 只出现在 `print`（`run_wdt` 第 100、107 行），**完全不参与 verdict**
⇒ 用户手动断电、seed panic 自重启等任何新 banner 都可能被判 `WDT_RECOVERED`；
且字符串 `"userspace watchdog"` 在当前全量日志中 **0 次**，`watchdog timeout` 0 次，
`panic(` 6 次全是 `offline scratch Data mount failed: 2`。

### 12.3 时钟 / 窗口口径冲突（跨构建外推）

- `backboard-watchdog-investigation.md:23-27` 的 **382 s** 来自
  `picasso-windows-20261030-063347-779.log`、UUID `CAE1ABB9-…`，**不是当前 `0EBB496E` 内核**，
  且当前内核 `wdt=0` 下从未出现 userspace watchdog panic ⇒ 属跨镜像外推，**未验证**。
- `watchdogd-trap-investigation.md:317-318` 原文：*"The original first deadline (600 seconds),
  subsequent checkin deadline (90 seconds) … remain distinct."* ⇒ 首轮 deadline 就是 **600 s**，
  与它设的 `--budget 600` **正好撞边**：脚本 `el >= budget` 先退出 `NO_RESET`，看不到复位。
- 结论：窗口应 ≥720 s（覆盖 600 s + 余量），并把 382/600 都写成「历史值、当前内核未实测」。

### 12.4 收益必须澄清（别把它当「修启动成功率」）

`wdt=1` 只把「GPU 卡死后需人工长按断电」变成「自动复位再启动」，**不消除 seed panic、
不消除 0 帧、不提高进桌面概率**；反而若正常桌面也在 382/600 s 被 reset，可用性下降。
用户要的 100% SLA 仍按 §3/§9：固定同内核 + 分区启动 + 连续 ≥10 次纯重启、
`STRICT 10/10`、`OVERALL_RC=0`。本实验属**故障恢复机制**，与主线分开记。

### 12.5 放行前必须完成（全部纯主机侧，不碰设备）

1. 修 `run_wdt` 索引：显式传入 `--start-banner <boot 后的期望编号>`，或让脚本在
   `fastboot boot` **之前**启动并等待 `n_now == total+1` 后才开始计时；禁止用 `--current`。
2. 把 `wdtpanic` 纳入判据：`WDT_RECOVERED` 必须同时满足「前一会话含 userspace-watchdog
   panic 或明确的 PSHOLD 复位标记」，否则只报 `RESET_UNKNOWN_SOURCE`；panik 文案要按
   实际日志核，不能只靠一个字符串常量。
3. `last_growth` 改用**日志时间**（给串口行加时间戳，或记录「末帧行号 + 首次观察到它的墙钟」），
   不得从脚本启动时刻重置。
4. `--budget` 默认 ≥720 s；把 A/B2/C2 三组对照写进回归集，另补一组「真复位」正样本
   （含 userspace-watchdog panic 文案）应判 `WDT_RECOVERED RC=0`，并给 `py_compile RC=0`。

### 12.6 上机执行与停止条件（补齐后再用）

- 进 fastboot 前记录基线：boot 分区 = trustcompat2h、Data UUID `0EBB496E` / `stage=4`、
  当前帧数、`device-info.txt`；`fastboot boot boot-685-trustcompat-wdt1-85253d3e.img` 一次，
  不 flash、不 erase、不改其它 boot-args。
- 判据命令不接管道取真实 `$?`；每次启动落独立日志并按内容命名。
- 停止条件：出现 Data panic、设备进入无法进 fastboot 的循环、或 720 s 内无任何复位 →
  立即停、断电从分区启动回基线、**如实归档，不自行改参数再试**。
- 回退就绪度 `check_android_rollback.py RC=3 DEGRADED`：14 项 HARD 全 PASS，
  `getvar all` 快照已补，**仅 EDL 9008 firehose 缺失**（L2 可用、L3 不可用）。
  本次是 `fastboot boot`、断电即回 L1，不依赖 L3；但若设备因循环复位进不了 fastboot，
  L3 缺失就是风险，须向用户说明。
- 冻结令 §十一 其余条款不变：禁止重编内核 / CI 出包 / `fastboot erase userdata` / 未评审 PnP。

---

## 13. 放行判定（14:30 复核）：**✅ 可以开始跑**

§12 的 4 项修复 WorkBuddy 14:14–14:23 全部落地，且我独立复跑通过。
**但「5/5 全过」是拼接口径**：最终 `reg_wdt.py` 只含 4 个用例，最后实跑
`REGRESSION 4/4 passed / REG_RC=0`；A 分支（不 boot → `NO_SESSION`）已从脚本移除，
是 14:15 早期版本（当时 2/5）的结果。我 14:30 用**新版 PENDING 状态机**补跑 A：
`--wdt --baseline 25 --budget 5 --wait-session 3` → `NO_SESSION RC=6`、且不再误打
「会话 #100 已出现」，✅ 通过。所以判据实际覆盖 5 个分支，但报表须写「4/4 脚本内 + A 单独复跑」。

### 13.1 独立复核证据（全部实跑，不采信转述）

| 项 | 实跑结果 |
|---|---|
| 回归 | `cd /d/k30i-gpu && python reg_wdt.py; echo $?` → **`REG_RC=0` / 4/4**：B `NO_RESET 6`、C `HEALTHY_RESET 4`、D `WDT_RECOVERED 0`、E `SILENT_RESET 2` |
| A 分支补跑 | `NO_SESSION RC=6` |
| 语法 | `py_compile RC=0` |
| 门禁 | `preboot_data_identity.py <wdt1> --baseline <trustcompat2h>` → **`GATE_RC=0`**（kernel/seed/uuid identical） |
| 镜像 | 14:09 后无新增/改动；`boot-685-trustcompat-wdt1-85253d3e.img` 仍是单变量 `wdt=0→wdt=1` |
| 设备动作 | 14:09 后**零**真实 fastboot/flash/erase/CI（唯一匹配是脚本注释里含 "fastboot boot" 字样） |

三条 bug 修法核对（源码 59–180 行）：废弃 `--current`、新增 `--baseline N`，阶段 1 等
`banners > N`（本次 boot）、阶段 2 等 `banners > N+1`（复位）；`wdt_panic` 参与判定
（含 `watchdog`/`checkin` → `WDT_RECOVERED`；其它 panic → `PANIC_RESET_OTHER rc=3`；
无 panic → `SILENT_RESET rc=2`）；`--budget` 默认 720 s 且 ≤600 打 WARN。
`last_growth` 仍用脚本墙钟但只在**被测会话内帧增长**时更新，不再从启动时刻重置 ✅。

### 13.2 上机前 1 分钟必做（按顺序，纯主机侧）

1. **实测 baseline，不要写死 25**：`grep -ac '^Darwin Kernel Version' D:/k30i-gpu/console/console-20261113-070546.log`
   → 14:30 实测 **25**，所以本次用 `--baseline 25`（被测会话 = 第 26 个）。若数字变了，跟着改。
2. **确认记录器仍连通**：全量日志有 `PORT OPEN 30 / PORT CLOSED 29 / PORT FAILED 32 / REPROBE 32`。
   断电进 fastboot 必然触发 CLOSED→433→REPROBE；**若 boot 的 banner 在重连窗口内打印，
   串口会整行丢失，脚本会给 `NO_SESSION rc=6` 而不是实验结论**。boot 前确认记录器进程活着且
   最后一条标记是 `PORT OPEN`；出现 `NO_SESSION` 先查串口重连，不许判「wdt 无效」。
3. **确认 `--log` 指向记录器正在写的那一份**；若记录器改成「每次启动独立日志」，两边路径必须同步。
4. **清理/标注调试残留**：`console/wdt-observe-20261113-1421*/1422*/1423*-4a6a853d.log`
   三份只有 `NO_SESSION RC=6`（两份还重复两行），且 imgsha 是默认基线 `4a6a853d`，
   不是 wdt1 的 `85253d3e`；按「产物按内容命名」移走或标注作废，避免被当正式结果。

### 13.3 执行顺序（关键：观察器必须先于 boot 启动）

```
# 1) 用户手动进 fastboot（当前桌面已 GPU hang，见 13.5，进 fastboot 无额外损失）
# 2) 实测 baseline N（=25）
# 3) 后台启动观察器，务必在 fastboot boot 之前
python -u observe_boot.py --log D:/k30i-gpu/console/console-20261113-070546.log \
  --wdt --baseline 25 --budget 720 --poll 5 --stall-s 60 --wait-session 180 \
  --imgsha 85253d3e > wdt-run-<时间>.out 2>&1 ; echo "OBS_RC=$?"    # 不接管道取真实 $?
# 4) 再发一次，只发一次
fastboot boot D:/k30i-gpu/next/boot-685-trustcompat-wdt1-85253d3e.img
```
不 flash、不 erase、不改其它 boot-args、不动 PnP、不停记录器。

### 13.4 结果解读（WorkBuddy 未写，必须补）

| 判据 | 含义 | 是不是「测成了」 |
|---|---|---|
| `WDT_RECOVERED rc=0` | 停住后由 watchdog panic 触发复位 | ✅ 唯一算「机制生效」 |
| `HEALTHY_RESET rc=4` | 还在出帧就被复位 = 误伤正常桌面 | ❌ 方案有害 |
| `PANIC_RESET_OTHER rc=3` | Data panic 等非 watchdog 复位 | ⚠️ **没测到 wdt** |
| `SILENT_RESET rc=2` | 停住后无 panic 直接复位（用户断电/硬件） | ⚠️ **没测到 wdt** |
| `NO_RESET rc=6` / `NO_SESSION rc=6` / `RESET_BEFORE_FRAME rc=6` | 720 s 无复位 / 串口丢 banner / 没出帧就复位 | ⚠️ **没测成，不是 wdt 无效** |

历史全量 25 会话里 seed panic 6、GPU 0 帧 6；若本次落在这些形态，
实验大概率给「没测成」，要按形态归档而不是下「wdt 无效」结论。
另 ⏳：`wdt_panic` 文案 `userspace watchdog timeout ...` 来自旧构建
（UUID `CAE1ABB9`），当前内核若真触发是否同格式**未验证**；`gap` 用墙钟，
串口写入延迟会让它偏小，可能把久停误判 `HEALTHY_RESET`。

### 13.5 当前状态与回滚

14:30:14 / 14:30:19 两次取样：`14970199 B / 194691 行 / 25 banner`，
字节与帧数完全相同；第 25 会话 `frames=549 / n=13830 / panic=0 / stage=1`，
末尾 `hangcheck polls=6323 fence=28202 hang_fence=28202` ⇒ **fence 冻结，当前是 GPU hang 停住**，
不是可用桌面（14:06 的「间歇出帧」已演进到停住）。全量 `check_boot_sla RC=2`：
`STRICT 2/25 = 8%`（较 14:09 的 3/25 下降，因 #25 转为 `STALLED_END`）。

- 回滚：`fastboot boot` 不写 boot 分区 → **长按电源断电、从分区启动**即回
  `boot-685-pscireset+trustcompat2h-4a6a853d.img`；门禁 `RC=0` 保证 Data 身份不变
  （仍会写持久 Data 卷）。Data UUID `0EBB496E…` / `stage=4`。
- 回退就绪度 `check_android_rollback.py RC=3 DEGRADED`：L2（回原厂安卓）可用、
  **L3（EDL 9008 firehose）缺失**；本次不依赖 L3，但循环复位进不了 fastboot 时是风险。
- 停止条件：Data panic、720 s 无复位、进不了 fastboot、串口持续重连 → 立即停、
  断电回基线、如实归档，**不改参数再试**。
- `wdt=1` 只把「人工断电」变「自动复位」，**不提高进桌面概率**；100% SLA 仍按
  固定同内核 + 分区启动 + 连续 ≥10 次纯重启 + `STRICT 10/10` 单独推进。

  **§13.5 状态更正（14:57）**：14:30 写的「GPU hang 停住」只是短窗口快照。
  14:56 静态样本帧数已从 288 涨到 322，14:57:30 实测 `frames=322 / n=9660`、
  `PICASSO_WAIT snapshot 9 of 15 uptime_ms=1315288`（会话已跑 ~22 分钟）⇒
  当前是**间歇出帧 + 长停顿**，不是永久停住。两次取样间隔 5–6 秒不足以判停住。

---

## 14. 「要不要再跑一次」（14:52 用户提问，14:57 复核）

**结论：❌ 不建议重新 `fastboot boot` 再跑一次同样的实验。**
**✅ 建议零成本「续观察」当前仍在运行的 wdt=1 会话，并先做纯主机侧归因。**

### 14.1 首轮实验复核：执行合规、结果 `NO_RESET`

| 项 | 证据 |
|---|---|
| 顺序 | 14:34:33 观察器先起（`--wdt --baseline 25 --budget 720 --poll 5 --stall-s 60 --imgsha 85253d3e`）→ 14:35:06 才发 `fastboot -S 4M boot <wdt1>`，`BOOT_RC=0`、`Sending 84060 KB OKAY / Booting OKAY / 28.281s`，**未 flash** |
| baseline | 14:34:25 实测 `N=25`（未写死）；记录器最后标记 `PORT CLOSED 14:33:52` |
| 串口 | `CLOSED 14:33:52` → `FAILED 433 14:36:30` → `REPROBE 14:36:45` → `PORT OPEN 14:36:45.576` → `KERNEL BANNER 14:36:45.611`；banner 在重连后 35 ms 抓到，**这次没丢**（但不代表下次不会） |
| 结果 | `wdt-observe-20261113-143434-85253d3e.log`：`135` 条 `banners=26`、`banners=27` **0**、`panic` 0、`VERDICT=NO_RESET RC=6`，`t=721s` |
| 合规 | 14:30 后只有 **1 次**真实 `fastboot boot`（另一条匹配是写 memory 文档的文本）；六档解读已落地；5/5 改为 4/4 + A 单独复跑；三份残留已 `.VOID` |

⚠️ 时长口径：`Duration 14m17s`（14:34:33→14:48:51 ≈ 858 s）与观察日志 `t=721s`
不矛盾——**137 s 是阶段 1 等 boot banner**（脚本启动 33 s 后才发 boot、传输 28 s、
到 banner 共 136 s），观察窗口从 14:36:50 起算。WorkBuddy 把多出的 136 s 归因为
「每次 poll 整文件读 15 MB」是**未验证归因**：整文件读影响的是 poll 精度（约 1.3 s/次），
不是 136 s 的等待期。

### 14.2 结论 ② 是循环论证（本轮最重错误）

WorkBuddy 14:49 写「102 秒画面停住却毫无 panic ⇒ backboardd checkin 仍正常 ⇒
**wdt=1 救不了 GPU 停帧，直接否掉路线**」。逐项核日志：

- `wdt=1` **确实被内核解析** ✅：196363 行 `com.apple.xpc.launchd ... boot-args = -v debug=0x14e wdt=1 cpus=8 ...`
  （其余 19 次是 `wdt=0` 的旧会话）。所以「boot-arg 没生效」这个替代解释可排除其一。
- 但 **`checkin` 78 次全是误匹配**：内容是 `isCdhashInTrustCache` / `Attached signature to file, checking ...`
  —— AMFI 签名检查里的 `checking` 含 "checkin" 字串，**没有一条 backboardd checkin 记录**。
  第 26 会话里 4 次同样是 AMFI 文本。
- **`deadline` 112 次全是 `NATIVE_UFS_SUBMIT deadline=begin/end`**（UFS 超时），与 watchdog deadline 无关。
- 日志里 `userspace watchdog timeout` / `no successful checkins` **均 0 次**；
  `PICASSO_WAIT snapshot` 是每 120 s 一次的线程栈 dump（`PICASSO_STK` / `PICASSO_THR`），
  **不是 checkin 成功日志**。
- ⇒ 「checkin 正常」是从「没 panic」反推的；而「没 panic」同样可能是 deadline 未启用
  （当前内核是否把 backboardd 注册为 watchdog client，未验证）。**从一次未触发的
  `NO_RESET` 推出机制性结论，属于未验证归因**；且与它自己在第 3 节写的
  「`NO_RESET` 不能写成 wdt 无效」自相矛盾。按 §13.4，`NO_RESET` 只等于「没测成」。

### 14.3 为什么不该重跑、该做什么

1. **重复同样的 boot 不会新增信息**：`wdt=1` 已确认传入内核；这次 721 s 无复位、
   无 panic、无误伤，再 boot 一次在同样条件下大概率同样 `NO_RESET`。
   要赌「这次卡死」是随机事件，成本 = 12–20 分钟 + 一次断电进 fastboot + 串口重连 +
   一次写持久 Data，且卡死不可控。
2. **设备此刻仍在跑 wdt=1，窗口还开着**：14:57:30 实测 `26 banner`、
   `frames=322 / n=9660`、`panic=0`、`PICASSO_WAIT snapshot 9 of 15 uptime_ms=1315288`
   （已运行 ~22 分钟，远超 382/600 s 历史 deadline），有 1 次 **127.2 s** 大停且仍在
   间歇出帧。观察器 14:48:50 已退出，**之后发生什么没人记录**。续观察零变量、零设备动作。
3. **一个更值钱的问题**：会话已过 600 s 两倍却无 panic。要么 userspace checkin 一直正常，
   要么 deadline 根本没启用——**两者用纯主机侧源码/日志即可区分，不需要上机**。
   先查 `picasso_original_iowatchdog_policy_check` 在 `wdt=1` 下是否真的注册 client、
   以及内核有没有打印 deadline 启用行。
4. **A/B 两个选项都绕开了最显然的动作**：A「接受现状转 WiFi/音频」与用户
   「启动成功率 100%」主线冲突（WiFi/音频此前明确排后）；B「出包查 GPU 根因」
   需解冻，而 EDL 9008 firehose 仍缺、恢复 SOP §4.1 偏移超范围。

### 14.4 续观察判据（已写脚本并实跑）

`scripts/watch_wdt_reset.py`：纯读日志、不发串口指令、不动设备；起点就是当前会话
（没有「等本次 boot banner」阶段），只盯第 `baseline+1` 个 banner，并按 panic 文案分类。

```
python -m py_compile scripts/watch_wdt_reset.py                       # RC=0
python -u scripts/watch_wdt_reset.py \
  --log D:/k30i-gpu/console/console-20261113-070546.log \
  --baseline 26 --budget 600 --poll 5
```
三组对照（合成样本在 `tests/fixtures/wdt-probes/`，**勿计入真实日志统计**）：
不复位 → `NO_RESET RC=6`；运行中追加带 watchdog panic 的第 27 banner →
`WDT_RECOVERED RC=0`；追加无 panic 的第 27 banner → `SILENT_RESET RC=2`。
`py_compile RC=0`，全部真实退出码、不接管道。

⚠️ **我方判据盲区（按纪律认）**：`check_boot_sla.py` 判 #26 `STALL`（127.2 s 大停），
`check_boot_session.py` 却判 `CLEAN`（末帧后 `hangcheck` 行数 0）——因为 hangcheck 会
限流停打印，末帧之后只有 1 行日志。**日志静止时 `check_boot_session.py` 的 CLEAN 不可信，
本轮统一以 `check_boot_sla.py` 为准**；修末态判据需要构造「自恢复 vs 真停住」两组样本，
列为待办，不擅自改逻辑。全量 26 会话：`LAX 14/26=53.8%` / `STRICT 2/26=7.7%` /
`AVAILABLE 2/26=7.7%`，两脚本 `RC=2`（取最严重会话，非当前）。

### 14.5 若用户仍决定再跑一次，先写死条件

- **样本预算**：最多 2 次，每次必报全量（含 `NO_RESET`），连续 2 次健康即停止；
- **零变量**：同一镜像 `boot-685-trustcompat-wdt1-85253d3e.img`、同一 `--baseline` 流程、
  同一 `--budget 720`、不 flash、不 erase、不改 boot-args、不停记录器；
- **先定义「真卡死」**：`fence == hang_fence` 且 ≥120 s 无新帧（`period_us` 证据），
  不能把 30–127 s 的间歇停顿算卡死；
- **每次上机前**：实测 banner 数、确认记录器 `PORT OPEN`、门禁 `RC=0`（不接管道）、
  记录 Data UUID `0EBB496E` / `stage=4` 基线；
- **停止条件**：Data panic、串口持续重连、进不了 fastboot → 立即停、断电从分区回
  trustcompat2h、如实归档，不改参数再试。

**UUID / panic 专项**：当前 `0EBB496E-FB09-4223-991E-22007B6FACD1` / `stage=4` /
第 26 会话 `panic 0`；全量 6 次历史 panic 均 `offline scratch Data mount failed: 2`。
`fastboot boot` 未写 boot 分区，断电即从分区回基线；门禁 `RC=0` 保证 Data 身份不变
（仍会写持久 Data 卷）。回退就绪度 `RC=3 DEGRADED`：L2 可用、**L3 EDL 9008 firehose 仍缺**。
