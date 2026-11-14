# Data 卷身份重建导致「重启进不了桌面」：复现、判据与恢复

日期：2026-11-13
状态：**根因为高概率推断，尚未由当前故障的串口日志坐实**（见 §3）
适用：红米 K30 5G / SM7250 / lito / picasso，iOS 16.5 XNU (UniXNU)

---

## 0. 先更正我自己的一个错误结论

我在 2026-11-12 23:53 的评审里把故障归因为「XNU 挂死后未 clean unmount，
Data 卷留下脏状态，下次启动挂载失败」。**这个说法是错的，在此撤回。**

真实机制项目里早有记录（`report-hw-firmware.md` §4.8 / §9，以及
`ed4075b9` 会话 L3095 / L3108 / L3137）：

> 新内核在启动时会按**自己的 seedHash 算法**重建 Data 卷。之后 boot 分区里的
> 旧内核认不出这块盘 → `NATIVE_OWNER_DATA stage=0 error=2` →
> `offline scratch Data mount failed: 2 @kern_exec.c:9908` → panic。
> 且切换是**单向**的（新内核建的盘，旧内核也认不出）。

L3095 原文还说了一句最关键的：**「你现在看到的桌面是内存里的镜像，
撑到重启为止。」** 这与用户描述的「下午内部刷好的 boot 每次能进桌面 →
中间全用 fastboot boot → 现在重启进不了桌面」完全吻合。

所以：**`fastboot boot` 不写 boot 分区，但会写 Data 卷。
「没刷机」不等于「没改设备」。** WorkBuddy 说「没动」在"没刷分区"这个
字面意义上成立，但作为"设备状态没变"的解释是误导。

---

## 0.1 00:28 复核更正：本文「重启后失败」这一前提尚未成立

复核 `ed4075b9` L12884–12960 后追加。**本次故障是"仍卡在旧会话"还是
"重启后挂载失败"未定，下面每条都可复跑；在拿到日志前不得执行 §3 恢复 SOP。**

1. 屏幕照片（`clipboard-2026-11-12T16-16-59-505Z-7d1fbfb4.jpg`，用户 00:16:59 上传）
   可见 `PICASSO_GPU hangcheck polls=875…925 fence=19` 与
   `PICASSO_GPU submits live 13: submissions=1`，但**没有 Darwin 横幅**。
   → 只能证明"存在一个卡在 GPU fence=19 的会话"，不能定位是哪次启动。
2. 磁盘反查：`polls=923` 在 `run-D1-warm.log` 中 **0 次**（该文件 polls 峰值 666，
   文件恰为 196608 B，疑似采集截断）；`submits live 13` 在 D1 中 0 次、
   在 `run-M3-retry.log` 中 1 次；`polls=923` 仅见于历史
   `run-cold1.log` / `run-R8-685.log` / `stuck.log`。polls 是**会话内从 0 累加**
   的计数，任何卡 GPU 的启动都会涨到 900+，不是某次启动的指纹。
3. 用户 00:21:06 明确说「我刚已经是断电重启的」；00:16:59 的照片早于该陈述，
   **不能用旧照片否定重启后的状态**（证据时点错配）。
4. 截至 00:28 **仍无任何新串口日志**（本轮 22 次工具调用中串口读全部 0 B /
   超时 / `OSError(22,…,121)`）。故"重启后旧内核挂载失败"这条链
   **无运行期日志支撑**。

另：三份日志 `run-M3-retry.log:985`、`run-D1-warm.log:923`、
`run-A2-685.log:1086` 均为
`PICASSO_DATA_INIT phase=2 … / complete=1 / DATASTORE ready=1`——
**Data 卷初始化在挂死的 D1 里也成功了**，"初始化阶段失败"同样不成立。

重建开关的源码结论（`repo-work/drivers/NativeUfs/`）：
- `PicassoPersistence.h:76` `PE_parse_boot_argn("picasso-data-replace-20g", &request, …)`
- `PicassoPersistence.h:94` `gPicassoDataInit.begin(…, request==1, …)`
- `PicassoDataInit.h:26` `if(phase==2){complete=true;return 2;}`

