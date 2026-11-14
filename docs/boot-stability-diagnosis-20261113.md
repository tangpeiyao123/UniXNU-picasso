# 进桌面稳定性：归因与断点定位（2026-11-13 23:55 起，11-14 00:45 校正 v3）

> 数据：`D:/k30i-gpu/console/console-20261113-220343.log`（11 段 banner 会话），
> `reproduce_desktop.py --cycles 10 --window 300 --cmd-file …`，22:08:15 起、22:51 收尾。
> 判据与分型全部写进 `scripts/measure_desktop_reach.py`，本文件的表就是它的实跑输出（`MEASURE_RC=0`）。
> 只写实测；推测单列并标注「未验证」。

---

## 0. 相对上一版（dee67ea）的三处修正

| 项 | 上一版 | 本版（有证据） |
|---|---|---|
| S01 为什么没有 panic | 「22:07:38 端口掉线，panic 文本丢了，可能是第 6 次 SEED」 | **没有 panic 可丢**：22:07:14.052 主机发了 `reboot-fastboot`，65 ms 后端口关。banner 22:05:31 → 重启 22:07:14，**102.1 s**，早于 300 s 判定窗口 ⇒ 这次启动是被我们自己重启掉的，不是故障。上一版的猜测作废。 |
| NO_DRAWABLE 的主因表述 | 「GPU hangcheck polls 持续上涨、fence=19 恒定 → 管线卡死」 | **主因是首个 IOSurface 从未 attach**；fence 停在初值只是下游症状。PASS 会话的 fence 会一路爬到 1188/1206（S11 末值 1206），19 不是特殊值。 |
| `selectVolumeSeed` 计数 | 2 | **按符号去重 = 1**。镜像里 2 次命中都是同一个 mangle 名 `__ZL16selectVolumeSeedPKcPiPcmPyS3_Ph`，分别落在两张符号表；评审 grep=1 正确。 |

---

## 1. 两个分母（都报，不挑）

```
TOTAL 11 sessions  PASS 3  STALL 7  TRUNC 1  pass-rate 27.3%
VALID 10 sessions (TRUNC excluded)  PASS 3  STALL 7  pass-rate 30.0%
```

- **27.3% (3/11)**：全量，含被主机中途重启的那一段；
- **30.0% (3/10)**：剔除 S01（`TRUNCATED_BY_HOST`，102.1 s 就被重启，没跑满判定窗口）。

| 会话 | vsync | desktop | panic | frames | eagl | texk | 判定 | 分型 / 备注 |
|---|---|---|---|---|---|---|---|---|
| S01 | 1 | 0 | 0 | 0 | 0 | 0 | **TRUNC** | 主机 102.1 s 重启，不计入 |
| S02 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | SEED |
| S03 | 2 | 313 | 0 | 0 | 1 | 0 | STALL | NO_DRAWABLE（319 s 才重启） |
| S04 | 17 | 246 | 0 | 0 | 1 | 1 | PASS | 61.9 s 早停，0 帧 |
| S05 | 2 | 313 | 0 | 0 | 1 | 0 | STALL | NO_DRAWABLE（347.8 s 才重启） |
| S06 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | SEED |
| S07 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | SEED |
| S08 | 17 | 247 | 0 | 0 | 1 | 1 | PASS | 61.1 s 早停，0 帧 |
| S09 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | SEED |
| S10 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | SEED |
| S11 | 17 | 5572 | 0 | **331** | 1 | 1 | PASS | 唯一真出帧（331 行 × 30 flip） |

分型直方图：**STALL_SEED 5 / STALL_NO_DRAWABLE 2**（TRUNC 不计）。

`texk IOSurface attached` 仍是零例外分水岭：打过它的 3 段全 PASS，没打的全部 STALL。
但 PASS 的含义要写死：`vsync>=17 + UI 起` = **显示管线 armed**，不是「屏幕可见桌面」——
S04/S08 在 61 s 就被判 PASS 并重启，首帧（历史中位 ~104 s）还没到，所以 frames=0。
要判桌面，`reproduce_desktop.py --min-frames 2` 才作数。

---

## 2. 分型一：SEED panic（5 次，单一根因）

签名完全一致，无第二类 panic：

```
panic(cpu N caller 0xfffffff007946a38): offline scratch Data mount failed: 2 @kern_exec.c:9908
```

uptime 来源行（panic 块内 `mach_absolute_time`）：日志 2679 / 11980 / 13517 / 17416 / 18968 行。

