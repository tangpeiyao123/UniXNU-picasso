from pathlib import Path
import re

CR, LF, TAB = chr(13), chr(10), chr(9)
p = Path(r'D:/k30i-gpu/repo-work/patches/picasso-usb-console-xnu.patch')
raw = p.read_bytes()

def L(s):
    return (s + CR + LF).encode()
def T(s):
    return L('+' + TAB + s)

# --- 1) include: 去掉 level1 加的 mach/thread_info.h，保留 mach/task.h（task_threads 需要）---
drop = L('+' + '#include <mach/thread_info.h>')
assert raw.count(drop) == 1, f'inc drop count={raw.count(drop)}'
raw = raw.replace(drop, b'', 1)

# --- 2) 替换 picasso_thread_dump 函数体 ---
start = raw.find(b'+picasso_thread_dump(int pid)')
end = raw.find(b'+static void' + CR.encode() + LF.encode() + b'+ud_process_command', start)
assert start > 0 and end > start, (start, end)

BS = chr(92)  # 反斜杠，用于 \n
body = b''.join([
    L('+picasso_thread_dump(int pid)'),
    L('+{'),
    T('proc_t p = proc_find(pid);'),
    T('task_t task;'),
    T('thread_array_t threads = NULL;'),
    T('mach_msg_type_number_t count = 0, i;'),
    T('kern_return_t kr;'),
    L('+'),
    T('if (p == NULL) {'),
    T('\tprintf("PICASSO_THREADS pid=%d absent' + BS + 'n", pid);'),
    T('\treturn;'),
    T('}'),
    T('task = proc_task(p);'),
    T('kr = task_threads(task, &threads, &count);'),
    T('if (kr != KERN_SUCCESS) {'),
    T('\tprintf("PICASSO_THREADS pid=%d task_threads kr=%d' + BS + 'n", pid, kr);'),
    T('\tproc_rele(p);'),
    T('\treturn;'),
    T('}'),
    T('printf("PICASSO_THREADS begin pid=%d count=%u' + BS + 'n", pid, count);'),
    T('for (i = 0; i < count && i < 96; i++) {'),
    T('\t/* threads[i] is a mach port identifier, NOT a kernel thread_t tid.'),
    T('\t * Stable within one capture session; distinguishes the same thread.'),
    T('\t * Whether it maps to a known poller is verified only at capture time. */'),
    T('\tprintf("PICASSO_THREADS idx=%u port=%p' + BS + 'n",'),
    T('\t    i, (void *)(uintptr_t)threads[i]);'),
    T('}'),
    T('printf("PICASSO_THREADS end pid=%d count=%u' + BS + 'n", pid, count);'),
    T('proc_rele(p);'),
    L('+}'),
    L('+'),
])
raw = raw[:start] + body + raw[end:]

# --- 3) 重算 count 行 ---
lines = raw.split((CR + LF).encode())
n, in_hunk = 0, False
for ln in lines:
    if ln.startswith(b'@@ -0,0 +1,'):
        in_hunk = True
        continue
    if in_hunk and ln.startswith(b'diff --git'):
        break
    if in_hunk and ln.startswith(b'+') and not ln.startswith(b'+++'):
        n += 1
old_m = re.search(rb'@@ -0,0 \+1,(\d+) @@', raw)
old_n = int(old_m.group(1))
raw = raw.replace(b'@@ -0,0 +1,' + str(old_n).encode() + b' @@',
                  b'@@ -0,0 +1,' + str(n).encode() + b' @@', 1)

p.write_bytes(raw)
import hashlib
print('include removed: mach/thread_info.h; kept mach/task.h')
print('count', old_n, '->', n)
print('CRLF', raw.count((CR+LF).encode()), 'bare LF', raw.count(b'\n') - raw.count((CR+LF).encode()))
print('literal /r/n', raw.count(b'/r/n'))
print('sha256', hashlib.sha256(raw).hexdigest())