→ `phase=2` 表示"已完成/恢复"，**触发重建需 boot-arg
`picasso-data-replace-20g=1`**，不是改 CRC。`patch_kernel2.py` 是把内核写死的
作者设备几何改成本机 107 GiB 真实值的**适配补丁**（OURS:
RegionFirst=25439066 / headerCrc=0xFE827362 / entriesCrc=0xFD3CB4C4 /
last=30944250），是让守卫通过的前提，不是故障源。

**结论**：先起串口 capture 拿到本次启动的 Darwin 横幅，用
`scripts/check_boot_session.py` 分类，再决定是否需要重建。

更该认的一点是：**护栏本来就有**。§9 已写明「换内核前必须先处理 Data 身份，
两个方向都要抹存储头 + 初始化日志」。今天不是"不知道"，是**没有把这条
护栏做成开机前强制判据**，纯靠人记，于是又踩了一次。

---

## 1. 已证实的证据（每条都标了来源）

| # | 事实 | 来源 |
|---|---|---|
| 1 | 数据身份切换会 panic，且单向不兼容 | `report-hw-firmware.md:462-465, 544-546` |
| 2 | 恢复办法 = 抹存储头 + 初始化日志，让内核重建（`DATA_INIT phase=1`） | 同上；`L3095`；`L3108` |
| 3 | 20:46 曾**成功恢复过一次**：`DATA_INIT phase=1` → `complete=1` → `OWNER_DATA stage=4 error=0` → `SpringBoard` | `ed4075b9` L3108、L3137 |
| 4 | 恢复后明确记录「boot 分区里那个镜像没动过，仍是 32 fps 基线」 | L3137、L540-542 |
| 5 | 今天 M3 与 D1 都走到 `NATIVE_OWNER_DATA done stage=4 error=0`，即确实接管/重建了 Data 卷 | `next/run-M3-retry.log`、`next/run-D1-warm.log:1082` |
| 6 | `fix_data_uuid.py --show` 三份镜像内核 UUID 与种子 7 份副本**全一致**（`0EBB496E…`） | 本机实跑，见 §5 |
| 7 | 三份镜像 **seed sha256 完全相同**(`9220bb17…`)，但**内核 banner / kernel sha 各不相同** | 本机实跑，见 §5 |
| 8 | 全天 `fastboot`/`dd`/`flash` 类**实际执行的工具命令数 = 0** | 扫描 `ed4075b9` 全部 12852 行 tool_use |
| 9 | **23:27 之后没有任何新日志**；含 `offline scratch Data mount failed` 的只有历史 `run-M2-trust5.log:1344`、`run-R7-685.log:1480` | `find` + `grep -rln` 全盘 |
| 10 | `fence=19` / `hangcheck` 在**成功启动里也出现**（M3: 18 / 486，同时 385 帧） | `next/run-M3-retry.log` |

第 7 条是这次最要紧的发现：**seed 相同、UUID 相同，也不能证明身份安全**。
风险来自内核侧 seedHash 算法，因此判据必须比 kernel 指纹，不能只跑
`fix_data_uuid.py --verify`（那个只校验 UUID 字面一致，此处会给出假的绿灯）。

---

## 2. 故障链（推断部分已标注）

```
下午：boot 分区内核 K0 + Data 卷身份 I0（K0 建的）→ 配对 → 每次重启都进桌面 ✅
       ↓  中间反复 fastboot boot 了 K1(ca349c0c, 12:50:51) / K2(4c199744, 15:08:18)
       ↓  M3、D1 走到 stage=4，按新内核算法重建 Data 卷 → 身份变成 I1/I2
现在：重启 → 回到 K0 → K0 认不出 I1/I2 → error=2 → panic → 进不了桌面 ❌
```

