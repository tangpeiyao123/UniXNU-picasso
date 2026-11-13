# 进桌面稳定性：10 次自循环的归因（2026-11-13 23:55，11-14 00:30 校正）

> 触发：用户选 A（先修进桌面稳定性）。
> 数据：`D:/k30i-gpu/console/console-20261113-220343.log`（11 段 banner 会话），
> `reproduce_desktop.py --cycles 10 --window 300 --cmd-file …`，22:08:15 起、22:51 收尾。
> 本文只写实测行号与字节证据；推测单列「未验证」。

---

## 0. 口径校正（上一版报错，此处认）

| 项 | 上一版 | 实测（`measure_desktop_reach.py`，RC=0） |
|---|---|---|
| 通过率 | 20%（2/10，运行时口播）与「3 个 PASS」混用 | **3/11 = 27.3%** |
| 分母 | 10（脚本只跑到 cycle 10） | **11**（第 11 段是 cycle 10 judge 之后才开始的新会话） |

两个数都不算错，但必须带分母说清楚：
- 运行时脚本计的是它数到的 10 个 cycle：**2 PASS / 10**；
- 全量日志按 banner 切是 **11 段：3 PASS / 8 STALL = 27.3%**（第 11 段 S11 是测试结束后的那次启动，
  它一直活到现在、确实出帧）。

**并且 PASS 不等于「可见桌面」**：判据 `vsync>=17 且 desktop>0` 只是
`PICASSO_DISPLAY vsync: kick/arm` 的日志行数（打印上限 8 → 17 行）加 UI 进程名出现。
本次 3 个 PASS 里，**只有 S11 有 `DPU_FRAME`（21 行）**；S04/S08 是 0 帧，因为脚本在
判 PASS 后立刻软重启，把它们掐在首帧之前（cycle 4：22:19:42 起、22:20:42 判 PASS → 60 s 就重启）。
→ 27.3% 应读作「**显示管线 armed + UI 栈起来**」，不是「屏幕上看得见桌面」。
要判桌面，PASS 必须要求 `DPU_FRAME` 连续增长（≥30 帧）或用户目视确认。

---

## 1. 全量 11 会话

`sess vsync kick desktop panic frames eagl texk verdict subtype`

| 会话 | vsync | desktop | panic | frames | eagl | texk | 判定 | 分型 |
|---|---|---|---|---|---|---|---|---|
| S01 | 1 | 0 | 0 | 0 | 0 | 0 | STALL | STALL_SILENT |
| S02 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | STALL_SEED |
| S03 | 2 | 313 | 0 | 0 | 1 | 0 | STALL | STALL_NO_DRAWABLE |
| S04 | 17 | 246 | 0 | 0 | 1 | 1 | PASS | - |
| S05 | 2 | 313 | 0 | 0 | 1 | 0 | STALL | STALL_NO_DRAWABLE |
| S06 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | STALL_SEED |
| S07 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | STALL_SEED |
| S08 | 17 | 247 | 0 | 0 | 1 | 1 | PASS | - |
| S09 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | STALL_SEED |
| S10 | 1 | 0 | **1** | 0 | 0 | 0 | STALL | STALL_SEED |
| S11 | 17 | 5568 | 0 | **21** | 1 | 1 | PASS | - |

**唯一干净的分水岭是 `texk IOSurface attached`（首个 drawable 附着）**：
打过的 3 段（S04/S08/S11）全部 PASS，没打的 8 段全部 STALL，零例外。
`eagl init gl=` 不够——S03/S05 也打了，但没走到附着。

---

## 2. 分型一：SEED panic（5 次，单一根因）

**结论**：同一 caller、同一签名、同一时刻。

```
panic(cpu N caller 0xfffffff007946a38): offline scratch Data mount failed: 2 @kern_exec.c:9908
```
S02/S06/S07/S09/S10 各一条，无其他 panic 文案。

**时刻证据（uptime 来源行）**：每条 panic 块内的 `mach_absolute_time`：

