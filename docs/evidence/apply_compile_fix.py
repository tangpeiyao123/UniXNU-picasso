from pathlib import Path

CR = chr(13)
LF = chr(10)
TAB = chr(9)
p = Path(r'D:/k30i-gpu/repo-work/patches/picasso-usb-console-xnu.patch')
raw = p.read_bytes()

# 当前被污染的函数体（用实际读到的字节锚定）
start = raw.find(b'+picasso_thread_dump(int pid)')
end = raw.find(b'+static void' + CR.encode() + LF.encode() + b'+ud_process_command', start)
assert start > 0 and end > start, (start, end)

def L(s):
    # s 为不含 CRLF 的一行（带 + 前缀），转成 CRLF 字节
    return (s + CR + LF).encode()

new_body = b''.join([
    L('+picasso_thread_dump(int pid)'),
    L('+{'),
    L('+' + TAB + 'proc_t p = proc_find(pid);'),
    L('+' + TAB + 'task_t task;'),
    L('+' + TAB + 'if (p == NULL) {'),
    L('+' + TAB*2 + 'printf("PICASSO_THREADS pid=%d absent' + chr(92) + 'n", pid);'),
    L('+' + TAB*2 + 'return;'),
    L('+' + TAB + '}'),
    L('+' + TAB + 'task = proc_task(p);'),
    L('+' + TAB + '/* Exported headers expose only forward declarations of struct task/thread,'),
    L('+' + TAB + ' * so per-thread fields (state/priority/saved_state/continuation) are not'),
    L('+' + TAB + ' * accessible in this translation unit. Emit a reachability marker instead:'),
    L('+' + TAB + ' * this still proves threads/kthreads is reachable in a STALL window. */'),
    L('+' + TAB + 'printf("PICASSO_THREADS pid=%d reachable proc=%p task=%p minimal' + chr(92) + 'n",'),
    L('+' + TAB*2 + 'pid, (void *)p, (void *)task);'),
    L('+' + TAB + 'printf("PICASSO_THREADS end pid=%d' + chr(92) + 'n", pid);'),
    L('+' + TAB + 'proc_rele(p);'),
    L('+}'),
    L('+'),
])

out = raw[:start] + new_body + raw[end:]
p.write_bytes(out)
print('source-line count in new body:', len(new_body.split(b'\r\n')) - 1)
print('CRLF total:', out.count(b'\r\n'), 'bare LF:', out.count(b'\n') - out.count(b'\r\n'))
i = out.find(b'reachability')
print('repr:', repr(out[i-40:i+160]))