- **已证实**：中间确实换过内核、且至少有 2 次成功接管 Data 卷（证据 5、7）。
- **推断（未坐实）**：当前这次"重启进不去"就是 `error=2` panic。因为证据 9
  显示**这次失败根本没抓串口日志**，也可能只是 M4/M5 的 seed 握手挂死、
  或 D1 的 GPU 卡死。必须用 §5 的判据脚本对新日志分类后才能定性。

---

## 3. 判据脚本（已实跑，退出码可复现）

### 3.1 `scripts/check_boot_session.py` —— 串口日志会话分类

按 `Darwin Kernel Version` 横幅切会话，逐会话判定：
`CLEAN` / `DATA_IDENTITY_PANIC` / `SEED_STALL` / `GPU_STALL` / `OTHER_PANIC` / `UNKNOWN`。
**以「有没有 `PICASSO_DPU_FRAME`」为准**，因为证据 10 证明 `fence=19`、
`hangcheck` 单独出现会误报。

```bash
cd D:/k30i-gpu/repo-work/scripts
python3 check_boot_session.py D:/k30i-gpu/next/run-M3-retry.log \
    D:/k30i-gpu/next/run-M4-retry2.log D:/k30i-gpu/next/run-M5-retry3.log \
    D:/k30i-gpu/next/run-D1-warm.log D:/k30i-gpu/next/run-M2-trust5.log \
    D:/k30i-gpu/next/run-R7-685.log
```

实跑结果（2026-11-13，全量 6 份，未挑窗口）：

| 日志 | 判定 | rc |
|---|---|---|
| `run-M3-retry.log` | CLEAN（385 帧） | 0 |
| `run-M4-retry2.log` | SEED_STALL | 3 |
| `run-M5-retry3.log` | SEED_STALL | 3 |
| `run-D1-warm.log` | GPU_STALL | 4 |
| `run-M2-trust5.log` | DATA_IDENTITY_PANIC（`[2]` 段是旧内核 CLEAN，130 帧） | 2 |
| `run-R7-685.log` | DATA_IDENTITY_PANIC | 2 |
| **全量合计** | `OVERALL_VERDICT=DATA_IDENTITY_PANIC` | **2** |

> 写脚本时犯过一次错并已当场修掉：最初把退出码当严重度用 `>` 取最大，
> 导致 `UNKNOWN(6)` 盖过一切、`GPU_STALL(4)` 盖过 `DATA_IDENTITY_PANIC(2)`，
> 全量跑出 rc=4（错）。现拆成 `RC` 与 `SEVERITY` 两张表，重跑得 rc=2（正确）。
> 保留这段，是因为门槛脚本的退出码一旦错就会直接误导判断。

### 3.2 `scripts/preboot_data_identity.py` —— fastboot boot 之前的身份门禁

对比「待引导镜像」与「内部已刷稳定 boot」的 kernel banner / kernel sha /
seed sha / UUID 四项指纹。主机侧读不到设备当前 Data 卷身份，所以输出的是
**风险判定**，不是设备现状诊断。

```bash
python3 preboot_data_identity.py <candidate.img> --baseline <internal-boot.img>
```

实跑结果（含单变量对照，一次只改一个变量）：

| 对照 | kernel 同 | seed 同 | UUID 同 | rc |
|---|---|---|---|---|
| `4c199744` vs `680537f5`（今晚真实组合） | ✗ | ✓ | ✓ | **3** |
| `680537f5` vs `680537f5`（自比，阴性对照） | ✓ | ✓ | ✓ | **0** |

`rc=3` 说明：**即便 seed 与 UUID 都一致，内核指纹不同就足以触发 Data 身份重建**。
这正是今天踩的坑，也是为什么 `fix_data_uuid.py --verify` 在这里会给假绿灯。

---

## 4. 恢复流程（SOP）

⚠️ **下列偏移数字未经核实，禁止照抄执行**，见 §4.1。