| 会话 | 日志行 | mach_absolute_time | @24 MHz | @19.2 MHz |
|---|---|---|---|---|
| S02 | 2679 | `0x16a637897` | 253.3 s | 316.7 s |
| S06 | 11980 | `0x16b1db064` | 253.8 s | 317.2 s |
| S07 | 13517 | `0x16a6c51f4` | 253.4 s | 316.8 s |
| S09 | 17416 | `0x16b1d6ded` | 253.8 s | 317.2 s |
| S10 | 18968 | `0x16b23c1bb` | 253.9 s | 317.4 s |

- 24 MHz 是 **假设**（ARM generic timer 常见值），日志里没有 CNTFRQ 行，未直接读到。
- 但五个数的**跨度只有 0.6 s**，与 CNTFRQ 无关；固定定时器特征成立。
- 旁证 24 MHz 更合理：S02 从 `provider=ram`(2266) 到 panic(2664) 有 395 行，即 240 s 内约
  1.6 行/s，与 host 侧实测 hangcheck ≈1–1.7 行/s 同量级；若按 19.2 MHz 则该窗口只剩 77 s，
  要求 5 行/s，明显偏高。

**代码路径**（`NativeRamBlock.cpp` / `selectVolumeSeed`）：

```c
if (&native_ufs_store_mode && native_ufs_store_mode()==2) {
    __atomic_store_n(&native_ram_block_attach_requested,1U,__ATOMIC_RELEASE);
    unsigned waited=0;
    while(!native_ram_block_ready && waited++<2400) IOSleep(100);  // 240 s
    if(native_ram_block_ready!=1) return 0;                        // -> ENOENT
}
```
`kern_exec.c` 拿到 `data_error=2` 后 `panic("offline scratch Data mount failed: %d")`。
日志同段可见 `NATIVE_OWNER_DATA provider=ram`(2266) → `done stage=0 error=2`(2661)。

**判别证据**：`PICASSO_DATASTORE geometry` 由持久化 worker 在 `attachStore()` 里、发布前打印。

- 有 geometry 的 5 段：S03/S04/S05/S08/S11 —— **全部走到 UI**；
- 没有 geometry 的 6 段：5 段 panic + S01。

成功段里 geometry 紧贴 `provider=ram` 下一行（S03: 3782→3783）⇒ **秒级发布**；
失败段 240 s 内一次都没有 ⇒ **双峰：要么秒级，要么根本不发布**，不是「挂载慢」。

---

## 3. 分型二：STALL_NO_DRAWABLE（2 次，S03/S05）

两段镜像式相同：`desktop=313`、`vsync=2`、`frames=0`、`eagl=1`、`texk=0`。

- 挂载是成功的：`NATIVE_OWNER_DATA done stage=4`、`PICASSO_DATASTORE ready=1`；
- UI 栈起来了：`backboardd` 206 行、`SpringBoard` 106 行；
- GL 客户端起了：`eagl init gl=0x…`、`eagl macroctx 0x…`、`client: calls=53 last=13/0`；
- **但没有 `texk IOSurface attached`** —— 首个 drawable 从未附着，之后只有
  `metal later: opens=1 foreign=0` 和空转；
- GPU 侧：`hangcheck polls 2→461 / 2→509`，`fence==hang_fence==19` 恒定不变。

所以这一类不是「GPU 挂死」，而是**渲染路径在拿到第一个 surface 之前停住**；
fence=19 是「初始化后没有新提交」的被动结果。

---

## 4. 分型三：STALL_SILENT（1 次，S01，未定论）

S01 是记录器 22:03:43 打开端口时正在跑的那次启动，**先于本轮测试**。
它只有 1157 行内核输出、80 条 hangcheck（polls 2→139、fence/hang=19/19）、
`provider=ram` 有、`NATIVE_OWNER_DATA done stage=4` 没有、`PICASSO_DATASTORE geometry` 没有。

关键：**22:07:38 端口掉了**（`PORT FAILED … CreateFile(COM3) failed: 433`），
22:08:08 才重开并立刻看到下一个 banner。也就是说 panic 文本（若有）落在端口断开窗口里，**丢了**。
它的里程碑形状与 SEED 完全一致（provider=ram 后无 stage=4），因此
**更可能是第 6 次 SEED panic 丢了日志**，而不是独立的第三类故障。
按纪律不并入 SEED，单列「未定论」；要定论只能等下次抓全。

