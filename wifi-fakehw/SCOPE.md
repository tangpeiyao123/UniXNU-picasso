# 假硬件 Apple80211 OS 侧骨架 — 范围与诚实声明

本目录是**第一阶段冒烟测试**,不是 WiFi 可用实现,也不是 16.7 ABI 的校准结果。

## 它验证了什么
- 6 个 Apple80211 结构在 **iOS 16.5(HakiOS 基线)** 的尺寸/偏移自洽:Python ctypes 镜像与 HakiOS 原始 `_Static_assert` 一致。
- Apple80211 的 ioctl 分派外壳、事件编码(vendor=1/class=6/subclass=1/dv0="en0")、若干 get/set happy path 的结构填充。
- errno 用的是 **XNU KERNEL 态真值**(EOPNOTSUPP=102、EAUTH=80;不是用户态的 45/97)。

## 它没有验证什么(重要)
- **同源自证**:6 个结构的期望值和字段数组都来自 HakiOS 同一份源码,`all_pass=true` 只证明"镜像与 HakiOS 声明一致",**不证明**这些布局等于真机 16.5/16.7 的 `IO80211.framework`。没有独立信源。
- **wifid 序列是构造的**(`fakehw_probe.py` 自称 "a plausible sequence"),不是真机实测。
- **阉割版**:缺 `copyin/copyout` 裁剪、`req_data` 校验、参数解析(set_scan 的 channel_count/ssid_length、set_join 的 key 判定)、set_power、give_network 的 elements/rates、station 边界的线程/锁/串行化(sta.c 的 `sta_runs`)。
- **不是 16.7 校准**:16.5→16.7 的 ABI 是否相同未被证实。真正校准能力来自**移植后的 C 代码在真机打印的日志**(`apple80211.c:667-738`),本骨架只提供日志格式模板与 `verify_layout(overrides)` 的 diff 框架。

## 评审已修正的错误(2026-11-22)
独立评审 + XNU errno.h 一手核实,修正了:
- `EOPNOTSUPP` 45→102、`EAUTH` 97→80(原值会污染真机日志校准)。
- `ask_join()` 从"同步返回 -EAUTH"改为只返回 0(认证结果走 JOIN_DONE 异步事件,对齐 sta.c:281-298)。
- 单一 `join_result`(消除双副本,对齐 apple80211.c 的 static)。
- get(19) power `state={1,0,0,0}`、channel_number(5220)=44、channel_fill OR `CHANNEL_20MHZ`、t=207 填 `passive[]`、link 事件 rssi、set_scan 的 EBUSY 分支。

## C 编译器 ABI 验证(2026-11-22 完成,替代原"下一步 1")
用 ziglang 0.17.1 自带的 clang 对 `verify_layout.c` + `apple80211_layout.h` 做了真实编译:
- 本机 Windows(x86_64-windows-gnu)编译并运行,打印 `assoc=592 link=32 channels=4824 channels_info=10016 scan_req=5456 scan_result=1164`,`SIOCSA80211=0x802869c8 SIOCGA80211=0xc02869c9`——与 Python 镜像、HakiOS 断言三方一致。
- 交叉编译 `-target aarch64-linux-gnu` 与 `-target aarch64-macos`(Darwin arm64)**编译期 `_Static_assert` 全部通过**,`-Wall -Wextra` 无告警。确认这套结构描述在 x86_64/arm64 三种目标 ABI 下自洽。**注意:arm64 自洽 ≠ 真机 20H392 兼容**——后者仍需真机日志校准。
- 逐字段回核 HakiOS 原始 `hakios.apple80211.c:95-192`:6 个结构的字段/类型/packed/偏移与断言逐字对应,转录无偏差(独立评审 A/B 项复核通过)。
- 补 `apple80211req` 断言(size=40、req_data offset=0x20):它是这组结构里唯一的 `aligned(8)` 字段,原先无断言覆盖(评审 C 项盲点)。此值由 C 结构定义推导,**非 HakiOS 原文自带断言**。
- 顺带把 ioctl type 43/44 的标注从 `?` 补为 `DRIVER_VERSION`/`HARDWARE_VERSION`(HakiOS 源码 :71-72、:295-296)。
- 固化物:`run_ci_check.sh`(用 **zig cc 自带 sysroot**,端到端实测 exit=0;原设计用 apt clang 交叉 macOS 会因缺 SDK+stdio.h 必然失败,已修正)、`.github/workflows/verify-layout.yml`(ubuntu-24.04 + `pip install ziglang` + Python 镜像检查)。
- **已合入 fork**(2026-11-22,用户授权):独立分支 `ci/apple80211-layout-check`,从 HEAD `3822c6d` 分出,只含本目录 CI 文件;不触碰远端的 `feat/ios16-gpu` 默认分支与工作区其它未提交项。

**验证升级了什么**:从"Python 镜像 = HakiOS 声明"升级为"真实 clang 在 x86_64/arm64 三目标下确认声明自洽 + 捕获转录/对齐意外"。
**没升级什么**:仍非 16.7 真机校准——期望值与结构定义同源,编译器验证**不引入任何关于真机 16.5/16.7 的新信息**;没有第二个独立信源。真机校准仍需移植后的 C 日志(`apple80211.c:667-738`)。断言只覆盖 size + 采样 offset,不等于"逐字段证明"(逐字段靠上面的人工回核)。

## 结论/下一步
作为"布局 + ioctl 分派外壳 + 事件编码 + 多目标 C ABI 自洽"的冒烟测试:**完成了,应停在主场**。
不要再堆 Python 分支。下一步优先级:
1. 移植 C 代码到真机时补 copyin/copyout 与 station 串行化,并用真机日志校准 16.7 ABI(唯一剩余的高价值目标)。
2. CI workflow 已合入 fork 独立分支(见上),可通过 PR 合并进主线;本会话未改动任何设备或现有分支。

设备状态:全程未触碰,仍为已固化的稳定三项合一镜像。
