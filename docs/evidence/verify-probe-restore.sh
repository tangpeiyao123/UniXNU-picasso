#!/usr/bin/env bash
# A′ 可归零门禁（第1级 attempt4：idx-only 最小逐线程输出）：
# 基线 0142419（探针恢复前）；累积 numstat=42/1；count 行 3334。
# 锚点含 task_threads / idx-only 格式串；无 threads[i]/port 强转、无 thread_info。
cd /d/k30i-gpu/repo-work || exit 1
P=patches/picasso-usb-console-xnu.patch
BASE=0142419
grep -qF '#include <mach/task.h>' "$P" &&
grep -qF 'picasso_thread_dump(int pid)' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "threads")' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "kthreads")' "$P" &&
grep -qF 'task_threads(task, &threads, &count)' "$P" &&
grep -qF 'PICASSO_THREADS idx=%u' "$P" &&
grep -qF 'count=%u' "$P" &&
grep -qF '@@ -0,0 +1,3334 @@' "$P" &&
! grep -qF 'threads[i]' "$P" &&
! grep -qF 'thread_info' "$P" &&
! grep -qF 'port=%p' "$P" &&
! grep -qF 't->machine.saved_state' "$P" &&
git diff --check -- "$P" &&
test "$(git diff --numstat "$BASE" -- "$P" | awk '{print $1"/"$2}')" = "42/1"
echo A_PRIME_RC=$?
