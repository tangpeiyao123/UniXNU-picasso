# 给上游作者的权限请求（可直接转发）

## 中文

> 你好，我在跟你的 UniXNU-picasso 项目（K30i / Picasso 移植）。你这周合的 PR #23（音频/图形/电源）和 #24（麦克风 + 调频）正是我们卡了很久的地方，非常有用。
>
> 遇到的困难：我 fork 的是 `tangpeiyao123/UniXNU-picasso`，落后 `main-ios16` 218 个 commit。我尝试在自己的 fork 上派发 `Compose Boot Image (FD + kernel + payload)`，三次都是 job 排队 15 分钟后被取消，`runner` 为空、`steps=0`（换成 `macos-15` 也一样）；`macos-15-intel` 与 `macos-15` 都试过。同样的 workflow 在你那边是能跑通的（比如 https://github.com/YinCQL/UniXNU-picasso/actions/runs/43218133828 ）。
>
> 而 `main-ios16` 尖端 `39d7908` 目前没有任何 boot 镜像 artifact，所以我没有办法拿到含 #23/#24 的可刷镜像。
>
> 想请你帮忙二选一：
> 1. 给 `tangpeiyao123` 一个 collaborator 权限（只读 + Actions 触发即可）；或
> 2. 你那边代为跑一次 `Compose Boot Image (FD + kernel + payload)`，参数：`fast=false`、`create_data_seed=false`（如果你建议造新种子，改成 `true` 也可以，我这边 Data 已经清了、可以重建）。
>
> 我只需要 `39d7908` 的 boot.img 产物 + 它配套的 seed，用来在本机做启动验证。不需要你改任何代码。

## English

> Hi — I'm working with your UniXNU-picasso port on the Redmi K30i (Picasso). PR #23 (audio/graphics/power) and #24 (mic + load-based DVFS) address exactly what we've been stuck on.
>
> Problem: my fork `tangpeiyao123/UniXNU-picasso` is 218 commits behind `main-ios16`. I dispatched `Compose Boot Image (FD + kernel + payload)` on my fork three times; every job sat queued for 15 minutes and was cancelled with an empty runner and zero steps (tried both `macos-15-intel` and `macos-15`). The same workflow works on your repo, e.g. https://github.com/YinCQL/UniXNU-picasso/actions/runs/43218133828
>
> The tip `39d7908` has no boot-image artifact, so I can't obtain a flashable image containing #23/#24.
>
> Could you either (1) add `tangpeiyao123` as a collaborator with read + Actions access, or (2) run `Compose Boot Image (FD + kernel + payload)` once on `main-ios16` with `fast=false` and `create_data_seed=false` (or `true` if you think a fresh seed is better — my Data is already wiped and can be rebuilt)?
>
> I only need the `39d7908` boot.img and its matching seed to verify boot locally. No code changes needed on your side.

## 附带说明（给我方）

如果拿到权限，需要的不只是"能点按钮"：还要能读到那个 run 的 artifacts（当前 token 对上游是 `pull:true`，读 artifact 列表可以，下载也可以）。