| 会话 | mach_absolute_time | @24 MHz（假设） | @19.2 MHz |
|---|---|---|---|
| S02 | `0x16a637897` | 253.3 s | 316.7 s |
| S06 | `0x16b1db064` | 253.8 s | 317.2 s |
| S07 | `0x16a6c51f4` | 253.4 s | 316.8 s |
| S09 | `0x16b1d6ded` | 253.8 s | 317.2 s |
| S10 | `0x16b23c1bb` | 253.9 s | 317.4 s |

日志无 CNTFRQ 行，24 MHz 是假设；**跨度 0.6 s 与频率无关**，固定定时器特征成立。
旁证：S02 从 `provider=ram`(2266) 到 panic(2664) 共 395 行，240 s 窗口 ≈1.6 行/s，
与 hangcheck 实测 1–1.7 行/s 同量级；若按 19.2 MHz 只剩 77 s，需 5 行/s，明显偏高。

路径（`NativeRamBlock.cpp` / `selectVolumeSeed`）：

```c
while(!native_ram_block_ready && waited++<2400) IOSleep(100);   // 240 s
if(native_ram_block_ready!=1) return 0;                          // -> ENOENT -> panic
```

判别证据：`PICASSO_DATASTORE geometry` 由 worker 在 `attachStore()` 发布前打印。
有它的 5 段（S03/04/05/08/11）全部走到 UI；没有它的 5 段（S02/06/07/09/10）全部 panic。
成功段 geometry 紧贴 `provider=ram` 下一行（S03: 3782→3783），即**秒级发布**；
失败段 240 s 内一次都没有 ⇒ 双峰：要么秒级，要么根本不发布。

---

## 3. 分型二：STALL_NO_DRAWABLE（S03/S05）——断点定位

### 3.1 分叉在哪一行

把两段的 EAGL 检查点按顺序列出（`PICASSO_GPU text:` 行，地址归一化后比较）：

| # | S11（PASS） | S03（STALL） |
|---|---|---|
| 9 | `eagl init gl=0xX` | `eagl init gl=0xX` |
| 10 | `eagl init ok api=3` | `eagl init ok api=3` |
| 11 | `eagl macroctx 0xX` | `eagl macroctx 0xX` |
| **12** | **`texk IOSurface attached i80e1 f80e1 t1401 1080x2400`** | **（无）** |
| 13 | `eagl at s1 1080x2400 px42475241 inv0 sh0 mr1` | （无） |

**分叉点 = `eagl.c` 的 `-texImageIOSurface:` 包装**（`note_format("IOSurface attached", …)`，eagl.c:4514）。
S03 的第 11 个检查点之后就再没有 EAGL 调用。

### 3.2 卡在哪个调用之前（调用级，行号取自 logs2/stall-03.txt）

| 行 | S03（STALL） | S11（PASS） |
|---|---|---|
| 1854–1857 | `IOCoreSurfaceRoot` 查找，`rpc=0 result=0 object=1`（**成功**） | 同（成功） |
| 1912–1913 | `PICASSO_FBUC call=15 selector=50` → `done=15 ret=e00002c7` | 同 |
| 1915–1916 | `AGXAccelerator` **第 1 次**用户客户端打开（`sample=10`，thread 709，`result=0 object=1`） | 第 1 次在 1970（thread 754），`result=0 object=1` |
| — | **没有第 2 次** | 1978 `AGXAccelerator` **第 2 次**打开（`sample=11`，thread 767） |
| — | **没有 `FBUC call=16`**（backboardd 序列停在 15；16/17 属于另一个进程 `CollectionsPoste`） | 1981 `FBUC call=16 selector=68` |
| — | **没有 `texk IOSurface attached`** | 1983 `texk IOSurface attached … 1080x2400` |

第 1916 行之后，S03 剩余 **1748 行里没有任何一行**含 `gl` / `texture` / `surface` / `eagl` / `drawable`
（`awk 'NR>1915' logs2/stall-03.txt | grep -i …` 为空）。
期间 backboardd 没有死：继续做 IOKit、brightness、WLED，SpringBoard 也起来了。

**分叉点的精确表述**：backboardd 停在第 1 个 AGX 客户端之后，
**第 2 个 `IOServiceOpen(AGXAccelerator)`（PASS 里在 texk 前 5 行）从未发生**。

#### 3.2.0 但要收窄：这不是 NO_DRAWABLE 独有的因果

把 `070546` 里所有 PASS 与 `AFTER_DRAWABLE` 会话按同样三项计数：

