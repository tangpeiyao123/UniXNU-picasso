# configd 打不开网络栈（e00002c2）根因诊断 + WiFi 现状与工程量评估（2026-11-22）

> 一句话结论：`PICASSO_NETWORK stack_open ... result=e00002c2 client=0` 的失败点**不在**网络栈加载、也**不在** MACF/沙箱权限检查，而在 `is_io_service_open_extended` 里 `IONetworkStack.newUserClient(connect_type=0)` 没能建出 user client。但更重要的是：即使让这个 open 成功，也**只是消除报错、让设置页不再卡住**，不会合成任何网络接口——真正的问题是这台设备的 WiFi 数据路径整条不存在。原作者截至 2026-11-21 未解决 WiFi（口头承诺"soon"，13 天无提交）。

这份档案记录两件事：(1) `e00002c2` 的源码级诊断与一次被自己推翻的错误假设；(2) 原作者 WiFi 进度的核查结果 + 若要自研的真实工程量。请后来者先读第 3 节"误区"，再决定是否投入。

---

## 0. 适用范围与红线

- 设备：Redmi K30i（picasso / SM7250），设备 UUID `0EBB496E-FB09-4223-991E-22007B6FACD1`。
- 仓库：`tangpeiyao123/UniXNU-picasso` 分支 `upstream-latest-20261118`；上游 `YinCQL/UniXNU-picasso`（默认分支 `main-ios16`）。
- 证据来源：固化后串口日志 `D:/k30i-gpu/capture-flash-final/console-20261122-093517.log`（2026-11-22 09:35）。
- 红线：本文所有结论均来自**只读**诊断。任何内核改动都必须重走完整 CI + `fastboot boot` 临时验证，禁止直接 `flash boot` 到生产分区；userdata 布局红线见姊妹文档 `desktop-entry-failure-and-three-in-one-fix.md` 第 0 节。

---

## 1. 现象：日志原文

```
PICASSO_NETWORK stack load=0 found=1 resource_busy=1
PICASSO_NETWORK complete_view=1 readonly=1 union=0 error=0
PICASSO_NETWORK config stage=5 error=0 stack_s=5 quiet_s=5
PICASSO_NETWORK interest type=IOBusyInterest class=IOPlatformExpertDevice error=0
PICASSO_NETWORK stack_open pid=34 type=0 error=0 result=e00002c2 client=0
```

配套证据：`PICASSO_EXEC ok pid=34 path=/usr/libexec/configd` —— pid=34 确认为 configd。

插桩源码：
- `patches/picasso-network-timeout-xnu.patch:357-362`：MAC 拒绝分支，返回 `kIOReturnNotPermitted`，并打印 `service_mac_denied=1`。
- `patches/picasso-network-timeout-xnu.patch:369-371`：在 `is_io_service_open_extended` 末尾 `*result = res` 之后打印 `stack_open`，字段依次为 `connect_type / err / res / (client != nullptr)`。
- `patches/picasso-network-progress-xnu.patch`：在 `IOUserClient.cpp` 的 `registerInterest` 插桩 interest 日志。
- 内核改动基线：`patches/unixnu-source-kernel.patch:5488` 起为 `is_io_service_open_extended` 的 hunk。

---

## 2. 逐字段诊断（已确证 vs 推断分开）

| 字段 | 值 | 判定 | 依据 |
| --- | --- | --- | --- |
| `stack load=0` | 0 | **已确证：成功** | `package/PicassoDisplay.cpp:346-356`：`loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IONetworkingFamily")`，`kern_return_t` 里 0 = `kIOReturnSuccess`。**曾有 AI 误读为"加载失败"，务必不要重犯。** |
| `found=1` | 1 | 已确证：服务存在 | `waitForMatchingService("IONetworkStack", 1s)` 返回非空 |
| `resource_busy=1` | 1 | 中性 | `getResourceService()->getBusyState()=1`，单一值不能证明失败 |
| `complete_view=1 readonly=1 union=0 error=0`、`config stage=5 error=0` | — | 已确证：成功标记 | `picasso-network-timeout-xnu.patch:301-302`：`int stage = error ? 2 : 5`，故 stage=5+error=0 是 InterfaceNamer plist 绑定成功 |
| `error=0` | 0 | **已确证：`err` 恒为 `KERN_SUCCESS`，不代表"检查通过"** | XNU 官方 `iokit/Kernel/IOUserClient.cpp:4388` `err = KERN_SUCCESS`；整条 `newUserClient` 失败路径里 `err` 从未被重新赋值（唯一会改它的段落在 `#if 0` 里）。**不能把它读成"MACF 权限检查通过"**——这是本档案写作过程中修正过的一次措辞错误。真正排除 MACF 拒绝的依据是：MACF 在 4404 行就直接 `return kIOReturnNotPermitted`，不会走到 4605 行的 `*connection` 赋值，也就不会打印 `stack_open`；而日志里出现了 `stack_open` 行。 |
| `result=e00002c2` | `kIOReturnBadArgument` | 已确证：内核没接受这次 open | 该字段是 `res`，来自 `newUserClient` |
| `client=0` | 0 | 已确证：user client 未创建 | 末字段 `(client != nullptr)` |

