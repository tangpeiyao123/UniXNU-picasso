# 卡住无法进桌面：phase=0 根因链、误判复盘与三项合一修复（2026-11-22）

> 一句话结论：journal identity 里的 `seedHash` 只认成功镜像 `4a6a853d` 内嵌的那颗 `SCRATC~1.IMG`（sha `9220bb17…`）；同时 `PicassoPersistence.h` 有 4 个写死的 GPT 几何常量与本机实测不符。二者叠加，让任何"新内核 + 新 seed"组合都停在 `phase=0` / `refused publication`，进不了桌面。修好这两处后，再叠加三项功能修复（音频 ADSP profile、GPU 直出参数、蓝牙 PR#25），一次 `fastboot boot` 真机验证全部通过。

这份档案的重点不在"最后做对了什么"，而在**中间走过的弯路**——那些误判几乎每一个都足够让人反复刷机一整天。请后来者（人 or AI）先读第 3 节误判清单，再动手。

---

## 0. 适用范围与红线

- 设备：Redmi K30i（picasso / SM7250），设备 UUID `0EBB496E-FB09-4223-991E-22007B6FACD1`。
- 仓库：`tangpeiyao123/UniXNU-picasso` 分支 `upstream-latest-20261118`（`YinCQL/UniXNU-picasso` 的 fork）。
- **userdata 分区红线**：root image 在相对偏移 `REL 0..2045951`，Data overlay 在 `REL 6941530+`。seed/setup 类镜像**绝不能** `flash userdata` 到偏移 0——会覆盖 root，直接变砖起不来。曾经踩过，靠重刷 `userdata-root.sparse.img`（11/11 chunk，RC=0）救回。
- 所有验证阶段的镜像都只用 `fastboot boot`（只写 RAM，重启即回滚）；固化 `flash boot` 是最后一件事，且必须在临时引导验证通过、独立评审之后。

---

## 1. 现象与时间线

| 阶段 | 现象 | 当时的判断 | 后来的定论 |
| --- | --- | --- | --- |
| 起点 | 自构 boot 曾稳定进桌面；"修电量/电池位置"后随机引导失败 | 怀疑电池改动引入随机故障 | 时间相关，非因果；两仓库文档均无此因果记录 |
| 中段 | 作者原版 boot（x190）也偶发 root panic、~2 分钟 crash | 以为是自构 boot 独有 | 作者镜像同样有，属另一类 crash-recovery / SEED stall |
| 卡死点 | 新内核一律进不了桌面：`PICASSO_DATA_INIT phase=0`、`refused publication state=4` | 反复猜 UUID / seed / 守卫 | 根因是 journal identity 的 seedHash 不匹配 + 几何守卫写死 |
| 突破 | 用成功镜像内嵌 seed `9220bb17` + 只改内核 UUID | 理论推演 | 真机 `phase=2/complete=1` 进桌面，连续两次成功 |
| 收官 | 三项合一（音频/GPU/蓝牙）镜像临时引导 | 担心新改动破坏已验证的 identity | 三项全生效、0 panic，用户实机确认声音/流畅度/蓝牙均正常 |

关键日志证据（本次收官验证）：`D:/k30i-gpu/capture-threeinone/console-20261122-092228.log`（串口，约 400 KB / 4500+ 行）。

---

## 2. 关键机制（源码级，务必先理解再改）

### 2.1 `seedHash` 从哪里来 —— 这是整个谜题的题眼

链路（已在源码逐行核对）：

1. `NativeRamBlock.cpp:197-200` 读 `/chosen/memory-map` 的 `PackedRAMDisk / RAMDisk`（16 B physical + bytes）。
2. 该节点由 `make-picasso-adt.py:229` 的 `packed_data_ramdisk` 声明，指向 payload FAT 里的 `EFI/UNIXNU/PAYLOADS/SCRATC~1.IMG`。
3. `payload.json` 的 `data_ramdisk_sha256` = sha256(setup.dbr)（`check-picasso-setup-identity.py:84`）。
4. `NativeRamBlock.cpp:216-232` `seedImportSparse`，`:236-246` `seedIdentity(SHNGSEED2 + size + 非零页 idx + 内容)` 得到 32 B seedHash，存全局。
5. `PicassoPersistence.h:91` `memcpy(identity+56, seed, 32)`。

**结论：`seedHash` 完全由 payload 内 `SCRATC~1.IMG` 决定，随镜像变，不随设备变，也不是什么 boot-arg。**

