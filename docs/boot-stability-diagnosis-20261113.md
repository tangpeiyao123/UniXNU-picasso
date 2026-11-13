# 进桌面稳定性：10 次自循环的归因（2026-11-13 23:55）

> 触发：用户选 A（先修进桌面稳定性）。
> 数据来源：`D:/k30i-gpu/console/console-20261113-220343.log`（11 个 banner 会话），
> 运行 `reproduce_desktop.py --cycles 10 --window 300 --cmd-file …`，22:08:15 起、22:51 收尾。
> 本文只写实测行号与字节证据；推测单列「未验证」。

---

## 1. 全量 11 会话（不挑窗口）

| 会话 | banner 行 | panic | DATASTORE geometry | 帧行 | vsync | desktop 标记 | 末 fence |
|---|---|---|---|---|---|---|---|
| S01 | 111 | 0 | 0 | 0 | 1 | 0 | 19 |
| S02 | 1284 | **1** | 0 | 0 | 1 | 0 | 19 |
| S03 | 2817 | 0 | 1 | 0 | 2 | 313 | 19 |
| S04 | 5484 | 0 | 1 | 0 | 17 | 246 | 21 |
| S05 | 7824 | 0 | 1 | 0 | 2 | 313 | 19 |
| S06 | 10576 | **1** | 0 | 0 | 1 | 0 | 19 |
| S07 | 12115 | **1** | 0 | 0 | 1 | 0 | 19 |
| S08 | 13653 | 0 | 1 | 0 | 17 | 247 | 21 |
| S09 | 16000 | **1** | 0 | 0 | 1 | 0 | 19 |
| S10 | 17553 | **1** | 0 | 0 | 1 | 0 | 19 |
| S11 | 19101 | 0 | 1 | **16** | 17 | 5568 | 1206 |

- `帧行` = `PICASSO_DPU_FRAME n=` 行数（驱动每 30 次 flip 打一行，`NativeLegacyDisplay.cpp:701`）。
- S01 是记录器起停时的残段（1173 行、无 geometry、无 panic），不计入结论分母。

---

## 2. 结论一：5 次 panic 全是同一个 240 s 定时器到期，不是随机故障

**判据（可复现）**：panic 帧里的 `mach_absolute_time` ÷ 24 MHz。

| 会话 | mach_absolute_time | 换算 uptime |
|---|---|---|
| S02 | `0x16a637897` | 253.3 s |
| S06 | `0x16b1db064` | 253.8 s |
| S07 | `0x16a6c51f4` | 253.4 s |
| S09 | `0x16b1d6ded` | 253.8 s |
| S10 | `0x16b23c1bb` | 253.9 s |

五次跨度 **0.6 秒**。原因是 `NativeRamBlock.cpp` 里的固定等待：

```c
// selectVolumeSeed(): mode==2 时先等持久化 worker 发布
__atomic_store_n(&native_ram_block_attach_requested,1U,...);
unsigned waited=0;
while(!native_ram_block_ready && waited++<2400) IOSleep(100);   // 2400 × 100 ms = 240 s
if(native_ram_block_ready!=1) return 0;                          // -> ENOENT
```

随后 `kern_exec.c` 拿到 `data_error=2`：

```c
int data_error = native_owner_data_prepare(p, scratch_addr);
if (data_error) panic("offline scratch Data mount failed: %d", data_error);
```

日志同一会话里先有 `NATIVE_OWNER_DATA provider=ram`、再有 `NATIVE_OWNER_DATA done stage=0 error=2`
（S02: 2266 → 2661）⇒ 失败点就是这次 seed 查询，不是挂载本身。

**判别证据（决定性）**：`PICASSO_DATASTORE geometry` 这行由 worker 的 `attachStore()` 在发布前打印。

- 6 个非 panic 会话（S03/04/05/08/11 及残段 S01 除外）**全部有** geometry 行；
- 5 个 panic 会话**全部没有** geometry 行。

即：**worker 在 240 s 内从未走到 attachStore**，而不是「挂载慢一点」。
成功会话里 geometry 紧贴 `provider=ram` 下一行（S03: 3782→3783），即 worker 是**秒级发布**。