**根因判定（源码级闭环）**：
- 失败路径 = `is_io_service_open_extended`（XNU `IOUserClient.cpp:4442`）中 `res = service->newUserClient(owningTask, ..., connect_type=0, propertiesDict, &client)` 返回 `kIOReturnBadArgument`，client 保持 NULL，于是 `*result=e00002c2`、`*connection=0`、函数仍 `return err=0`。这精确解释了 `error=0 / result=e00002c2 / client=0` 这个看似矛盾的组合。
- **最后一环已定位**（Apple XNU `iokit/Kernel/IOService.cpp:8232-8235`）：`IOService::newUserClient` 在 `client->initWithTask(owningTask, securityID, type=0, properties)` 返回 false 时 `return kIOReturnBadArgument`。即：找到了 user client 类、alloc 成功、类型检查通过，但 **`initWithTask` 拒绝了 type=0 / properties**。
- 【确证边界】`initWithTask` 的具体实现在 `IONetworkStackUserClient`（Apple 二进制 `IONetworkingFamily.kext`，非本仓源码）。为什么它对 configd 的 type=0 返回 false（是 type 不支持、properties 不符、还是 entitlement 校验），本仓源码无法进一步判定；需在设备上对 `initWithTask` 加插桩才能坐实。
- **构建门禁层面已锁死**：`scripts/prelink-picasso.py:147-158` 对 `IONetworkStack` 的 personality 做 require 校验——`"Network stack must keep its original matching and user client"`，且校验 `__TEXT_EXEC` 指令与原始二进制逐字节相同（`"Original network-stack instructions changed"` 会 fail）。也就是说：即便想 patch 二进制 kext 的 user client 让它接受 type=0，也会被本仓 prelink 门禁直接挡下。想试必须同时改构建校验，风险与工作量陡增。

---

## 3. 误区清单（最值得后来者读的部分）

1. **把 `stack load=0` 读成"加载失败"** —— 错。0 是 `kIOReturnSuccess`。这是最容易犯、且已经犯过一次的错。
2. **以为"给 sandbox grant 加 configd"能修 e00002c2** —— 错。两重理由：(a) `error=0` 不是"MACF 通过"的证据，它只是 `err` 从未被赋值；MACF 拒绝会发生在本仓 patch 的 `!native_sandbox_grant && mac_iokit_check_open_service != 0` 分支，返回 `kIOReturnNotPermitted` 并打印 `service_mac_denied=1`，日志里都没有。(b) 真正返回 `kIOReturnBadArgument` 的是 `IOService::newUserClient → initWithTask`，在 `native_sandbox_grant` 逻辑之后，加 grant 完全不触及该路径。即便能走到后面的 sandbox filter（`io_filter_resolver`），那也是 `res=kIOReturnNotPermitted`，不是这里的 `e00002c2`。
3. **以为"配置 stage=5"是失败码** —— 错。源码 `stage = error ? 2 : 5`，stage=5 正是成功。
4. **以为"消除了 e00002c2 = 能上网"** —— 大错。作者文档明说 open 成功也不合成 interface/MAC/QUIET/COMPLETE，不等于有网（见第 5 节）。
5. **以为"启用 wifid 就能修网络等待"** —— 错。`docs/picasso-wlan-bringup.md:56-57` 明确：About 页网络等待要独立从 configd/InterfaceNamer 诊断，单独开 wifid 修不了驱动层缺口。

---

## 4. 为什么这个"失败"是被设计保留的

`docs/build168-settings-brightness.md` 是作者对此事的详细交代，关键原文：

- 现象：`Preferences` 卡在 `PSGAboutDataSource::_macAddress -> SCNetworkInterfaceCopyAllWithPreferences -> dispatch_once`，等 `Plugin:InterfaceNamer`（`SCDynamicStoreNotifyWait`）。
- "Loading IONetworkingFamily succeeded, but finding IONetworkStack alone did not complete the naming handshake."
- 修法：把 Apple configd 的 `WaitStackTimeout` / `WaitQuietTimeout` 从默认 300/240 秒压到 5 秒（在 launchd 前只读 union 挂载 Info.plist，保留全部原元数据与 `Builtin=true`，不改可执行文件、不改签名策略）。
- 明确边界："no interface, MAC address or QUIET/COMPLETE notification is synthesized. ... **This bounds the startup wait; it does not fix every unfinished device match or implement Wi-Fi.**"

