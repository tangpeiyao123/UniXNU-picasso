# 退回安卓的回退准备 + 100% 启动成功率验收定义

生成时间：2026-11-13 12:1x（主机侧只读整理，**未执行任何设备命令**）

## 0. 当前状态基线（不可口头覆盖）

| 项 | 值 | 证据 |
|---|---|---|
| boot 分区 | `boot-685-pscireset+trustcompat2h-4a6a853d.img` | 09:43:47 `fastboot -S 4M flash boot` → `FLASH_RC=0` / `Writing 'boot' OKAY` / 43.033s |
| 引导方式 | **分区启动**，不是 `fastboot boot` | 09:43 之后所有命令仅为 reboot，无 boot/flash/erase |
| Data | UUID `0EBB496E-FB09-4223-991E-22007B6FACD1`、`stage=4 error=0`、20 GiB | 串口日志逐会话 |
| 内核 banner | `Wed Nov 11 16:14:41 UTC 2026`（685） | 只换 TRU，内核字节未变，**banner 无法区分 685 原版与 trustcompat2h** |
| 全量会话 | 20 个 banner（07:06–12:02），`check_boot_session.py` → `RC=2 DATA_IDENTITY_PANIC` | 12:1x 实跑 |

## 1. 回退就绪度判据（已实跑，可复现）

```bash
cd /d/k30i-gpu/repo-work
python scripts/check_android_rollback.py ; echo "RC=$?"
```

实测结果：

```
RC=3   ANDROID_ROLLBACK=DEGRADED   hard_fail=0 warn=2
[HARD] 14 项全部 PASS：
  D:/k30i-backup/boot-stock.img  134217728 B  sha 0614705392e38dad…  头 ANDROID!
  /d/k30i-firmware/firmware-manifest.json  source = "live partitions dumped
          read-only via TWRP 3.5.2"，6 个分区镜像逐个 size+sha256 校验通过
          (modem / persist / fsg / modemst1 / modemst2 / bluetooth)
  BACKUP-RESTORE.md、userdata-root.sparse.img (8380243396 B)、
  boot-next-685-pscireset.img、boot-685-pscireset+trustcompat2h-4a6a853d.img
[WARN] getvar all 快照 (device-info.txt) 缺失 → current-slot / 分区表 / anti-rollback 未落盘
[WARN] EDL 9008 firehose / rawprogram / flash_all 缺失 → 进不了 fastboot 时无救援
[WARN] chunks/ 215 个分块 PASS（21 GiB）
```

对照（验证判据本身有效，不是空跑）：

```bash
EXPECT_BOOT_STOCK_SHA=0000…0000 python scripts/check_android_rollback.py ; echo "RC=$?"
# RC=2  ANDROID_ROLLBACK=BLOCKED  stock boot FAIL MISMATCH
python -m py_compile scripts/check_android_rollback.py   # RC=0
```

**结论：L2（走 fastboot 回原厂安卓）的硬资产已备齐且逐字节校验通过；L3（EDL）未准备。**

## 2. 三档回退（按代价从小到大）

### L1 — 回 iOS 同内核基线，保留 Data（仅限"内核没变过"）

```bash
cd "/c/Users/Administrator/WorkBuddy/2026-11-08-19-26-16/k30i-ios"
python /d/k30i-gpu/repo-work/scripts/preboot_data_identity.py \
  D:/k30i-gpu/next/boot-next-685-pscireset.img \
  --baseline D:/k30i-gpu/next/boot-685-pscireset+trustcompat2h-4a6a853d.img \
  --payload-helper . ; echo "GATE_RC=$?"
# 实测 GATE_RC=0  kernel identical: True  seed identical: True  uuid identical: True
/d/ADB/fastboot.exe -S 4M flash boot D:/k30i-gpu/next/boot-next-685-pscireset.img
```

- 代价：**丢掉图标兼容哈希**（`signature error=1`、`metrics installed 0`），Data 卷不动。
- 边界：一旦引导过**重编内核**（新 `kernel_sha`），Data 身份会被重建，此时 L1 不再成立，只能走 L2 或恢复 SOP §4.1（该节两个偏移超出 userdata 分区范围、TWRP 未核实，当前禁止执行）。

### L2 — 回原厂安卓，放弃 iOS Data（**不可逆，须用户显式批准**）

```bash
# 0) 只读，先落盘（补 WARN 缺口）
/d/ADB/fastboot.exe devices
/d/ADB/fastboot.exe getvar all > D:/k30i-backup/device-info.txt 2>&1
#    从输出确认 current-slot / partition-size:userdata / anti-rollback 版本
# 1) 记录当前 iOS 基线：Data UUID / stage / geometry / 当前日志末尾时间戳
# 2) 写 boot
/d/ADB/fastboot.exe -S 4M flash boot D:/k30i-backup/boot-stock.img ; echo "FLASH_RC=$?"
# 3) 清掉被 iOS 占用的 20 GiB Data（回安卓语境下是必需步骤）
/d/ADB/fastboot.exe -w
# 4) 校准分区异常时才有必要（这几份是活机 dump，sha 已校验）
#    fastboot flash persist  D:/k30i-firmware/persist.img
#    fastboot flash modemst1 D:/k30i-firmware/modemst1.img
#    fastboot flash modemst2 D:/k30i-firmware/modemst2.img
# 5) 重启 + 回读校验
/d/ADB/fastboot.exe reboot
#    BACKUP-RESTORE.md §4：dd 回读 boot 与 boot-stock.img 做 SHA256 比对，不一致别重启
```