### 2.2 journal identity 的 88 字节布局

`PicassoDataInit.h` + `PicassoPersistence.h` 生成的 88 B identity：

| 偏移 | 内容 |
| --- | --- |
| 0 | `gStoreGeometry.first` |
| 8 | `gStoreGeometry.last` |
| 16 | `gStoreGeometry.imageBytes` |
| 24 | `gStoreRegionFirst` |
| 32 | `gStoreRegionBlocks` |
| 40 | `headerCrc` |
| 44 | `entriesCrc` |
| 48 | `pages` |
| **56** | **`seedHash[32]`** ← 最容易搞错的地方 |
| 88 | phase（1 pending / 2 complete，在 92 B payload 里） |

`Journal::begin()` 在 `memcmp(saved, identity, 88) != 0` 时直接 return 0 → 这就是 `phase=0` 的直接来源（`PicassoDataInit.h:24`）。**journal 认的不是"设备"，而是当初写 journal 那颗镜像的完整 identity。**

### 2.3 持久化守卫：4 个写死常量是坑

`f10634d`（fix-persistence-geometry）删除了 `PicassoPersistence.h` 里 4 个写死比对：

- `gStoreRegionFirst != 9825114`
- `gStoreGeometry.last != 15330298`
- `headerCrc != 0x3c17a352`
- `entriesCrc != 0xd82988c2`

理由：这些是另一块板的 build-time 值，本机实测 GPT 不同，一旦写死就 `refused publication`。删掉后由**运行时几何**填入 identity，仍由 journal 的 `memcmp` 做完整性绑定——结构守卫（`pages`、`regionBlocks`、`first`、`imageBytes`、`reserveBlocks`、GPT check）和 identity 的 9 行生成全部保留，一个字节没动。

对应的 host 回归测试后来也暴露了同类问题：`scripts/tests/picasso-persistence-host.cpp:133` 还把 `gStoreRegionFirst=9825114` 当编译期硬门，断言"改了就拒绝初始化"。这与运行时几何语义冲突。修法（提交 `9a20d64`）：空白窗口允许设备实测几何初始化；已完成的 journal 再拒绝另一个 `regionFirst`。这只改测试，不动运行代码。

---

## 3. 误判清单（本档案最值钱的部分）

| # | 当时的判断 | 为什么错 | 正确做法 |
| --- | --- | --- | --- |
| 1 | `0EBB496E…` 是无关常量 | 用户纠正：这就是本机设备 UUID | 工具/内核/seed 三处 UUID 都要对齐到设备值 |
| 2 | 刷 `seed47`(`906347e0`) 到 userdata 能修 identity | 完全不是 journal 认的种子，且刷 userdata 偏移 0 覆盖了 root | 立刻重刷 `userdata-root.sparse.img`；**seed 类镜像永不写 userdata** |
| 3 | 换 `SCRATC~1.IMG` = 换 seedHash | 中途被"seedHash 来自 data.dbrdisk"的说法带偏，一度以为换错对象 | 最终读源码确认：就是 `SCRATC~1.IMG`，`data.dbrdisk` 并不在 boot.img 里 |
| 4 | `seed190`(`0033835a`) 是 journal 配套 seed | journal 里固化的是成功镜像的 seedHash，二者无关 | 只认 `9220bb17…` |
| 5 | 把 seed 里 7 处 UUID 改成设备值 | 成功 seed 里**本来就是** `0EBB496E`×7；改了反而破坏 seedHash | 种子一个字节都别动；只改内核那 1 处 ASCII UUID |
| 6 | `header=-1` 说明 Overlay 层坏了 | `header` 是下游结果；`phase=0` 时 `overlay.open()` 根本没调用，`header` 保持初值 -1 | 由上往下查 journal phase，不要拿下游值反推上游 |
| 7 | UUID 对齐后 root panic 就消失了 | 长窗口证明仍会 panic | 把根因转向 seed/Data identity，别停在表面相关 |
| 8 | 音频卡住是 TZ/SCM 结构性限制 | GPU ZAP `pas init/auth a0=0, loaded=1` 证明 TZ/PAS 可用 | 真正卡点是 `adsp.b00` 哈希不匹配（`error=-8`） |
| 9 | 作者 boot 不会有 root panic | 实测作者 x190 同样出现 | 不要假设原版有魔力，实测为准 |
| 10 | GPU 参数在 `board.toml` 里改 | `board.toml` 根本没有 scanout/glwait；真实来源是 `scripts/picasso_media_payload.py:28-29` 硬编码 | 评审官指出后才改对位置：`scanout=2`、`glwait=0` |
| 11 | host regression 红 = 产物有问题 | 失败在 `check-picasso-persistence.py` storage 子检查，与 boot.img 构建路径无依赖；compose/kernel 双绿才是产物判据 | 分开归因：CI 产物 vs host 逻辑回归 |

