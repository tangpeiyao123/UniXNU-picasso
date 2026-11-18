from pathlib import Path
import re

CR, LF, TAB = chr(13), chr(10), chr(9)
p = Path(r'D:/k30i-gpu/repo-work/patches/picasso-usb-console-xnu.patch')
raw = p.read_bytes()

def L(s):
    return (s + CR + LF).encode()

# --- 1) include: 在 kern/task.h 之后加 mach 公开 API 头 ---
inc_anchor = ('+' + '#include <kern/task.h>').encode() + CR.encode() + LF.encode()
assert raw.count(inc_anchor) == 1, f'include anchor count={raw.count(inc_anchor)}'
inc_new = inc_anchor + (
    L('+' + '#include <mach/task.h>') +
    L('+' + '#include <mach/thread_info.h>')
)
raw = raw.replace(inc_anchor, inc_new, 1)

# --- 2) 替换 picasso_thread_dump 函数体 ---
start = raw.find(b'+picasso_thread_dump(int pid)')
end = raw.find(b'+static void' + CR.encode() + LF.encode() + b'+ud_process_command', start)
assert start > 0 and end > start, (start, end)

def T(s):  # 函数体行，带 tab 缩进
    return L('+' + TAB + s)

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
    T('\tprintf("PICASSO_THREADS pid=%d absent' + chr(92) + 'n", pid);'),
    T('return;'),
    T('}'),
    T('task = proc_task(p);'),
    T(('kr = task_threads(task, &threads, &count);')),
    T('if (kr != KERN_SUCCESS) {'),
    T('\tprintf("PICASSO_THREADS pid=%d task_threads kr=%d' + chr(92) + 'n", pid, kr);'),
    T('\tproc_rele(p);'),
    T('\treturn;'),
    T('}'),
    T('printf("PICASSO_THREADS begin pid=%d count=%d' + chr(92) + 'n", pid, count);'),
    T('for (i = 0; i < count && i < 96; i++) {'),
    T('\tthread_identifier_info_data_t tinfo;'),
    T('\tmach_msg_type_number_t tinfo_count = THREAD_IDENTIFIER_INFO_COUNT;'),
    T('\tkern_return_t ikr = thread_info(threads[i], THREAD_IDENTIFIER_INFO,'),
    T('\t    (thread_info_t)&tinfo, &tinfo_count);'),
    T('\tprintf("PICASSO_THREADS tid=%d idx=%d port=0x%x thread_id=%llu kr=%d' + chr(92) + 'n",'),
    T('\t    i, i, threads[i],'),
    T('\t    (ikr == KERN_SUCCESS) ? (unsigned long long)tinfo.thread_id : 0ULL,'),
    T('\t    ikr);'),
    T('}'),
    T('printf("PICASSO_THREADS end pid=%d count=%d' + chr(92) + 'n", pid, count);'),
    T('proc_rele(p);'),
    L('+}'),
    L('+'),
])
raw = raw[:start] + body + raw[end:]

# --- 3) 重算第一个 new-file hunk 的源行数并更新 count 行 ---
lines = raw.split((CR + LF).encode())
n = 0
in_hunk = False
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
print('include added; count', old_n, '->', n)
print('CRLF', raw.count((CR+LF).encode()), 'bare LF', raw.count(b'\n') - raw.count((CR+LF).encode()))
print('sha256', hashlib.sha256(raw).hexdigest())
