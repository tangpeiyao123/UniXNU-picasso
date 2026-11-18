#!/usr/bin/env bash
# A′ 可归零门禁（第1级 attempt3：port 作线程标识 + 修 format warning）：
# 基线 0142419（探针恢复前）；累积 numstat=43/1；count 行 3335。
# 锚点含 task_threads / 逐线程 port 格式串；无 thread_info、无旧 count=%d format 写法。
cd /d/k30i-gpu/repo-work || exit 1
P=patches/picasso-usb-console-xnu.patch
BASE=0142419
grep -qF '#include <mach/task.h>' "$P" &&
grep -qF 'picasso_thread_dump(int pid)' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "threads")' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "kthreads")' "$P" &&
grep -qF 'task_threads(task, &threads, &count)' "$P" &&
grep -qF 'PICASSO_THREADS idx=%u port=%p' "$P" &&
grep -qF 'count=%u' "$P" &&
grep -qF '@@ -0,0 +1,3335 @@' "$P" &&
! grep -qF 'thread_info' "$P" &&
! grep -qF 'count=%d' "$P" &&
! grep -qF 't->machine.saved_state' "$P" &&
git diff --check -- "$P" &&
test "$(git diff --numstat "$BASE" -- "$P" | awk '{print $1"/"$2}')" = "43/1"
echo A_PRIME_RC=$?