**贯穿性教训**：每次"看起来合理"的推断，都要回到源码行号和实际哈希去验证；宁可多读一次 `SeedDisk.h` / `NativeRamBlock.cpp`，也不要再刷一次机。独立评审机制（每个刷机/改镜像动作前先过一遍证据链）就是为拦住这类误判而设的。

---

## 4. 修复链（一步一步，可复现）

### 4.1 成功 seed 的提取

从 Nov11 成功镜像 `4a6a853d`（`D:/k30i-gpu/next/boot-685-pscireset+trustcompat2h-4a6a853d.img`）里直接提取它自己的 payload 种子：

```bash
python tools-bringup/patch_payload_file.py <成功镜像> \
  --read EFI/UNIXNU/PAYLOADS/SCRATC~1.IMG -o success-seed-9220bb17.dbr
```

结果：`1,343,728 B`，sha256 `9220bb1714245529599a62f13c8813ff36a0917dc31c2d7d0b6da175cc790952`，内含 `0EBB496E`×7（**不需改 UUID**）。

按 `SeedDisk.h:72-95` 复刻 seedHash：成功 seed → `9d6fba1c`；seed190 → `175bc39f`；seed47 → `3020e817`。三者互不相同，journal 只认前者。

### 4.2 固化镜像构造（本地，不重编、不公开分发 seed）

```bash
# 1) 把成功 seed 原样写回新内核镜像的 payload（0 字节改动）
python tools-bringup/patch_payload_file.py <新内核 boot.img> \
  --replace EFI/UNIXNU/PAYLOADS/SCRATC~1.IMG success-seed-9220bb17.dbr -o <中间镜像>

# 2) 只把内核里 1 处 ASCII UUID 改到设备值（种子 0 处改动）
python tools-bringup/fix_data_uuid.py <中间镜像> <最终镜像> \
  --to 0EBB496E-FB09-4223-991E-22007B6FACD1

# 3) 三道本地门禁
python tools-bringup/patch_payload_file.py <最终镜像> --read EFI/UNIXNU/PAYLOADS/SCRATC~1.IMG   # sha 仍 9220bb17
python tools-bringup/fix_data_uuid.py <最终镜像> --verify                                        # 内核/种子两侧一致
python tools-bringup/preboot_check.py <最终镜像>                                                 # 四项全过
```

### 4.3 结果

- `boot-author-final-uuid.img`，sha `ea4b934b1fb4190e85049a1c9e4ac5953fe07b9dadfff5950f3ad464bef5b486`：新内核（守卫修复 + 作者音频）+ 正确 seed + UUID 对齐 → 真机连续两次 `phase=2/complete=1` 进桌面，0 panic。这是"能稳定进桌面"的基线。

---

## 5. 三项合一：在基线上叠加功能修复

一次 CI 同时解决三个长期问题（分支 `upstream-latest-20261118`，commit `be2744d` → merge PR#25 `a8916c9` → `739bc23`）：

| 项 | 根因 | 修复 | 验证 |
| --- | --- | --- | --- |
| 音频 | 内核 profile 期望 `adsp.b00` sha `08aa69bf…`，设备 `modem.img` 实际 `94241d41…`，`memcmp` 失败返回 `-8` | 用设备 `D:/k30i-firmware/modem.img`（335,544,320 B，sha `1a64b027…`）重生成 `PicassoAdspFirmwareProfile.h`，34 条、恰好 12 条更新；modem.img 未上传 repo | 真机 `firmware error=0 / files=34 / validated=1 / pas=1`，`-8` 消失 |
| GPU 卡 | `scripts/picasso_media_payload.py:28-29` 硬编码 `sheng-dpu-scanout=0` + `glwait=1`，走软拷贝路径 | 改为 `scanout=2`（SDE 直出经 display SMMU）+ `glwait=0`（不等 GPU） | 真机 `PICASSO_DPU direct scanout on bank=2`、`PICASSO_ACCEL published=0` |
| 蓝牙 | 构建分支未合入上游 PR#25 的 WCN399x/Skywalk 运行时 | fetch upstream + `merge a8916c9`；冲突解=保留我们的 ADSP 硬锚 + GPU_ARGS，蓝牙收上游 | 真机 `PICASSO_BT` 首次 -60 后恢复、QCA 固件下载、`power=1 result=0 baud=3000000` |