| 类别 | 会话 | AGXAccelerator 匹配 | FBUC call | texk | 帧 |
|---|---|---|---|---|---|
| PASS | S01/02/03/09/12/14/17/18/20/21/22/23/25/26/27/30/34/43 | **3** | 160–284 | 1 | >0 |
| AFTER_DRAWABLE | S05/07/11/24/31 | **2** | 19–21 | 1 | 0 |
| NO_DRAWABLE（日志 B） | S46/48 | **2** | ~19 | 0 | 0 |

⇒ 「少一次 AGX 打开、FBUC 只到 ~19」是**所有提前停住的会话共有的现象**，
不是 NO_DRAWABLE 特有的原因。它把「停住」定位到「backboardd 建立第一套渲染客户端之后没多久」，
但**不能**用来区分 NO_DRAWABLE 与 AFTER_DRAWABLE——两者的唯一差别是首块 IOSurface 有没有 attach。
这也是为什么下一步必须是探针，而不是继续在日志里找差异。

### 3.2.1 为什么不能再说「更具体」

`-texImageIOSurface:` 的包装在早退分支上会 `say("hakios-gl: … not done")`，但
**`say()` 不走串口**（全段 `hakios` 命中 = 0，S03 和 S11 都是 0），所以看不到返回值。
因此「调用发生了但 `texture==0`/`image_new==NULL` 早退」与「根本没调用」这两种情况，
现有日志**分不开**。要分只能加探针（见 `patches/held/NO-DRAWABLE-probe.md`）。

另外更正一条评审意见：`eagl.c` **在仓库里**（`git ls-files` 有 `drivers/NativeGL/eagl/eagl.c`），
4514 行就是 `note_format("IOSurface attached", …)`，是源码行号、不是解码符号位置，
所以将来要改的确实是这个文件（冻结令解除后）。

### 3.3 fence 是下游

S03/S05：`polls 2→461 / 2→509`，`fence==hang_fence==19` 恒定。
S11：fence 一路 1→…→1188→1206，同时 `DPU_FRAME` 331 行。
S04/S08（PASS 早停）：fence 停在 21。所以 **19 只是「初始化后没有新提交」的初值**，
不是特殊值；真正的分水岭是「首个 drawable 有没有 attach」。

---

## 4. 分型三：S01 = TRUNCATED_BY_HOST（不是 SILENT，也不是丢 panic）

主机侧时间线（全部来自日志里的 `===== … host=… =====`）：

```
22:03:43.500  RECORDER START
22:04:38.487  CMD SENT reboot-fastboot -> COM3      ← 前一个实例留下的命令
22:05:16.883  PORT FAILED
22:05:31.946  KERNEL BANNER                          ← S01 起点
22:07:14.052  CMD SENT reboot-fastboot -> COM3      ← 又一次
22:07:14.117  PORT CLOSED (ReadFile 995)             ← 65 ms 后
22:08:08.589  KERNEL BANNER                          ← S02
```

- 22:07:14 那条命令发出后 65 ms 端口就关了 → S01 是被**主动重启**的，不是自然卡死；
- 它只活了 102.1 s，远早于 300 s 判定窗口，因此对「启动成功率」没有证据价值；
- 两条早于本脚本 22:08:15 启动的 `reboot-fastboot`，说明**当时有第二个复现实例在共用同一个 cmd 文件**。

处置：① `measure_desktop_reach.py` 增加 `TRUNCATED_BY_HOST` 判定（banner→reboot < `--window` 即判 TRUNC 并剔出分母）；
② `reproduce_desktop.py` 增加独占锁（`<cmd-file>.lock`，含 PID，异常退出 6 h 后视为陈旧），
第二个实例直接 `RC=2` 拒绝启动。已用两个进程实测：第二个 `RC=2`，第一个正常退出后锁被清掉。

---

## 5. 「retry 在不在镜像」——精确核

镜像 `boot-685-pscireset+trustcompat2h-4a6a853d.img`（86,077,440 B）：

| 字符串 | 次数 | 性质 |
|---|---|---|
| `selectVolumeSeed` | 2 | 同一个 mangle 名 `__ZL16selectVolumeSeedPKcPiPcmPyS3_Ph` 出现在两张符号表 → **按符号去重 = 1** |
| `native_ram_block_volume_seed` | 2 | 同上 |
| `offline scratch Data mount failed` | 1 | 基字符串 |
| `PICASSO_DATASTORE geometry` | 1 | **阳性对照：另一补丁新增的串确实在镜像里** |
| `PICASSO_SEED enter` / `wait ready` | **0 / 0** | `4c19974` 新增 |
| `NATIVE_OWNER_DATA seed retry` | **0** | `ca349c0` 新增（`printf("NATIVE_OWNER_DATA seed retry attempt=%u")`） |

