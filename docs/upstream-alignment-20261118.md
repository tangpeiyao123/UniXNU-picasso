# 对齐上游：能不能直接用他们的产物开机（2026-11-18）

只读调研。没动设备、没 flash、没派发 CI。依据：GitHub API 实查 + 本地校验 + `git diff`。

## 一句话结论

**不能直接用。** 上游只对 `d627aa63`（2026-11-12，**PR #23 合并之前**）产出过 boot + seed 成对产物；
合并后的 `main-ios16` 尖端 `39d7908`（含 #23 音频/图形、#24 麦克风）**没有任何 boot 镜像 artifact，只有 0.1 MB 主机库**。

## 一、上游产物清单（详见 `docs/evidence/upstream-artifacts-20261118.txt`）

| 产物 | 大小 | commit | 说明 |
|---|---|---|---|
| `picasso-bootimg-190` | 55.3 MB | `d627aa63` | boot.img，**已本地校验 sha256 一致** |
| `picasso-linux-bootimg-23` | 52.4 MB | `d627aa63` | Linux 侧构建产物 |
| `picasso-data-seed-190` | 1.3 MB | `d627aa63` | `setup.dbr`（20 GiB→1.3 MB），**已展开并校验** |
| `picasso-generated-data-seed-1` | 1.2 MB | `35ba40fd`（11-10） | 更早的种子 |
| `picasso-host-*` ×3 | 0.1 MB | `39d7908` / `79161cfa` | **只有主机库，无镜像** |

关键事实：**`39d7908` 没有 boot 镜像**。想用 #23/#24 的修复，只能自己编；而这个仓库拿不到 macOS runner（三次派发：`cancelled / runner=空 / steps=0`）。

## 二、我们落后 218 个 commit，但分歧集中在 104 个内核/驱动文件

`git diff FETCH_HEAD HEAD`：**331 个文件，+7784 / −33440**（相对上游尖端）。

图形与 CI 的关键差异（上游版本已含 PR #23 的修复，我们的是旧实现）：

| 文件 | 差异规模 | 含义 |
|---|---|---|
| `drivers/NativeAdreno/NativeAdrenoPublish.cpp` | 255 行 | IOSurface publish/detach 路径 |
| `drivers/NativeAdreno/NativeAdrenoVm.cpp` | 73 行 | 显存/IOSurface 映射 |
| `drivers/NativeAdreno/NativeAdrenoDvfs.h` / `Profile.h` | 上游已删/改 | 频点策略 |
| `.github/workflows/compose-bootimg.yml` | 153 行 | 上游新增 `prepare-picasso-compose-seed.py` |
| `.github/workflows/generate-data-seed.yml` | **124 行删除** | 上游**删掉了**独立造种子流程 |

最后一行值得注意：**上游把"单独造种子"这条流程删了**，改成 compose 内部生成。这从侧面印证我们昨晚"自己造种子刷 userdata"是逆着上游设计走的。

`eagl.c`、`PicassoAccelerator.cpp` 在本次 diff 中没有单独列出（路径或已被上游移动/重命名），需要更细的逐文件比对才能下结论——**这一项标注为未完成**。

## 三、为什么昨晚"boot + seed 成对"仍然失败

实刷记录：`fastboot flash boot`（d627aa63）→ `fastboot flash userdata`（20 GiB 种子）→ 仍然

```
panic: offline scratch Data mount failed: 2 @kern_exec.c:10060   （10 次）
```

最可能的原因（未验证，不写成结论）：

1. 分区 **107 GiB**，种子是 **20 GiB 容器**，头部容量对不上；
2. `setup.dbr` 可能不是最终 Data 镜像，而是 compose 的**输入**（`compose-bootimg.yml` 里 `prepare-picasso-compose-seed.py` 消费它）；
3. 上游"保留既有 20 GiB seed"的前提，在这台设备的现有 Data 上已不成立——**我把原来的 Data 覆盖了，而且没有备份**。

## 四、现在要开机，需要什么

| 选项 | 前提 | 风险 |
|---|---|---|
| A. 回安卓（`boot-stock.img`） | 已有，sha 在 manifest | iOS 桌面彻底没了，但手机可用 |
| B. 用 `d627aa63` 产物 | 需要把 20 GiB 种子按上游的方式装进去（不是裸刷 userdata） | 机制未验证，可能再失败一次 |
| C. 编 `39d7908` | 需要 macOS runner 或等价本地工具链 | 当前两条都拿不到 |

## 五、建议顺序

1. **先确认 B 的装配方式**：读 `compose-bootimg.yml` + `prepare-picasso-compose-seed.py`，看 `setup.dbr` 到底被写到哪里、写多少——这一步纯只读，30 分钟内能给出"该刷哪个分区/偏移"的答案；
2. 若 B 的装配方式明确且与"裸刷 userdata"不同，再动设备；
3. 若 B 仍需要 macOS compose，则**先回安卓（A）**，iOS 线等有 macOS 环境再谈。

在写入方式搞清楚之前，**不再 flash 任何东西**。