也就是说：`e00002c2` 这个 open 失败是**已知、被界定、故意不修**的——目标是让设置页 5 秒内超时继续，而不是打通网络。

---

## 5. WiFi 现状：原作者进度（核查于 2026-11-22）

- 上游 `YinCQL/UniXNU-picasso` 最后推送 `2026-11-21T11:06Z`，为 `feat/debug` 的 "customize Data seed offline launch policy"，与 WiFi 无关。
- 近 100 条提交中，WiFi/WLAN 相关仅 3 条（均 11-17）：`ab034d9b`、`c3280261`、`5d431051`，全是 InterfaceNamer / configd **诊断**，无实质修复。
- Issue #22（本项目使用者所提，含 `WLAN_MSA_BASE` 常量问题）：作者 `silenzio111` 于 11-09 回复原文 "wifi, we are working for it, it doesn't work"、"but it will be available soon"。此后 13 天无 WiFi 提交。
- 源码硬门槛仍在：
  - `drivers/NativeWlan/WlanProbe.cpp:196-206`：stage3 的 PIL/SCM 加载不可用（"no SCM/TZ call in this tree and no wlan PIL node"），WLFW QMI 请求/指示均跳过，返回 `WLAN_ERR_NOTSUP`。
  - `WlanProbe.cpp:266-271`：stage5 "no Apple 802.11 stack (IO80211) or mac80211-style data path"，返回 `WLAN_ERR_NOTSUP`。
  - 正常启动参数下 stage3/stage5 都不开启；日志里只有 `PICASSO_WLAN smem ... radio_started=0` 和 smp2p 两行——是有意的只读 survey，不是崩溃。

**结论：原作者未解决 WiFi，处于"知道、在做、明确说现在不能用"的状态。**

---

## 6. 若要自研：真实工程量（来自 `docs/picasso-wlan-bringup.md` 的 5 步）

1. 适配 picasso 的 MPSS 签名固件、board data、私有 NV；在 Distribution 私有 blob 之外实现受控固件访问。
2. 实现板级 PMIC/clock 归属、secure load、共享内存权限、有序关机/恢复。
3. 实现 SMP2P/GLINK/QRTR/QMI 及所需 RMTFS/TFTP/PD/PDR/DMS 服务；证明 WLFW 固件就绪与 picasso MSA/SMMU 映射。
4. 集成 ath10k SNOC、全部 12 个 CE 中断、DMA/cache 归属与 worker；先证明扫描可用，再谈认证与数据流。
5. 接入选定的 iOS Apple80211 ABI 与真实接口/MAC；此后才能启用依赖 Wi-Fi 的 launch jobs，测试 DHCP/DNS/流量/重连。

这是数周量级的平台 bring-up 工程，不是改一两个 boot-arg 能解决的。参考文档提到 Raphael 机型曾做类似移植，结果是"能连但不稳定、WiFi 重新使能失败"。

**可行性裁定**：
- 想"消除 e00002c2 报错、让设置页正常"：属内核 patch 级，需先坐实 `newUserClient` 失败原因，再谈是否值得；即便成功也**不能上网**。
- 想"让这台设备能上网"：必须先完成上面 5 步，蓝牙 PAN 客户端也不构成捷径（iOS 原生只做分享方 NAP，不做客户端 PANU；本机也无物理网络接口）。

---

## 7. 给后来者的精确措辞（可直接引用）

> `e00002c2` 表示 configd 打开 IONetworkStack 的 user client 失败（`IOService.cpp:8234`：`initWithTask` 返回 false → `kIOReturnBadArgument`），不是网络栈加载失败（`stack load=0` 是 `kIOReturnSuccess`），也不是权限被拒（权限拒绝会走到不同的返回码且不会打印 `stack_open`）。日志里的 `error=0` 只是 `err` 恒为 `KERN_SUCCESS`，不能解读为"检查通过"。这是作者已知且刻意保留的状态：他们把 configd 等待超时从 300/240 秒压到 5 秒防止设置页卡死，但没有合成任何网络接口，也没有实现 Wi-Fi。原作者截至 11-21 未解决 WiFi。真正让这台设备联网需要完成固件、协议栈、ath10k 驱动、Apple80211 接口五大部分，是数周量级的 bring-up 工程；单纯消除这个报错不等于有网，加 sandbox grant 也不对症。