→ 这个故障是**双峰**的：要么立刻发布，要么 240 s 内根本不发布。没有观察到「慢但成功」。

---

## 3. 结论二：当前跑的内核根本没有后来写的两条修复（字节证据）

内核 banner 构建时间：`Wed Nov 11 16:14:41 UTC 2026`。
拿镜像 `D:/k30i-gpu/next/boot-685-pscireset+trustcompat2h-4a6a853d.img`（86,077,440 B）直接数字符串：

| 字符串 | 出现次数 | 含义 |
|---|---|---|
| `PICASSO_DATASTORE geometry` | 1 | mode1-guard 补丁在（11-12 之前） |
| `PICASSO_SEED wait ready` | **0** | `4c19974`（11-12 22:37）诊断**不在** |
| `NATIVE_OWNER_DATA seed retry` | **0** | `ca349c0`（11-12 20:34）重试**不在** |

也就是说，现在这版内核比仓库 HEAD 少两次提交，而这两次提交恰好是：
①唯一能定位「worker 卡在哪」的日志；②唯一针对本条 panic 的改动。
今晚 10 次里 5 次 panic，用的是**没有这两条修复**的内核。

---

## 4. 结论三：`ca349c0` 的重试按现在的写法不能直接上，会放大 40 倍

`picasso-data-seed-retry-xnu.patch` 在 `kern_exec.c` 里循环 40 次调用
`native_ram_block_volume_seed()`，而 **240 s 等待就在这个函数内部**（`selectVolumeSeed`）。
一次失败 = 240 s，40 次 = **9600 s（2 小时 40 分）**才 panic。
所以它不但救不了失败会话（实测失败是「永不发布」，不是「慢」），还会把 4 分钟的
panic-自愈变成数小时的挂死。

**已处理**：把该补丁移出构建（`patches/held/`），并从 `unixnu` 的 `LATE_OVERLAYS` 摘掉，
注释写明理由与重新启用的前置条件。这是本轮唯一的代码改动，未构建、未碰设备。

---

## 5. 结论四：现在的 "PASS" 判据不等于「进桌面」

`reproduce_desktop.py` 判 `vsync>=17 且 desktop>0`。但 vsync 只是
`PICASSO_DISPLAY vsync: kick/arm` 的**日志行数**（8 次 kick + 8 次 arm + 1 ≈ 17，打印上限 8）。
本次 3 个判 PASS 的会话里：

- S04、S08：`vsync=17`、desktop≈246，但 **DPU_FRAME = 0 行**（0 帧）；
- S11：`vsync=17`、desktop=5568，DPU_FRAME 16 行（n 到 480，即 480 次 flip）。

`vsync=17` 只说明**显示管线被 arm 过**，不说明屏幕上真的画出了东西。
因此昨天报的 **20%（2/10）应改口径为「显示管线 armed + UI 栈起来」，不等于「可见桌面」**。
要判「桌面」，必须同时有连续 `DPU_FRAME` 增长（≥30 帧）或用户目视确认；
否则无法区分「静止桌面」和「黑屏/冻结」。

---

## 6. 下一步（需要一次构建，未执行）

单变量实验：**只加日志，不改行为**（seed-diag 已是 log-only，retry 已摘掉）。

1. CI 出包（`build-kernel-linux.yml` workflow_dispatch），比对两点：
   - `PICASSO_SEED wait ready=` 在失败会话里每 20 s 打一次，能看出 worker 是 `0`（还在跑）还是 `2`（已拒绝）；
   - 若 `2`：同一会话应出现 `NativeRamBlock: persistent store refused: <reason>` 或
     `PICASSO_DATASTORE fallback-refused reason=...`，直接给出拒绝条件。
2. 设备侧：分区启动，连续 10 次（软重启自循环已可用），每次独立日志。
3. 验收：先只看 `PICASSO_SEED` 三行（enter / wait / timeout 或 matching done），
   不碰 boot-args、不 flash、不改 Data。

**冻结令仍生效**：本轮只做了主机侧（移动补丁、改 `unixnu` 列表、写文档），
没有 dispatch、没有 flash、没有碰设备。