---

## 5. 「retry 在不在镜像里」——精确核（回应评审）

评审指出镜像里有 `selectVolumeSeed` / `native_ram_block_volume_seed`。**这点是对的，我上一版措辞含糊**。
精确说法：**基函数在镜像里，但 retry 补丁新增的代码不在。**

镜像 `D:/k30i-gpu/next/boot-685-pscireset+trustcompat2h-4a6a853d.img`（86,077,440 B）实测：

| 字符串 | 次数 | 性质 |
|---|---|---|
| `selectVolumeSeed` | 2 | 基符号（补丁之前就有） |
| `native_ram_block_volume_seed` | 2 | 基符号 |
| `offline scratch Data mount failed` | 1 | 基字符串 |
| `PICASSO_DATASTORE geometry` | 1 | **对照：另一补丁新增的串，确实进了镜像** |
| `PICASSO_DATASTORE ready=` | 1 | 对照 |
| `PICASSO_SEED enter` | **0** | `4c19974` 新增 |
| `PICASSO_SEED wait ready` | **0** | `4c19974` 新增 |
| `NATIVE_OWNER_DATA seed retry` | **0** | `ca349c0` 新增（`printf("NATIVE_OWNER_DATA seed retry attempt=%u\n", …)`） |

方法说明：`printf/IOLog` 的格式串是编译期常量，补丁被编进去就必然留下该串；
同一镜像里另一补丁的串（`PICASSO_DATASTORE geometry`）能被数到 1，说明检索方法本身有效。
→ 结论只取「**retry 补丁不在镜像**」；基函数在镜像属正常，与我上一版结论不矛盾，但措辞应改为
「retry 的**新增代码**不在镜像」，不再写「镜像不含 retry」。

---

## 6. 下一版诊断构建建议（只写建议，未构建、未 flash）

1. **只带诊断**：`picasso-seed-diag-ramblock.patch`（log-only）本就随构建应用；
   `data-seed-retry` 继续保持 held（见 `patches/held/README.md`：40×240 s = 9600 s 风险）。
2. 目标读数（失败会话必出现其一）：
   - `PICASSO_SEED wait ready=0 waited=N` 一直涨到 2400 → worker 卡住没发布；
   - `PICASSO_SEED ready timeout=2` → worker 明确拒绝，配合同段
     `NativeRamBlock: persistent store refused:` 或 `PICASSO_DATASTORE fallback-refused reason=` 看条件；
   - `PICASSO_SEED waitMatching begin/end` 的时间差 → UUID 匹配是否又是一段等待。
3. 修 SEED 的正确方向（拿到读数后再动）：把 `selectVolumeSeed` 的等待与 retry **共享同一个 deadline**，
   而不是在外面套 40 次。**不要**直接把 240 s 调小——那只是把 panic 提前，
   除非读数证明 `ready` 永远不为 1。
4. 判据同步改：`reproduce_desktop.py` 的 PASS 改要求 `DPU_FRAME` 连续增长（≥30 帧），
   否则 27.3% 会被读成「桌面率」而实际只是「显示管线 armed」。

---

## 7. 附：本轮脚本改动

`scripts/measure_desktop_reach.py` 分型改为按「走到了哪一步」判，并新增 frames/eagl/texk 三列：

| 分型 | 判据 |
|---|---|
| `STALL_SEED` | 有 `panic(` 行 |
| `STALL_AFTER_DRAWABLE` | 有 `texk IOSurface attached` 但仍无帧 |
| `STALL_NO_DRAWABLE` | 有 `eagl init gl=`、无 `texk IOSurface attached` |
| `STALL_NO_GL` | 有 UI 标记、无 `eagl init gl=` |
| `STALL_SILENT` | 无 UI 标记、无 panic |

并在汇总里固定打印一句：`PASS = display armed + UI up, NOT a frame count`。
`py_compile RC=0`；实跑本日志 `MEASURE_RC=0`，输出即第 1 节表。