结论只取：**基函数在镜像，retry 补丁新增的代码不在**。不写「镜像不含 retry」。

---

## 6. 下一版诊断构建建议（只写建议，未构建、未 flash）

1. 只带 `picasso-seed-diag-ramblock.patch`（log-only）；`data-seed-retry` 继续 held
   （40×240 s = 9600 s 风险，见 `patches/held/README.md`）。
2. 目标读数：
   - `PICASSO_SEED wait ready=0 waited=N` 涨到 2400 → worker 卡住没发布；
   - `PICASSO_SEED ready timeout=2` → worker 明确拒绝，配合同段 `persistent store refused:` 看条件；
   - `waitMatching begin/end` 的间隔 → UUID 匹配是否又是一段等待。
3. 若要继续查 NO_DRAWABLE，需要一个新的探针：在 IOSurface 外部方法（create/lookup）入口打点，
   才能区分「surface 没创建」与「创建了没送到 GL」。这是下一版可选项，不与 SEED 混在一版。
4. 判据同步：`reproduce_desktop.py --min-frames 2` 才算「桌面」，否则一律读作「显示管线 armed」。

---

## 7. 本轮主机侧改动（未 dispatch、未 flash、未写 COM3）

| 文件 | 改动 |
|---|---|
| `scripts/measure_desktop_reach.py` | `frames/eagl/texk` 三列；按阶段分型（SEED / NO_DRAWABLE / NO_GL / AFTER_DRAWABLE / SILENT）；`TRUNCATED_BY_HOST` 判定与双分母；`--window` |
| `scripts/boot_state_transitions.py` | 新增：状态序列 + 上一段→下一段矩阵 + Fisher 检验（复用 measure 的切分与判定） |
| `scripts/reproduce_desktop.py` | `--min-frames`（默认 0，不改旧行为）；`<cmd-file>.lock` 单实例锁 |
| `k30i-ios/console_recorder.py` | 端口掉线写 `CAPTURE GAP BEGIN`，重连写 `CAPTURE GAP END gap_s=`；停止时若仍在 gap 内写 `CAPTURE GAP OPEN AT STOP`。日志句柄本就是 `buffering=0`，主机侧字节不丢；**设备在断线期间打印的内容无法回填**（没有重放通道），所以只标记窗口，供下游判「未观测」而不是「静默」 |

验证：`py_compile` 三个文件 `RC=0`；`measure` 实跑 `RC=0`；gap 标记用临时日志跑过（begin 幂等、end 带时长、停止时告警）；
锁用两个进程实测（第二个 `RC=2`）；`boot_state_transitions.py` 实跑 `RC=0`。

---

## 8. 全量历史（三份带 banner 的日志，59 段）与状态转移

```
python scripts/boot_state_transitions.py \
  --log D:/k30i-gpu/console/console-20261113-061914.log \
  --log D:/k30i-gpu/console/console-20261113-070546.log \
  --log D:/k30i-gpu/console/console-20261113-220343.log     # RC=0
```

覆盖：`061914` 5 段、`070546` 43 段、`220343` 11 段 = **59 段**。
其余 `session-*.log` 只有 1 个 banner（单段碎片），未计入。

| 状态 | 次数 | 占比 |
|---|---|---|
| PASS（显示管线 armed + UI 起） | 22 | 37.3% |
| STALL_SEED | 17 | 28.8% |
| STALL_SILENT | 7 | 11.9% |
| STALL_AFTER_DRAWABLE（texk 有、0 帧） | 6 | 10.2% |
| STALL_NO_DRAWABLE | 4 | 6.8% |
| STALL_NO_GL | 2 | 3.4% |
| TRUNCATED_BY_HOST | 1 | — |

**NO_DRAWABLE 不是固定比率**：`070546` 2/43 = 4.7%，`220343` 2/11 = 18.2%，`061914` 0/5。
样本小，只能说「随日志/阶段漂移」，不能给稳定比例。
`AFTER_DRAWABLE`（画到了但没出帧）6 次里 5 次在 `070546`、1 次在 `061914`，
`220343` 一次都没有——它说明「attach 成功」也不是充分条件，比 NO_DRAWABLE 更值得留意。

**状态转移（56 次转移，合并三份）**

| 上一段 \ 下一段 | AFTER | NO_DRAW | NO_GL | PASS | SEED | SILENT | TRUNC |
|---|---|---|---|---|---|---|---|
| AFTER_DRAWABLE | 0 | 0 | 0 | 2 | 4 | 0 | 0 |
| NO_DRAWABLE | 0 | **0** | 0 | 2 | 1 | 1 | 0 |
| NO_GL | 0 | 0 | 0 | 1 | 0 | 1 | 0 |
| PASS | 2 | 3 | 1 | 8 | 5 | 0 | 0 |
| SEED | 4 | **1** | 0 | 6 | 4 | 2 | 0 |
| SILENT | 0 | 0 | 1 | 1 | 2 | 3 | 0 |
| TRUNC | 0 | 0 | 0 | 0 | 1 | 0 | 0 |

