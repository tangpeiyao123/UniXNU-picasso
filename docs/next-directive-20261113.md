# 下一步执行指令（2026-11-13 07:25，评审方下发）

> 目标：**停止试探性测试**。当前设备是唯一可用桌面，任何"再出一版试试"都会引入新变量。
> 本文件只给一条已验证的路线、一个待批准的上机动作、以及明确的停止条件。
> 本文件记录的所有主机侧命令与退出码均已实跑，未打开 COM3、未发串口指令、未动设备。

## 0. 冻结清单（未解除前一律禁止）

- 禁止 `fastboot flash boot` / `fastboot erase userdata`（后者 06:19 已证实会擦掉整分区与 root image）。
- 禁止重新 CI 出包、重编内核、改 boot-args、改 Data 卷、动 PnP、动串口记录器实例。
- 禁止用 `boot-ca349c0c-c0f21761.img`、`boot-4c199744-3285608c.img`、`boot-2a34ab07*`（含 `+trust758`）引导：
  这些相对当前 685 baseline 的门禁都是 **RC=3**，会重建 Data 身份，重启回 685 必 panic。
- 禁止在恢复 SOP `data-volume-recovery.md` §4.1 偏移未核实、TWRP 可用性未确认前执行任何恢复动作。

## 1. 阶段 0（已完成，不要再"验证一遍"）

**结论：图标修复不需要新内核、不需要 CI 出包。"只换 TRU、内核不动"这条路已验完，门禁 RC=0。**

根因（字节级）：当前 boot 分区 `boot-next-685-pscireset.img` 的 `KERNEL~1.TRU` 是 68012 B / 4 模块，
只信任新编库 `ef00fcf8…`；设备 Data seed 里实际那份库是 `46583aef…` / `758a8864…`，
故 07:06 日志出现 `unsuitable CT policy` ×3、`signature error=1` ×2、`metrics installed` 0。

已生成候选镜像（按内容命名，不覆盖旧文件）：

```
D:/k30i-gpu/next/boot-685-pscireset+trustcompat2h-4a6a853d.img   (86,077,440 B)
sha256 4a6a853d98b6baee3a87bdcfc703da664479582d22fe97ee36b566fdff3f08d1
```

生成命令（可复现，退出码不接管道）：

```bash
cd C:/Users/Administrator/WorkBuddy/2026-11-08-19-26-16/k30i-ios
python add_trust_hash.py D:/k30i-gpu/next/boot-next-685-pscireset.img \
  46583aef1e7a10ee8a62ba009ec0318b3a0bfbf2 \
  758a8864a317c2f0696ef5c55b905c67ddd15586 \
  -o D:/k30i-gpu/next/boot-685-pscireset+trustcompat2h-4a6a853d.img
echo "RC=$?"     # 实测 RC=0，TRU 68012 -> 68116 B，6 modules
```

单变量对照（全部实测，勿凭直觉跳过）：

| 项 | 685 baseline | 候选镜像 | 20:01 `+trust758` CI 版 |
|---|---|---|---|
| `KERNEL~1.MAC` sha256 | `6b097f24ecfbe513` | `6b097f24ecfbe513` | `3c649184f23a4045`（不同） |
| `SCRATC` seed sha256 | `9220bb17…` | `9220bb17…` | `9220bb17…` |
| boot-args | 995 B / 52 tokens | 完全一致 | — |
| `KERNEL~1.TRU` | 68012 B / 4 mod | 68116 B / 6 mod | 68116 B / 6 mod |
| TRU sha256 前 16 | `369ea5a1bc4e46f9` | `3aed1e1708e107a8` | `3aed1e1708e107a8` |
| 小模块 cdhash | `ef00fcf8` | `ef00fcf8` / `46583aef` / `758a8864` | 同候选 |
| 门禁 vs 685 baseline | — | **RC=0** | **RC=3** |

即：**候选镜像唯一改动是 TRU；且这份 TRU 与 20:01 CI 产物字节完全相同**（不是新造格式）。
`preboot_data_identity.py` 比的是 `KERNEL~1.MAC` sha256，所以内核不动 ⇒ RC=0 ⇒ 引导不重建 Data 身份。