### 5.1 合并撞车的一个关键判断

PR#25 把 ADSP source 常量改成 `picasso_firmware.manifest('adsp')` 驱动，但它的 `firmware/adsp/manifest.json` 还 pin 着旧 NON-HLOS（`178958336 / a77ac6ad`），且 `picasso_firmware.py:40` 有 `<= 256MiB` 上限（设备 modem.img 是 320MiB），`:34` 还强制 `stock_version == V13.0.5.0.SGICNXM`。

→ 适配 manifest 要同时改大小上限**和**版本锚定两处上游校验；保留我们的硬编码常量则零上游改动。评审确认：硬锚是设备实盘固件，本来就是更正确的锚。

### 5.2 CI 架构变化

PR#25 后 `build-kernel-linux.yml` 必填 `compose_run_id`：kernel 消费**同 revision** 的 compose 产物。这是正向约束——杜绝"内核与 payload 不同源"的归因混乱。

- Compose run `44966131439`（head `739bc23`）success
- Kernel run `44968524804`（head `739bc23`，`compose_run_id=44966131439`）success
- 产物 `picasso-linux-bootimg-6`，boot.img sha `bc1c5e09aa4e6b090268575bdba3e4a01793b6076975cb1be4303a8fca0ff827`

### 5.3 最终三项合一镜像

`D:/k30i-backup/boot-threeinone-final-uuid.img`，sha256 `2b60313ff41dda4fc184ea2e26f595f345ff10003912a49c0c95f7264b2c6884`。

- payload 内 `SCRATC~1.IMG` 与 `9220bb17` **逐字节相同**（偏移 2992128，1,343,728 B）
- 内核/种子 UUID 双侧 `0EBB496E`，三个旧值 `72443F51/F1743A19/219442C4` 计数全 0
- bootargs 实测含 `sheng-dpu-scanout=2`、`glwait=0`
- 内核内含 `adsp.b00` 实测 digest `94241d41…`（kernel.macho 相对偏移 4651356 / 镜像绝对偏移 9003356）

---

## 6. 真机验证结果（临时引导，未 flash）

设备 `bed01658`，fastboot `getvar product` 两次 RC=0；`fastboot boot` RC=0。串口日志 `docs/evidence/` 同类证据可参考本次 `D:/k30i-gpu/capture-threeinone/console-20261122-092228.log`。

### 6.1 桌面主判据 —— 全过

- KERNEL BANNER 一次（L26），全程无重启；严格 panic 搜索 0 命中
- 3 处 "panic" 字样均为噪音：L143 PSHOLD `panic-reset=1` 旧标记、L2158 `DumpPanic` 守护启动、L2509 `PICASSO_AKS name=DumpPanic`
- L1072 `PICASSO_DATA_INIT phase=2 ... marker=30870533`；L1073 `complete=1`
- L1233 `NATIVE_OWNER_DATA done stage=4 error=0 path=/dev/disk5s1`
- L1238 `PICASSO_EXEC ok pid=1 /sbin/launchd`；L2058 `pid=26 SpringBoard`；L2858 `PICASSO_SBINJECT metrics=installed pid=26`
- L666 `PICASSO_DPU direct scanout on bank=2`；L669 `PICASSO_ACCEL published=0`；L1769 bootargs 含 `scanout=2` / `glwait=0`
- 内核戳 `Darwin Kernel Version 23.6.0: Sun Nov 22 00:54:00 UTC 2026`

### 6.2 音频专项 —— 内核/守护层全就绪（用户实机也确认有声）

- L1046 `PICASSO_ADSP firmware source=modem ... error=0 ... files=34`；L1048 `validated=1 pas=1`
- L1792 `observed ready=1 fatal=0 handover=1 glink_error=0`
- L1296/L1321 `glink channel=apr_audio_svc ... up=1`；L1525 `apr_ready=1 apr_error=0`
- L1483 `qmi domain=.../audio_pd ... result=0`；fastrpc `up=1`
- 全文 `error=-8` / `validated=0` 0 命中