1. 长按电源 12–15 s 黑屏，按住「音量下 + 电源」进 FASTBOOT。
2. 用 TWRP **引导**（`fastboot boot twrp.img`，不刷 recovery）。
3. 抹存储头 1 MiB + 初始化日志 1 MiB（两个偏移见 §4.1，**先核实再用**）。
4. `fastboot reboot` → 回到 boot 分区内核。
5. 等内核自己重建 Data，串口应依次出现验收四件套：

```
PICASSO_DATA_INIT phase=1 ... complete=1
PICASSO_DATASTORE overlay open=1 header=1 present=0
NATIVE_OWNER_DATA done stage=4 error=0 path=/dev/disk5s1
PICASSO_EXEC ok pid=... SpringBoard
```

6. 用 `check_boot_session.py` 判这次恢复日志：必须 `CLEAN`（rc=0）+ 有帧 +
   `panic=0`。**只看到 `stage=4` 不算恢复成功**（D1 就有 stage=4 但 0 帧）。
7. 再重启一次确认不 panic —— 因为 L3095 说过，桌面可能是内存镜像，撑到重启为止。

**代价**：该卷里的用户数据会没（§4.8 原文）。

### 4.1 偏移数字的矛盾（必须先解决）

§9 写「相对 userdata 偏移 `22555482 × 4096`」和「`27986949 × 4096`」。但：

- `22555482 × 4096` = **86.04 GiB**
- `27986949 × 4096` = **106.76 GiB**
- 而 `docs/persistent-data-20g.md` 的分区表：userdata 末 LBA `15330298 × 4096` = **58.48 GiB**

两个偏移都**超出 userdata 分区范围**。可能的解释：是全盘绝对 LBA，或来自
另一版分区布局（build164 LUN 0），或文档单位/基准写错。**在未用
`sgdisk -p` / UFS LUN 表核对之前，照抄去 dd 会写到分区外，属于高风险操作。**
请 WorkBuddy 先给出这两个数字的口径来源，再执行第 3 步。

---

## 5. 根因修复建议（给 WorkBuddy，按优先级）

1. **把 §9 的护栏做成开机前强制门禁**：任何 `fastboot boot` 之前先跑
   `preboot_data_identity.py`，`rc=3` 就拒绝引导（或强制要求显式确认
   "已准备好恢复流程"）。今天的问题不是缺文档，是**护栏没进脚本**。
2. **不要再用「没刷机」当作设备未变的论据**。`fastboot boot` 会重建 Data 卷，
   这句话只能说明 boot 分区没动。
3. **抓日志**：现在这次失败没有串口日志。下次开机前先起 capture，
   把日志喂给 `check_boot_session.py` 分类，再谈定性。
4. **固定单一内核做日常开机**：日常只用 boot 分区那一颗内核；要测新内核就
   接受"测完必须恢复 Data 身份"的代价，并把恢复流程写进 `flash_verify.py`
   的收尾步骤。
5. **清理历史误导**：`fix_data_uuid.py --verify` 的绿灯只覆盖 UUID 字面一致，
   应在输出里明确写"不校验内核 seedHash 算法版本"，避免再被当作身份安全证明。
6. **根因层**：XNU 侧应让 Data 卷身份与内核版本解耦（或至少把身份标记写成
   双向可读），否则"换内核必重建"会一直复发。这是长期项，但值得记一笔。

---

## 6. 待核实清单（不许写成结论）

- [ ] 本次"重启进不了桌面"的串口日志 → 判定是 `DATA_IDENTITY_PANIC`、
      `SEED_STALL` 还是 `GPU_STALL`。
- [ ] 两个抹头偏移的口径（全盘 LBA / 分区内 LBA / 哪版布局）。
- [ ] 当前 boot 分区里到底是哪颗内核（`680537f5` 还是别的）。
- [ ] "新内核 seedHash 算法不同"的具体代码位置（目前只有报告文字与现象，
      未定位到源文件行）。
- [ ] 20:46 那次恢复的**原始日志**在磁盘上没找到（全盘 grep
      `PICASSO_DATA_INIT phase=1` 无命中），目前只有 L3108 的转述。
