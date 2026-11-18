#!/usr/bin/env bash
# A′ 可归零门禁（attempt5：换编译单元取逐线程 state/continuation）：
# 基线 0142419（探针恢复前）；累积 numstat=56/2。
# usb 侧 picasso_thread_dump 为 stub，调 picasso_thread_dump_internal()；
# debug.c 侧 internal 函数直接走 task->threads，输出 tid/state/cont 格式串。
cd /d/k30i-gpu/repo-work || exit 1
P=patches/picasso-usb-console-xnu.patch
BASE=0142419
grep -qF 'picasso_thread_dump(int pid)' "$P" &&
grep -qF 'picasso_thread_dump_internal(pid)' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "threads")' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "kthreads")' "$P" &&
grep -qF 'picasso_thread_dump_internal(int pid)' "$P" &&
grep -qF 'queue_iterate(&task->threads, thread, thread_t, task_threads)' "$P" &&
grep -qF 'PICASSO_THREADS tid=%u state=%d cont=%p' "$P" &&
grep -qF '@@ -0,0 +1,3309 @@' "$P" &&
! grep -qF 'task_threads(task, &threads, &count)' "$P" &&
! grep -qF 'thread_info' "$P" &&
! grep -qF 'port=%p' "$P" &&
! grep -qF 'PICASSO_THREADS idx=%u' "$P" &&
! grep -qF 'panic_spin_forever_impl' "$P" &&
! grep -qF 'proc_rele(proc_find(pid))' "$P" &&
git diff --check -- "$P" &&
test "$(git diff --numstat "$BASE" -- "$P" | awk '{print $1"/"$2}')" = "56/2"
echo A_PRIME_RC=$?
