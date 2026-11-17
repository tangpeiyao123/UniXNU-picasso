# 镜像到手之后怎么刷（本轮不刷，只备方案）

前提：**目前手上没有任何含 PR #23/#24 的镜像**。尖端 `39d7908` 从未产出 boot.img，
唯一存在的镜像对是 `d627aa63`（合并前两天），而且它配我们现有的 Data 已经失败过一次。

## 一、什么条件下的产物才值得刷

必须同时满足，缺一不刷：

| 判据 | 怎么查 |
|---|---|
| 镜像来自 `39d7908` 或更新 | artifact 的 `workflow_run.head_sha` 前缀 |
| 与 seed 成对（同一 run） | artifact 名 + 同一 `head_sha` |
| see 身份与 Data 一致 | `seed.json` / `setup.json` 的 `seed_identity_sha256` 与设备分区上的一致 |
| 主机侧校验通过 | `boot.json` 的 `image_sha256` == 实际 `sha256sum boot.img` |

## 二、首选：`fastboot boot`（临时引导，不写分区）

```
fastboot boot <boot.img>
```

判据（任一不满足即停）：

1. `fastboot boot` 返回 `Booting OKAY`；
2. 串口 banner 出现且内核构建日期 = 目标 commit 的日期；
3. **不出现** `offline scratch Data mount failed`；
4. `done stage=4` 出现 → Data 挂上了；
5. `texk IOSurface attached` 出现 → drawable 挂上了；
6. `PICASSO_DPU_FRAME` ≥ 2 → 真出帧。

前 3 条在 90 秒内可判；第 4 条约 2 分钟；5–6 条约 4 分钟。

## 三、只有 `fastboot boot` 反复不成立时，才考虑成对写入

```
fastboot flash boot    <boot.img>
fastboot flash userdata <seed 镜像>     # 仅当 seed 与 boot 同 run，且分区尺寸匹配
```

**分区尺寸必须核对**：本机 `partition-size:userdata = 0x1AC2BFB000 = 107 GiB`。
若 seed 的逻辑尺寸不等于分区尺寸，**不要刷**——2026-11-17 就是这么失败的
（20 GiB 种子刷进 107 GiB 分区，`@kern_exec.c:10060` 依旧）。

## 四、回退路径（每次都先确认可用再动手）

```
fastboot flash boot D:/k30i-gpu/next/boot-685-pscireset+trustcompat2h-4a6a853d.img
# sha256 4a6a853d98b6… 与 rollback-manifest.json 一致
fastboot flash boot D:/k30i-backup/boot-stock.img      # L2 回安卓
# sha256 06147053… 与 manifest 一致
```

注意：**`4a6a853d` 现在也起不来**，因为它的 Data 已被覆盖；回退到它只能拿回内核，
拿不回桌面。真正"能用"的回退只有 L2 安卓。

## 五、仍然缺的东西（按重要性）

1. 一个 `39d7908` 的 boot.img —— 需要 macOS runner 或上游 collaborator 权限；
2. 与该镜像配套的 Data seed —— 若沿用 `seed_artifact_id`，需要上游那次 compose 的保留 artifact；
3. 设备上原有的 Data —— **已经被我覆盖，没有备份**，这是不可逆损失。
