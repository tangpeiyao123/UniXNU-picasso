# Redmi K30i 5G 跑 iOS 16：GPU 直出打通，以及四个踩坑记录

> 设备：Redmi K30i 5G（`picasso` / SM7250 / 6 GB / 解锁 bootloader）
> 系统：iOS 16.7.16（20H392）userdata，按上游 `feat/ios16-gpu` 的配方构建
> 本文只记录**有日志支撑**的事实；没跑通的部分会明确写"未定"。
> 上游项目：[`YinCQL/UniXNU-picasso`](https://github.com/YinCQL/UniXNU-picasso)

---

## 结果先说

| 项目 | 状态 |
|---|---|
| 内核启动 + 数据卷挂载 | ⚠️ 多数能进桌面；**间歇失败**：`NATIVE_OWNER_DATA stage=0 error=2` → `panic: offline scratch Data mount failed: 2` → 重启回 boot 分区的旧镜像（19 份会话里 2 次）。`data-seed-retry` 补丁（40×50 ms 重试）已提交，待真机验证 |
| GPU 渲染（CoreAnimation 走 Adreno） | ✅ `plugin type=4147`，GL 后端加载并编译着色器 |
| 零拷贝直出 | ✅ `PICASSO_DISPLAY copy` = 0，`fallbacks=0`，`latch_timeouts=0` |
| 帧节奏 | ⚠️ `PICASSO_DPU_FRAME period_us` 16.3–17.6 ms（57–61 Hz）是**面板扫描节拍**，不是 UI 帧率；UI 帧率**尚未用可靠口径测出**，滑动体感约 40 多 fps（见第二节） |
| 电池 / 充电状态 | ✅ 插拔约 1 秒响应（`chg_status=3`、`powered=1`），电量按 VADC 曲线更新 |
| 状态栏注入（电池避让挖孔） | ⚠️ **曾经成功**（merge5 镜像，`battery shift 14.5 mm`，肉眼确认）；当前批次失败，根因是 trust 表里放的 cdhash 与设备 Data 卷里实际那份库不是同一次构建，已补 `758a8864` 待验证 |
| 键盘右侧被裁 ~45 px | ❌ 未解决（已排除两个假设） |
| 音频 / WiFi | ⏸ 卡在 SCM/PIL，探针已写好，结果未出 |
| 调试参数 | ✅ 已从源码剥离到 `params/picasso.args`，改一个值本地重打镜像即可，不跑 CI（上限 1000 字节，已用 997） |

> 上表更新于 2026-11-12 21:00 CST。此前写着"帧率 57–61 fps"和"状态栏注入双重确认"的两行已按证据更正。

## 最近一周的进展（2026-11-12）

这几条都是**上机验证过**或**有明确反面证据**的，不是计划：

**1. 电池与充电状态（已解决）**
`PICASSO_BATT state ok=1 mv=4001 pct=62 usb=4841mV powered=1 charging=1 chg_status=3`。
读数来自 PM7250B 的 SMB5 `BATTERY_CHARGER_STATUS_1`（`CHGR+0x06`），插拔约 1 秒内响应；
百分比曲线按 VADC 重标定（3961 mV 从 72 % 调到 51 %，与屏幕对得上）。

**2. 状态栏避让挖孔：失败原因不是"位置常量"，是信任表版本错配（已定位，待验证）**
`libpicasso-sb.dylib` 被 AMFI 拒：

```
AMFI: '/usr/lib/libpicasso-sb.dylib': unsuitable CT policy 0 for this platform/device, rejecting signature.
```

原因是设备 Data 卷里那份库是 **2026-11-09 15:48** 构建的（cdhash `758a8864…`），
而 trust 表里放的是**另一次构建**的 cdhash。库没被加载 → 状态栏钩子没装 → 图标回到默认位置。
修法是把"设备上实际在用的 cdhash"作为**追加模块**写进 trust bundle（不是替换新库）。
`merge5` 那份镜像能成功，正是因为它的 trust 表同时列了新旧两个哈希。

**3. 启动 panic：`stage=0 error=2` 是 seed 查找"一次失败就放弃"**
```
NATIVE_OWNER_DATA done stage=0 error=2
panic: offline scratch Data mount failed: 2 @kern_exec.c:9908
```
`native_ram_block_volume_seed()` 只调用一次，UFS LU 没就绪就直接走 panic 路径。
已改成 **40 次 × 50 ms 重试**（`delay()` 单位是微秒），耗尽后仍走原来的 ENOENT 分支。
这个 bug 还有个副作用值得记：panic 后设备会从 boot 分区重启，**那一轮日志跑的是旧镜像**，
如果判据不按内核横幅切分会话，就会把旧镜像的结果算到新镜像头上。

**4. 调试参数外置（boot-args 已到天花板）**
所有可调参数现在集中在一个文本文件里，改一个值本地重打镜像即可，不跑 CI：

```
python apply_params.py --show boot.img
python apply_params.py params/picasso.args boot.img
```

参数不变时重打，产物与源镜像**逐字节相同**（幂等）。但 `DarwinBoot` 命令行上限 1000 字节，
现在已经用了 **997**——这是为什么"再加 WiFi/音频参数"必须走外置读取（misc 分区或 UEFI），
方案写在 `proposals/params-file-on-device.md`。

**5. 判据本身的修正（这一条比上面任何一条都重要）**
过去几轮出现过三次同类错误：用主机墙钟当分母、用采样打印的帧号当分子、
把全文件累计的"帧行"当成当前会话的。现在的规则是：
- 帧/启动类判定只看**第一个 Darwin 横幅之后**的会话；
- panic 是硬失败，不需要跑满窗口；
- 窗口不足时输出"未验证"，而不是"未通过"或"通过"。


---

## 这不是双系统，也不是「一次性实验环境」

先说清楚方案性质，否则后面所有结论都会被误读。**这里要纠正一个很容易想当然的预期**：

- **不是双系统**：没有开机菜单，没有「Android / iOS 二选一」。boot 分区写入 iOS 引导镜像后，
  开机直接进 iOS，Android 不再是可启动的系统。
- **不是重启就回原系统**：数据是**持久化**的，重启后 iOS 里的东西还在。

数据持久化的证据在启动参数和计数器里：

```
sheng-data-persist=2   picasso-data-init-blank=1
picasso-data-replace-20g=1   picasso-persist-records=1
```

```
DATA st= hdr= gpt=xx/xx lba=...   pres= ld= wr= sy= er=
```

后半行是持久化层的统计：`pres` 存在标志、`ld` 已加载页、`wr` 写入次数、`sy` 同步次数、`er` 错误数；
`gpt` / `lba` 说明它按 GPT 定位到一个真实分区，`replace-20g` 说明替换的是 20 GiB 那块。

所以真实形态是「**把原系统替换掉的单系统安装**」：

| | |
|---|---|
| 开机 | 直接进 iOS |
| iOS 数据 | 持久化在分区里，重启后还在 |
| Android | 不再是可启动系统 |
| 回退 | 必须刷回原厂 boot 镜像，必要时还要处理被替换的数据分区 |
| `fastboot boot` | 只是**测试新镜像时**的手段（它确实不写分区），不是设备当前状态 |

`fastboot boot` 不写分区这件事仍然成立，但它的含义是「这一次启动用内存里的镜像」，
**不会把设备恢复成 Android**。本仓库所有实验都用 `boot` 而不是 `flash`，是为了试错时不动分区，
不是因为设备还能回原系统。

前置条件：**bootloader 已解锁**（必须；解锁会清空设备数据，先备份）；**不需要 root，也不需要 TWRP**。
完整清单见 [`docs/OTHER-DEVICES.md`](docs/OTHER-DEVICES.md)。

---

## 一、从 20 Hz 到 60 Hz 节拍：差别只有一个启动参数

现象很好骗人：桌面正常、毛玻璃正常、还能滑动，只是"卡"。三条计数器才说明它在跑软件合成：

| 指标 | 软件合成 | `sheng-gpu-metal=1` |
|---|---|---|
| `PICASSO_ACCEL` | `published=1`（CPU 桩还在） | `published=0` |
| `PICASSO_GPU open` | 从不出现 | `plugin type=4147 pid=50 name=backboardd` |
| `IOUC ... failed MACF ... backboardd` | **约 226 行/秒**，占控制台 85% | 0 |
| `PICASSO_GPU submits` | 整场 `submissions=1` | 数千 |
| `PICASSO_DPU_FRAME period_us` | ~50000（20 Hz 节拍） | 16300–17600（57–61 Hz 节拍，**不是帧率**） |
| `PICASSO_DISPLAY copy` | 有（每帧 10.4 MB） | **0** |

`PicassoDisplay.cpp` 里的逻辑是：默认仍然发布 CPU 加速器桩（`PicassoCpuAccelerator`，故意不提供
`MetalPluginName`），只有 `sheng-gpu-metal=1` 时才撤掉它。桩在 → CoreAnimation 停在软件路径 →
毛玻璃由 CPU 算，所以"看起来是好的，只是慢"。

**这个失败是静默的**：除了那 226 行/秒的 MACF 拒绝，没有任何显式报错。
所以判断有没有真上 GPU，第一步应该看 `PICASSO_ACCEL published=0`，而不是看画面。

---

## 二、帧率怎么量：两个指标都不能直接用（初版错误已更正）

**更正说明（2026-11-12）**：这一节初版把 `PICASSO_DPU_FRAME period_us` 当成"每帧真实间隔"，
据此宣布"稳态 57–61 fps"。**这个结论撤回。**

原因是 DPU 的 `period_us` 是**扫描输出节拍**：只要 panel 以 60 Hz 扫描，内容有没有更新都是
约 16.6 ms 一次。桌面静置时它当然稳定在 16.3–17.6 ms，但那只能证明"面板在按 60 Hz 刷"，
不能证明"UI 每秒画了 57–61 张新画面"。实际滑动时体感只有 40 多 fps，两者并不矛盾。

同样，`eagl f<N>` 的帧号差除以抓取秒数也不可靠：那批日志是限频打印的，分母（抓取秒数）
和分子（打印到的帧号）不是同一段时间。所以早先那个 43.6 fps 也一并作废。

**现在能确定的**：
- `PICASSO_DPU_FRAME period_us` = 面板扫描节拍，60 Hz 面板上恒 ~16.6 ms，**不能当帧率**；
- `latch_us`（内容 latch 到扫描的延迟）中位 4.8 ms、最大 8.7 ms，`fallbacks=0`、
  `latch_timeouts=0` —— 这三项说明**直出链路本身没在等**，掉帧不在 DPU 这一层；
- UI 帧率**尚未用可靠口径测出**，滑动体感约 40 多 fps。

**下一次要怎么量（写完就按这个来，不再拍脑袋）**：
1. 用内核 `time_ms` 做分母（不是主机墙钟，主机秒会因为串口积压失真）；
2. 分子取 `PICASSO_DPU_FRAME` 的 `n`（累计帧）在同一时间窗口内的增量，且要求该窗口内
   屏幕上有**连续内容变化**（滑动/动画），静置窗口不计；
3. 同时记录 `backboardd` 的 CPU 占用，用来区分"渲染不出来"和"渲染出来但没提交"。

---

## 三、owner trust cache：一个库一个模块，模块 uuid 由 cdhash 派生

trust bundle 的格式不复杂：`u32 count` + `u32 offset[count]` + 若干模块；模块是
`u32 type` + `u8 uuid[16]` + `u32 entries` + 24 字节的条目。

不显然的是这条规则：

```
module_uuid = uuid5(NAMESPACE_URL, "picasso:statusbar:" + cdhash_hex)
```

我在三份独立生成的模块上验证过。由此引出两个结论：

1. **把两个不同的库塞进同一个模块**（uuid 只能对应其中一个 cdhash），启动器会直接返回
   `EFI_COMPROMISED_DATA`，连内核都进不去。
2. **模块数量本身不是限制**。5 个模块（两个 Apple 基线 + GPU addon + 两个单条目库模块）
   照样能启动，只要每个模块的 uuid 和它自己的内容自洽。

这条坑让我废掉两次启动，最后才发现问题不在"模块太多"。

---

## 四、kernel 与 seed 是配对的；卷 UUID 存在两个地方

- **新内核 + 旧种子** → `panic: offline scratch Data mount failed: 2`（ENOENT）。它们是同一次构建的产物，不能交叉组合。
- **卷 UUID 存在两处**：内核里一处 ASCII 字符串，种子里 **7 份大端二进制副本**。
  只改内核那一处，结果仍是 `EFI_COMPROMISED_DATA`——内核会拿自己的字符串去比种子里的。

**但正常情况下你不需要动它**：同一次构建的 kernel + seed 两侧天然一致。
只有混用不同批次的产物时才需要对齐，正确做法优先是"别混用"。

### 一条值得记下的红鲱鱼

种子里还有一个 48 字节零填充的 ASCII UUID，位于逻辑偏移 `0xa4000000`（一段 696 KB 稀疏 extent 的开头）。
**它不参与启动校验**：UEFI 固件里连 `DBRDISK` 这个串都出现 0 次，内核里也没有它。
实证是所有能启动的镜像在这一处都是 `D952AA48…`，而内核是 `0EBB496E…`——两者从来就不等，照样启动。
我在这上面花了不少时间才确认它无关。

---

## 五、还没解决的

### 键盘右侧被裁约 45 px

现象：键盘最右一排（p / l / return）被切掉约半个键宽。
先后面向两个假设做了单变量实验，**两个都失败**：

| 实验 | 变量 | 结果 |
|---|---|---|
| ADT `model`：`iPhone10,3` → `iPhone13,1` | 布局宽度 375 pt → 360 pt（`target-type` 仍为 `D22`） | 无变化 |
| `display-scale` 3 → 2 | 逻辑宽度 360 pt → 540 pt | 无变化 |

所以它既不按 `hw.machine` 选模板，也不按逻辑宽度算。下一步是在 SpringBoard 注入里打印
`UIScreen.bounds / nativeBounds / scale / nativeScale`，看 1125 这个数到底从哪来。

### 音频 / WiFi

两者共用同一道门：ADSP 与 WCN3990 的固件必须经 **PIL / TrustZone** 鉴权才能启动，而内核里
SCM 客户端是 0（`scm_call` / `qcom_scm` / `pas_init` 字符串出现 0 次，设备树无 `qcom,scm`）。
GPU 能跑恰恰是因为它**绕开**了 PIL（ZAP 由 TZ 鉴权，本内核只做容器解析）。

已写好只读探针（`GET_VERSION` + `IS_CALL_AVAILABLE`，外加一条 `PSCI_VERSION` 对照组用于区分
"我们的内联汇编写错"和"TZ 真的拒绝"），结果未出，**暂不下结论**。

---

## 六、复现要点（命令级）

```bash
# 1. 只 boot 不 flash，永远留一条回退路
fastboot boot boot.img

# 2. 判断是否真的走了 GPU（不是看画面）
grep "PICASSO_ACCEL published="      console.log   # 期望 0
grep "PICASSO_GPU open"              console.log   # 期望 plugin type=4147
grep -c "PICASSO_DISPLAY copy"       console.log   # 期望 0

# 3. 量帧率（唯一可靠的口径）
grep -o "PICASSO_DPU_FRAME n=[0-9]* period_us=[0-9]*" console.log | tail -20
```

必备启动参数（少一个就退回软件路径）：

```
sheng-gpu-metal=1  sheng-gpu-addon=1  sheng-dpu-scanout=2
```

---

## 七、致谢

上游 `YinCQL/UniXNU-picasso` 的文档密度是这件事能在 Windows 上做起来的前提——
尤其是 trust cache 的构建脚本和 `PicassoDisplay` 的 policy 代码，上面四条里有三条是从它们里面读出来的。
`silenzio111` 在 #19/#20/#21 里的渲染路径与性能分析，直接给了我可对照的帧率基线。

本文所有数字都来自手机上的控制台日志；没有引用的地方就是没测过。

---

## 八、如何复刻

**镜像本身不提供**。boot 镜像里有两样东西不适合公开分发：

| 文件 | 内容 | 为什么不能放 |
|---|---|---|
| `SCRATC~1.IMG` | iOS 数据盘内容（种子） | 含 Apple 的用户态文件 |
| `KERNEL~1.TRU` | Apple 二进制的 cdhash 白名单 | 分发它等于分发一套签名绕过构件 |

所以这里只放**能自己构建出同样结果**的东西：

| 目标 | 位置 |
|---|---|
| 完整构建配方：参数、trust 规则、UUID 两处对齐、boot 前门禁、日志判读 | [`docs/BUILD.md`](docs/BUILD.md) |
| **在别人的 K30 上跑起来**：机型判定、UUID 怎么定、屏幕参数怎么改 | [`docs/OTHER-DEVICES.md`](docs/OTHER-DEVICES.md) |
| **备份与恢复**：备份哪些分区、怎么备、怎么刷回去、出事了怎么办 | [`docs/BACKUP.md`](docs/BACKUP.md) |
| 日志关键字对照表（每条打印的含义与正常值） | [`docs/LOG-KEYWORDS.md`](docs/LOG-KEYWORDS.md) |
| 可直接使用的构建工作流（含本文两处关键改动） | [`ci/`](ci/) |
| 改 payload / 改 UUID / 合并 trust / 启动前自检 / 抓日志判读 的脚本 | [`tools/`](tools/) |

照着 `docs/BUILD.md` 走，用的是上游的构建脚本 + 这里的几个工具，产出的是**你自己那份**镜像，
也只应该用在你自己的设备上。