- ⚠️ **口径不要混**：`fastboot erase userdata` 在 iOS 语境是灾难（会抹掉 root image，06:19 事故），但回安卓时清 userdata 是目标动作；`-w` 会**不可逆清除 Data UUID `0EBB496E` 和当前桌面**。
- ⚠️ 回 iOS 需要重刷 `D:/k30i-ios/userdata-root.sparse.img`（7.9 GiB，06:31 实测 218 s 成功），不是"重启就回来"。
- ⏳ 未验证：vbmeta / AVB 状态、dtbo 是否被项目改过、`current-slot`（06:18 的 `getvar all` 输出未含该行，且完整输出未保存）。**不确认这三项之前，L2 属于"可执行但未演练"**。

### L3 — 进不了 fastboot 时的 EDL 9008 救援：**当前不可用**

- 缺 `prog_*.mbn` firehose、`rawprogram*.xml`、`flash_all.bat`。
- 必须补齐并做**离线校验**（文件齐全 + 机型匹配 + 工具能识别），不能靠"到时候再下载"。
- 这是"退回安卓的路"目前唯一的真空：**L2 依赖 fastboot 仍可达**。

## 3. "启动成功率 100%" 的验收定义（不许口头承诺）

### 3.1 现有全量数据（不挑窗口）

截至 12:1x，按行首 banner 切出 **20 个会话**：

| 类别 | 次数 | 特征 |
|---|---|---|
| 出过帧（WorkBuddy 判"成功"） | 9 | 其中 #1 683 帧长稳；#12 1000 帧但约 10 fps；#20 360 帧后 `hangcheck` 冻结 |
| seed 类 panic | 6 | `provider=ram` → 210–240 s → `stage=0 error=2` → 自动重启 |
| 0 帧 / GPU 无帧 | 5 | 不 panic、不自动重启，只能长按断电 |

WorkBuddy 12:03 报的 19 会话（07:06–11:52）= 8 / 6 / 5、42%，与独立统计**逐会话吻合**；12:02:46 新增第 20 会话。全量 `check_boot_session.py` → `RC=2`。

### 3.2 "成功"判据必须收紧（否则数字没意义）

现在的"成功 = `stage=4` + 出过 ≥1 帧"会把"出几帧后 GPU hang"算成成功。第 20 会话两次取样（12:09:05 / 12:10:02）`DPU_FRAME` 都停在 `n=360`，`hangcheck polls` 581→689 而 `fence=1056` 冻结 ⇒ 确认 hang，不是掉帧。

**新判据（写进脚本、逐会话跑）**：`panic(=0)` + `stage=4 error=0` + 连续出帧 ≥ 60 s + 该窗口内 `hangcheck` 无 `fence == hang_fence` 冻结 + `SpringBoard started` + `check_boot_session.py` 该会话 `CLEAN`。任何一条不满足即失败。

### 3.3 验收方式

- 固定**单变量**：同一内核（trustcompat2h）、同一引导方式（分区启动）、同一 Data、不刷不擦；只做纯重启。
- 连续 N 次（建议 ≥ 10）逐次记录，报**全量**，用脚本给每次的退出码；`OVERALL_RC=0` 且全部 CLEAN 才算"这一组通过"。
- **100% 只能对有限样本成立**，不能承诺"永远 100%"。要先分别消除两类故障：seed 类（当前 6/20）与 GPU 类（当前 5/20，收紧判据后可能更高），各自做单变量对照，禁止一次改多处。
- 查根因目前需要"只加日志"的重编内核 → 门禁 RC=0 变 RC=3。**在 L2 就绪并被用户确认前不得解冻**；L2 就绪只是把"刷坏了救不回来"变成"能回安卓但会丢 Data"，仍需显式批准。

## 4. 冻结与禁止（沿用 incident-20261113-lessons.md §十一）

1. 禁止重新 CI 出包 / 重编内核，除非解冻条件全部满足且用户显式批准；
2. 禁止 `fastboot erase userdata`（回安卓时改用 `-w`，且须单独确认）；
3. 禁止用 UUID / `DATA_INIT phase=2` 证明"Data 身份未变"；
4. 门禁 `rc=3` 默认拒绝引导；命令不接管道，取真实 `$?`；
5. 判状态必须带日志末尾时间戳、连续两次取样，禁止一次性快照下"停住/掉线"结论。

## 5. 待办缺口（把 DEGRADED 变成 READY）

| # | 事项 | 动作 | 是否碰设备 |
|---|---|---|---|
| 1 | `device-info.txt` | `fastboot getvar all` 完整输出落盘，确认 `current-slot` | 只读 |
| 2 | EDL firehose | 找齐机型对应的 `prog_*.mbn` / `rawprogram*.xml` / `flash_all.bat` 并离线校验 | 否 |
| 3 | TWRP | manifest 写的是 11-09 的历史 dump；**当前**能否引导进 TWRP 未核 | 只读验证 |
| 4 | vbmeta / AVB / dtbo | 核实是否被项目改过，避免刷回原厂 boot 后校验失败 | 只读 |
| 5 | `BACKUP-RESTORE.md` §7 | 现写"数值只是示例、照抄没意义"，但实测 sha 与文档完全一致，应改成"这就是本机真备份，可直接用于校验" | 否 |
| 6 | 收紧判据 | 把 §3.2 写进 `check_boot_session.py` 并用 20 会话样本做前后对照 | 否 |
