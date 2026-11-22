#!/usr/bin/env bash
# Apple80211 layout ABI verifier — CI entry point.
# Compiles apple80211_layout.h's _Static_asserts with a REAL C compiler across
# three target ABIs. No XNU SDK / macOS SDK / device / network needed: uses
# zig cc, which ships its own libc headers + sysroot for every target. This is
# the key reason not to use apt clang here — system clang cross-compiling to
# Darwin fails on '#include <stdio.h>' without a macOS SDK.
set -euo pipefail
cd "$(dirname "$0")"

fail() { echo "ERROR: $*" >&2; exit 1; }

# zig via the pip package (portable, no PATH assumptions).
ZIG="python -m ziglang cc"
python -m ziglang version >/dev/null 2>&1 || fail "ziglang not installed (pip install ziglang)"
echo "== compiler: zig $(python -m ziglang version) =="

echo "== [native] compile + run (sizeof printout) =="
# No -target: zig uses the host's native ABI (x86_64-pc-windows-gnu on the
# local box, x86_64-linux-gnu on ubuntu CI) and ships its own libc sysroot.
$ZIG verify_layout.c -o verify_native -lc
./verify_native

# Compile-only: _Static_assert is a compile-time contract, no linking/SDK run
# needed. Covers arm64 Linux and Darwin arm64 (the 20H392 target arch).
for target in aarch64-linux-gnu aarch64-macos; do
    echo "== [_Static_assert] $target =="
    $ZIG -target "$target" -Wall -Wextra -c verify_layout.c -o "verify_${target}.o"
done

echo "ALL C LAYOUT CHECKS PASSED"