复跑命令：

```bash
python /d/k30i-gpu/repo-work/scripts/preboot_data_identity.py \
  D:/k30i-gpu/next/boot-685-pscireset+trustcompat2h-4a6a853d.img \
  --baseline D:/k30i-gpu/next/boot-next-685-pscireset.img --payload-helper .
echo "RC=$?"     # 实测 RC=0；对照：20:01 CI 版 RC=3
```

## 2. 阶段 1（唯一允许的上机动作，**须用户显式批准后才执行**）

批准前先记录基线：boot=685、Data UUID `0EBB496E-FB09-4223-991E-22007B6FACD1`、
`stage=4 error=0`、`first=25439066`、20 GiB、当前日志 `console-20261113-070546.log`（19770 帧 / panic 0）。

只允许一次 `fastboot boot <候选镜像>`（**不写分区**）。四条验收判据写进脚本、逐条取真实退出码：

1. `statusbar metrics installed (… battery shift 14.5 mm)` 出现 ≥1；
2. `unsuitable CT policy` = 0、`PICASSO_INJECT signature error=1` = 0；
3. `SpringBoard started` 出现、`panic(` = 0、`NATIVE_OWNER_DATA done stage=4 error=0`；
4. `check_boot_session.py` 对该会话给出 `OVERALL_RC=0 CLEAN`（用 07:12 修过的行首判据版）。

**停止条件（任一命中立即停，不重试、不换参数再试）**：panic、`stage≠4`、0 帧、图标判据未过。
处置：长按断电重启回 685（内核相同、RC=0 ⇒ 不触发 Data 身份 panic），把失败会话原样归档并上报，
**不许自行改 boot-args、改 TRU 或再出一版**。

## 3. 阶段 2（阶段 1 通过后才讨论）

- 通过 → 由用户决定是否 `fastboot flash boot` 固化；固化前仍要跑一次门禁并留回滚镜像路径。
- 未通过 → 图标线按用户要求**暂停并归档**（以 `statusbar-icon-position-proposal.md` 17:58 版本错配口径为准，
  不要沿用 17:11"路径不通"或 `WIFI-STATUS.md` §六"原子替换进 Data"的旧结论），恢复条件写清。

## 4. WiFi / 音频开线（阶段 0 之后、纯主机侧，不碰设备）

两者**共用同一道 SCM/PIL 门**，不是两条独立线。门 0 实测 `SCM_GET_VERSION` = -1、`iscall PIL_*` 22 次全 -1，
hvc(EL2) 结果至今未出。因此只允许做 `WIFI-STATUS.md` §五列出的只读项：

1. 读 `0x8ba00000` 头 64 字节判固件是否预载；
2. 定位 `IO80211Family.kext` 的 `IOKitPersonalities` 匹配条件；
3. 音频 A1′ 只读探针（现有设备是什么、VirtualAudio 在不在）。

**禁止**在 SCM 门判定为通之前写 ath10k / ADSP 驱动（`drivers/` 目前无音频驱动目录）；
任何新内核出包都会撞 RC=3，恢复 SOP 不可用 ⇒ 出包不能上机，先解决"新镜像如何安全引导"。

## 5. 纪律（防止再次盲目测试 / 创造新 bug）

1. 一次只改一个变量；本轮已坐实唯一变量是 TRU，不要再叠加 boot-args / 内核 / Data。
2. 先验后做：门禁 `RC=0` 才允许上机；`RC=3` 默认拒绝，需用户显式确认 + 回滚就绪。
3. 判据写进脚本并真的跑；命令重定向到文件再取 `$?`，**禁止 `| tail` 后读 `$?`**（07:03 的假通过）。
4. 报全量，不挑窗口；"每次能进桌面"需固定 685 连续 3–5 次纯重启逐次报分类，样本 2 次不许下结论。
5. 产物按内容命名、不覆盖同名；结论必须有可复现命令与退出码，未验证一律写"未验证"。