注意：早期 L724/L1262 曾 `ready=0 / glink_error=-2`，最终态才转好——以最终态为准，但不能说"全程无 glink 错误"。日志本身没有听感证据，听感来自用户实机确认。

### 6.3 蓝牙专项

首次 `version-reply sample=1 error=-60`（已知握手超时）→ sample=2 `error=0` → QCA FW `BTFM.CHE.3.2.1-00174-QCACHROMZ-1.372365.1` 下载完成 → `power=1 result=0 baud=3000000` → UART 通道建立。

用户实机确认：**声音、流畅度、蓝牙三项都正常**。

---

## 7. 可复用规则（写给未来的操作和评审）

1. **先读源码再动手**：identity/seedHash 这类问题，源码行号比任何猜测都可靠。`SeedDisk.h`、`NativeRamBlock.cpp:197-248`、`PicassoPersistence.h`、`PicassoDataInit.h` 是必读四件套。
2. **journal 认的是镜像，不是设备**：换新内核/新 payload 后能否 resume，取决于 payload 内 seed 算出的 seedHash 是否与 journal 里固化的一致。要 resume 旧 Data，就把旧成功镜像的 `SCRATC~1.IMG` 原样塞回。
3. **种子字节是圣物**：不要"顺手"改 seed 里的任何字节（包括 UUID）。UUID 只改内核那 1 处 ASCII。
4. **seed 类镜像永不写 userdata**：`flash userdata` 偏移 0 = 覆盖 root = 变砖。
5. **临时验证先于固化**：`fastboot boot` 可逆；`flash boot` 不可逆。任何改动先临时引导，跑够长窗口（音频/蓝牙需要时间才就绪），再评审固化。
6. **CI 归因分离**：compose/kernel 绿=产物 OK；host regression 红=逻辑回归，两者不要混为一谈。
7. **每个动作过评审**：改镜像、刷机、下判断前，把证据链（sha、日志行、源码行号）发给独立评审官核一遍。这次至少拦住了"改 board.toml"和"改 manifest"两个错误方向。
8. **区分内核层与用户层**：日志能证明 `error=-8` 消失、ADSP ready、glink up；不能证明"用户听到声音"。后者必须实机确认，不能写进日志结论里。

---

## 8. 关键数据速查

| 项 | 值 |
| --- | --- |
| 设备 UUID | `0EBB496E-FB09-4223-991E-22007B6FACD1` |
| 正确 journal seed sha | `9220bb1714245529599a62f13c8813ff36a0917dc31c2d7d0b6da175cc790952` |
| 成功 seed 来源镜像 | `boot-685-pscireset+trustcompat2h-4a6a853d.img` |
| 稳定基线镜像 sha | `ea4b934b1fb4190e85049a1c9e4ac5953fe07b9dadfff5950f3ad464bef5b486` |
| 三项合一镜像 sha | `2b60313ff41dda4fc184ea2e26f595f345ff10003912a49c0c95f7264b2c6884` |
| 设备 modem.img | 335,544,320 B，sha `1a64b0271772811e4e83d5527c5ac5b626c843ebad070f17272b399299161410` |
| 设备 `adsp.b00` sha | `94241d41a3a0b24c7a929cc28b3117c5441003304e8b66f63d142b4522c95e55` |
| 旧 profile 期望 sha | `08aa69bf…`（不匹配 → `error=-8`） |
| 分支 / 最终 revision | `upstream-latest-20261118` / `739bc236d1e018b9a186306ccbf496feeb7da04c` |
| Compose / Kernel run | `44966131439` / `44968524804`（均 success） |
| 最终验证串口日志 | `D:/k30i-gpu/capture-threeinone/console-20261122-092228.log` |

补充提交：`be2744d`（GPU+ADSP profile）、`739bc23`（merge PR#25）、`5edbfcd`（诊断期上传路径）、`9a20d64`（host 持久化测试对齐运行时几何，仅测试/workflow，不动运行代码）。后两个是诊断/回归修正，不改变 boot 产物。

> 状态注记：截至 2026-11-22 09:30，三项合一镜像仍只是 `fastboot boot` 临时验证通过，**尚未 flash 固化**。固化需在独立评审后执行 `fastboot flash boot <镜像>`（只写 boot，不碰 userdata），并以 RC=0 + host sha 回读 + reboot 后重新进桌面三判据验收。
