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

### 3.2 卡在哪个调用之前

- 最后一条 EAGL 调用是 `getMacroContextPrivate`（对应 #11 `eagl macroctx`），**返回了非空 table**；
- 之后 S03 立刻做了 `IOCoreSurfaceRoot` 查找：`PICASSO_IOKIT end … rpc=0 result=0 object=1` —— **服务打开成功**；
- 但整段日志里 `*urface` 只出现 **1 次**（就是上面这条查找），**没有任何 IOSurface 分配/绑定**；
  对照 S11 有 13 次（2 次 texk attach/show + SpringBoard 的 IOSurfaceRoot 查找）。
- 期间 backboardd 没有死：它继续做 IOKit、FBUC（`call=10 selector=12 ret=0`、WLED power=1）、brightness，
  SpringBoard 也起来了（106 行）。

⇒ 结论只写到这里：**卡在「用户态拿到 IOSurface 并调用 texImageIOSurface」之前**，
具体是「surface 没被创建」还是「创建了但没送到 GL」，现有日志分不开——
内核侧没有 IOSurface 外部方法的探针。要分，只能加诊断（见 §6）。

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
| `scripts/reproduce_desktop.py` | `--min-frames`（默认 0，不改旧行为）；`<cmd-file>.lock` 单实例锁 |
| `k30i-ios/console_recorder.py` | 端口掉线写 `CAPTURE GAP BEGIN`，重连写 `CAPTURE GAP END gap_s=`；停止时若仍在 gap 内写 `CAPTURE GAP OPEN AT STOP`。日志句柄本就是 `buffering=0`，主机侧字节不丢；**设备在断线期间打印的内容无法回填**（没有重放通道），所以只标记窗口，供下游判「未观测」而不是「静默」 |

验证：`py_compile` 三个文件 `RC=0`；`measure` 实跑 `RC=0`；gap 标记用临时日志跑过（begin 幂等、end 带时长、停止时告警）；
锁用两个进程实测（第二个 `RC=2`）。
