#!/usr/bin/env bash
# A′ 可归零门禁（第1级 attempt2：公开 API 加逐线程 tid）：
# 基线 0142419（探针恢复前）；累积 numstat=47/1；count 行 3339。
# 锚点含 task_threads / THREAD_IDENTIFIER_INFO / 逐线程 tid 格式串；minimal marker 已移除。
cd /d/k30i-gpu/repo-work || exit 1
P=patches/picasso-usb-console-xnu.patch
BASE=0142419
grep -qF '#include <mach/task.h>' "$P" &&
grep -qF '#include <mach/thread_info.h>' "$P" &&
grep -qF 'picasso_thread_dump(int pid)' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "threads")' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "kthreads")' "$P" &&
grep -qF 'task_threads(task, &threads, &count)' "$P" &&
grep -qF 'THREAD_IDENTIFIER_INFO' "$P" &&
grep -qF 'PICASSO_THREADS tid=%d idx=%d' "$P" &&
grep -qF '@@ -0,0 +1,3339 @@' "$P" &&
! grep -qF 'reachable proc=%p task=%p minimal' "$P" &&
! grep -qF 't->machine.saved_state' "$P" &&
git diff --check -- "$P" &&
test "$(git diff --numstat "$BASE" -- "$P" | awk '{print $1"/"$2}')" = "47/1"
echo A_PRIME_RC=$?
