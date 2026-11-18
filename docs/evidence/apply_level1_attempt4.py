from pathlib import Path
import re

CR, LF, TAB = chr(13), chr(10), chr(9)
p = Path(r'D:/k30i-gpu/repo-work/patches/picasso-usb-console-xnu.patch')
raw = p.read_bytes()
def L(s): return (s + CR + LF).encode()
def T(s): return L('+' + TAB + s)

# 定位注释起点到 printf 两行结束（逐线程输出块）
start = raw.find(b'+' + TAB.encode()*2 + b'/* threads[i] is a mach port identifier')
end_marker = b'+' + TAB.encode()*2 + b'    i, (void *)(uintptr_t)threads[i]);' + CR.encode() + LF.encode()
end = raw.find(end_marker, start)
assert start > 0 and end > start, (start, end)
end += len(end_marker)

new_block = (
    T(TAB + '/* idx is a thread ordinal (0..n-1), not a kernel tid;')
  + T(TAB + ' * stable only within one capture session. True tid/state/')
  + T(TAB + ' * continuation require a different dump path (later level). */')
  + T(TAB + 'printf("PICASSO_THREADS idx=%u' + chr(92) + 'n", i);')
)
raw = raw[:start] + new_block + raw[end:]

# 重算 count 行
lines = raw.split((CR+LF).encode())
n, in_hunk = 0, False
for ln in lines:
    if ln.startswith(b'@@ -0,0 +1,'):
        in_hunk = True; continue
    if in_hunk and ln.startswith(b'diff --git'): break
    if in_hunk and ln.startswith(b'+') and not ln.startswith(b'+++'): n += 1
old_m = re.search(rb'@@ -0,0 \+1,(\d+) @@', raw)
old_n = int(old_m.group(1))
raw = raw.replace(b'@@ -0,0 +1,' + str(old_n).encode() + b' @@',
                  b'@@ -0,0 +1,' + str(n).encode() + b' @@', 1)
p.write_bytes(raw)
import hashlib
print('count', old_n, '->', n)
print('CRLF', raw.count((CR+LF).encode()), 'bare LF', raw.count(b'\n')-raw.count((CR+LF).encode()), 'literal /r/n', raw.count(b'/r/n'))
print('sha256', hashlib.sha256(raw).hexdigest())
