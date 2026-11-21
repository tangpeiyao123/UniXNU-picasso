#!/usr/bin/env bash
# A′ 可归零门禁（编译修复 attempt 1 版）：
# 验证 threads/kthreads 探针最小编译修复后仍在补丁中、行数正确、diff 干净。
# 基线 0142419 = 探针恢复前（无 threads/kthreads）；累积到当前 = 30/1。
# 锚点变更：去 t->machine.saved_state；加 (p == NULL)/reachable/3322。
cd /d/k30i-gpu/repo-work || exit 1
P=patches/picasso-usb-console-xnu.patch
BASE=0142419
grep -qF '#include <sys/proc.h>' "$P" &&
grep -qF '#include <kern/task.h>' "$P" &&
grep -qF 'picasso_thread_dump(int pid)' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "threads")' "$P" &&
grep -qF 'ud_command_is(ud.cmd, "kthreads")' "$P" &&
grep -qF 'if (p == NULL)' "$P" &&
grep -qF 'PICASSO_THREADS pid=%d reachable' "$P" &&
grep -qF '@@ -0,0 +1,3322 @@' "$P" &&
! grep -qF 't->machine.saved_state' "$P" &&
git diff --check -- "$P" &&
test "$(git diff --numstat "$BASE" -- "$P" | awk '{print $1"/"$2}')" = "30/1"
echo A_PRIME_RC=$?