- `P(NO_DRAWABLE | 上一段 SEED) = 1/17 = 5.9%`
- `P(NO_DRAWABLE | 上一段 PASS) = 3/19 = 15.8%`
- **Fisher 精确检验双侧 p = 0.605** ⇒ **没有状态依赖证据**；方向还与「SEED 残留态更容易 NO_DRAWABLE」相反。
- `NO_DRAWABLE → NO_DRAWABLE = 0/4`：这一类不自我延续。

结论：**NO_DRAWABLE 不是「上次 SEED panic 留下的 Data 残留」造成的**（至少这批数据不支持），
按独立故障处理，不要和 SEED 混进同一版改动。

## 9. 下一版（NO_DRAWABLE）最小补丁思路 —— 只写建议，未改代码

已落到 `patches/held/NO-DRAWABLE-probe.md`。要点：

1. **先探针，不改行为**：在 `my_texImageIOSurface` 入口加一条 `ktrace`，打出
   `surface` 是否 NULL、`target`、绑定到的 `texture`、`image_new` 是否成功、返回 YES/NO。
   现在 `say()` 不走串口，等于这个函数是黑盒；加一条 ktrace 就能把 §3.2.1 的两种可能分开。
2. **不要**先做「跳过已成功的 surface-root 查找」这类改动：surface-root 查找在 S03 里
   `result=0 object=1` 已经成功，跳过它不会改变任何事。
3. **不要**先做重试：目前没有任何证据表明 `texImageIOSurface` 被调用过并失败，
   重试一个没被调用的函数是空转。
4. 若探针证明是 `texture == 0` 早退：那问题在「调用方没有先绑定纹理」，方向转向
   CA/GL 的 drawable 建立顺序，而不是 EAGL 包装。


---

## 10. B 指令的可行性核查：探针不在内核里（评审问题 1 的回答）

评审问「eagl.c 补 ktrace 编进哪个产物、kernel_sha 会不会变」。实查结论：**编不进内核，
但也因此需要三步才上得了机器**，不是一次 CI 出包能做完的。

证据链：

1. **eagl.c 是用户态库**：镜像 `4a6a853d.img` 里 `eagl macroctx` / `texk IOSurface attached`
   各 0 次，而 `PICASSO_GPU text:` 1 次 —— 文本经内核转发，但产生文本的代码在用户态。
2. **它的产物是 `libpicasso-sb.dylib`**：`scripts/build-picasso-sb-inject.py` 用
   **macOS 的 iPhoneOS SDK** 编译并 ad-hoc 签名（`xcrun --sdk iphoneos`）。
3. **Linux CI 只复用、不重建**：`scripts/picasso_linux_inputs.py:19` 把
   `libpicasso-sb.dylib` 列为**输入**，`:125` 要求 macOS Data seed 里的库 sha256 与审计值一致。
4. **信任是内核里的**：`prepare-picasso-ui-trust.py` 把新库的 CDHash 加进内核 trust bundle；
   `configs/picasso-trust-compat.json` 明写「Data seed 保留卷首次建立时写入的那份副本，
   **重建 libpicasso-sb.dylib 永远不会更新它**」。

⇒ 要把 3 条 ktrace 送上机器，需要：① macOS 侧重编 dylib；② 新 CDHash → 内核 trust 重建
（**kernel_sha 会变，Data 按 seedHash 重建**）；③ 新 dylib 进 Data seed 并重刷 userdata（8.4 GB / 218 s）。
用户已接受「丢桌面」，但这仍是 **Compose 全流程 + Data 重刷**，不是「一次 workflow_dispatch + fastboot boot」。

### 可替代的纯内核路径

现有的 `PICASSO_STACKSHOT` 只能导出**已经完成的 panic stackshot**
（`picasso-usb-console-xnu.patch:2459` 注释原文），没有「运行中按需 stackshot」的入口。
若要问「停住时 backboardd 卡在哪」，需要新写一个「fence 停住 N 秒 → 抓 stackshot」的内核探针：
只改内核、不碰 dylib/trust/Data 镜像，但仍会改 kernel_sha（桌面重建，用户已接受）。

两条路都要一次真实构建。**在选路之前不再出包**——这是本轮唯一未决的事。
