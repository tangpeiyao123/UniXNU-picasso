# 线程栈探针（内核侧，未编译验证）

状态：**proposal only**。冻结令下不 dispatch。写成 `.md` 而不是 `.patch`，避免被 `LATE_OVERLAYS` 误收。

## 为什么需要它

2026-11-14 08:47 实机验证：`panic-test` 只能抓到发命令的 CDC poller（kernel_task tid 155），
导出的 kcdata 8565 B、无任何线程名。要回答「NO_DRAWABLE 停住时 backboardd 在干什么」、
「SEED 时 ram-block worker 卡在哪」，必须在**不 panic** 的前提下按需抓指定进程的线程状态。

## 落点

`patches/picasso-usb-console-xnu.patch` 的命令解析处（`ud_process_command`），
新增一个 `threads` 命令。选择这里是因为该文件已经包含 `kern/thread.h`，且命令通道是现成的。

需要在文件头部补两个 include：

```c
#include <sys/proc.h>        /* proc_find, proc_task, proc_rele */
#include <kern/task.h>       /* task_t, task->threads */
```

## 代码（拟）

```c
/* Read-only: print each thread of one process and the state it was saved in.
 * Saved state of a blocked thread is its user context, which is what we need:
 * a userspace pc that can be symbolized against libpicasso-sb.dylib. */
static void
picasso_thread_dump(int pid)
{
	proc_t p = proc_find(pid);
	task_t task;
	thread_t t;
	int n = 0;

	if (p == PROC_NULL) {
		printf("PICASSO_THREADS pid=%d absent\n", pid);
		return;
	}
	task = proc_task(p);
	printf("PICASSO_THREADS begin pid=%d\n", pid);
	queue_iterate(&task->threads, t, thread_t, task_threads) {
		arm_saved_state_t *ss = t->machine.saved_state;
		printf("PICASSO_THREADS t=%d tid=%d state=%d pri=%d ss=%d pc=%llx lr=%llx sp=%llx\n",
		    n++, thread_tid(t), (int)thread_get_state(t), (int)t->priority,
		    ss != NULL,
		    ss ? (unsigned long long)get_saved_state_pc(ss) : 0ULL,
		    ss ? (unsigned long long)get_saved_state_lr(ss) : 0ULL,
		    ss ? (unsigned long long)get_saved_state_sp(ss) : 0ULL);
		if (n >= 64) break;      /* bounded: the console is slow */
	}
	printf("PICASSO_THREADS end count=%d\n", n);
	proc_rele(p);
}
```

命令分支：

```c
} else if (ud_command_is(ud.cmd, "threads")) {
	/* threads <pid>; default 50 is backboardd on this board */
	int pid = 50;
	if (ud.cmd[7] == ' ') pid = (int)strtol(&ud.cmd[8], NULL, 10);
	picasso_thread_dump(pid);
}
```

## 编译器风险（必须先承认）

这份代码**没有在任何工具链上编译过**（本机不能编内核）。已知不确定点：

1. `task->threads` 的链字段名（`task_threads`）在不同 XNU 版本一致，但本树的 `thread_t` 定义未在本机核对。
2. `thread_get_state(t)` 的返回类型/可用性未核对；若不可用，直接删掉该字段。
3. `proc_task()` / `proc_rele()` 的头文件归属（`sys/proc.h`）未核对。
4. `arm_saved_state_t` 与 `get_saved_state_pc()` 需要 `arm64/saved_state.h`，可能要补 include。

因此**第一次构建大概率要修一轮编译错误**；这是可接受的代价（CI 会直接报错，不会产出坏镜像），
但不应该在还没决定出包的时候假装它已经能编。

## 判读

- 若 `PICASSO_THREADS` 里 backboardd 的线程都停在 `mach_msg_trap` 一类的 pc → 它在等 IPC，
  下一步查是谁没回包（而不是查 GL）。
- 若某个线程的 pc 落在 `libpicasso-sb.dylib` 的 texImageIOSurface 附近 → 调用发生过、卡在里头，
  与「没调用」区分开。
- 若线程列表里根本没有渲染线程 → 用户态还没建那条线程，问题在更早的阶段。

## 与 SEED 的关系

同一个命令可以直接 `threads <ram-block worker pid>`（该 worker 属于 kernel_task，pid 0，
所以第一版可能看不到它；那就需要第二版按线程名/kext 过滤）。SEED 的 worker 在 kernel_task 里，
这一点先记下来，别在第一版里硬求。
