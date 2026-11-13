patches/held — 未加入构建的补丁（不是废弃）

picasso-data-seed-retry-xnu.patch
--------------------------------
来源：ca349c0（2026-11-12 20:34）。它想修的是
`panic: offline scratch Data mount failed: 2 @kern_exec.c:9908`。

为什么先扣住：

1) 它在 kern_exec.c 里循环 40 次调用 `native_ram_block_volume_seed()`，
   而 **240 秒等待就写在这个函数里面**（NativeRamBlock.cpp / selectVolumeSeed：
   `waited++<2400` × `IOSleep(100)`）。一次失败 240 s，40 次最坏 9600 s，
   会把现在约 4 分钟的 panic 自愈变成两小时以上的挂死。
2) 2026-11-13 的 10 次实测（console-20261113-220343.log）显示失败是
   **双峰**：成功会话 worker 秒级发布；失败会话 240 s 内根本没走到 attachStore
   （`PICASSO_DATASTORE geometry` 一次都没打）。所以「retry 慢查询」这个假设
   与实测不符，重试不会提高成功率。

重新启用前必须满足：

- 让重试与等待**共享同一个 deadline**（重试不得再进 240 s 的等待），或
- 用 PICASSO_SEED 诊断（picasso-seed-diag-ramblock.patch）证明失败属于
  「慢但最终发布」，再决定放宽等待。

详见 docs/boot-stability-diagnosis-20261113.md 第 4 节。
