# NO_DRAWABLE 探针提案（未应用、未构建）

状态：**proposal only**。冻结令下不 dispatch、不出包。刻意写成 `.md` 而不是 `.patch`，
避免有人把它当补丁直接塞进 `LATE_OVERLAYS`。

## 为什么需要

`logs/stall-03.txt` 里 backboardd 走到 `eagl macroctx`（检查点 #11）就再没有 EAGL 调用，
第 12 个检查点 `texk IOSurface attached` 从未出现。但 `-texImageIOSurface:` 的早退分支
只调用 `say()`，而 `say()` 不走串口（全段 `hakios` 命中 0），所以现在分不清：

1. 调用根本没发生；还是
2. 调用发生了，但因为 `texture == 0` 或 `image_new() == NULL` 在 `note_format()` 之前就 return NO。

## 最小改动（诊断，不改行为）

在 `drivers/NativeGL/eagl/eagl.c` 的 `my_texImageIOSurface` 入口与两个早退点各加一条 `ktrace`：

```c
static BOOL
my_texImageIOSurface(id self, SEL selector, IOSurfaceRef surface, unsigned long target, unsigned long internal,
    GLsizei width, GLsizei height, unsigned long format, unsigned long type, uint32_t plane, BOOL invert)
{
    ...
    /* NEW: entry, before any early return */
    ktrace("texk entered surface=%p target=%#lx", surface, target);
    if (e == NULL || surface == NULL) {
        ktrace("texk refused surface=%p ctx=%p", surface, e);        /* NEW */
        return NO;
    }
    eagl_enter(e, 0);
    clear_now(e);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    if (target != GL_TEXTURE_2D || texture == 0 || (image = image_new(e, (GLuint)texture, surface)) == NULL) {
        ktrace("texk refused target=%#lx texture=%d image=%p", target, texture, image);  /* NEW */
        say("hakios-gl: -texImageIOSurface: target %#lx, texture %d: not done\n", target, texture);
        return NO;
    }
```

判读：

| 新日志 | 结论 |
|---|---|
| 连 `texk entered` 都没有 | 调用方根本没调 → 问题在调用方（CA/GL 的 drawable 建立顺序），不在 EAGL 包装 |
| `texk entered` 有、`texk refused …` 有 | 调用发生并早退：看 `texture=` 和 `image=` 就能定死是哪一个条件 |
| `texk entered` 有、无 refused、也无 `IOSurface attached` | 卡在中间（`clear_now`/`image_new` 内部），再往里加一层 |

## 不要先做的两件事

1. **跳过已成功的 surface-root 查找**：S03 的 `IOCoreSurfaceRoot` 是
   `rpc=0 result=0 object=1`，本来就成功，跳过它不改变任何东西。
2. **给 texImageIOSurface 加重试**：目前没有证据表明它被调用过并失败；
   重试一个可能从未被调用的函数是空转，且会把 `image` 生命周期弄得不可控。

## 与 SEED 的关系

`docs/boot-stability-diagnosis-20261113.md` §8 的转移矩阵显示
`P(NO_DRAWABLE | 上一段 SEED) = 1/15`、`P(NO_DRAWABLE | 上一段 PASS) = 3/19`，
Fisher p = 0.613 ⇒ 没有状态依赖证据。所以本探针**不要**和 `PICASSO_SEED` 诊断放进同一版，
否则读不出是哪条改动起了作用。
