#!/usr/bin/env python3
"""Real-firmware structural regression for NativeWlan. Compiles check-picasso-wlan-real-fw.c with clang/zig,
runs it against a read-only firmware directory (argv --firmware), and records results. No blob is embedded.
NOT a firmware authentication test: signed images are expected to require TrustZone PAS auth."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--firmware', type=Path, default=Path('D:/k30i-firmware/modem-image'))
    p.add_argument('--cc', default=shutil.which('clang'))
    p.add_argument('--zig', action='store_true', help='use "python -m ziglang cc" as the C compiler')
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    driver = ROOT / 'drivers/NativeWlan'
    probe = ROOT / 'scripts/check-picasso-wlan-real-fw.c'

    cc = ['python', '-m', 'ziglang', 'cc'] if args.zig else [args.cc]
    if not args.zig and not args.cc:
        raise SystemExit('no C compiler: pass --cc or --zig')

    def run(name, cmd):
        r = subprocess.run(cmd, capture_output=True, text=True)
        (out / (name + '.log')).write_text(r.stdout + r.stderr)
        if r.returncode:
            raise RuntimeError(name + ': ' + r.stdout + r.stderr)
        return r.stdout

    flags = ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(driver)]
    if os.name == 'nt':
        flags += ['-D_CRT_SECURE_NO_WARNINGS']
    obj = out / 'real_fw.o'
    exe = out / ('real_fw.exe' if os.name == 'nt' else 'real_fw')
    run('compile_fw', cc + flags + ['-c', str(driver / 'WlanFirmware.c'), '-o', str(obj)])
    run('compile_probe', cc + flags + ['-c', str(probe), '-o', str(out / 'probe.o')])
    if args.zig:
        cxx = ['python', '-m', 'ziglang', 'c++']
        run('link', cxx + [str(out / 'probe.o'), str(obj), '-o', str(exe)])
    else:
        run('link', [args.cc, str(out / 'probe.o'), str(obj), '-o', str(exe)])

    stdout = ''
    fw = args.firmware.resolve()
    present = fw.is_dir()
    if present:
        stdout = run('run', [str(exe), str(fw)])
        print(stdout, end='')
    else:
        (out / 'run.log').write_text('SKIP: firmware directory not present: ' + str(fw) + '\n')
        print('SKIP: firmware directory not present:', fw)

    inputs = [driver / 'WlanFirmware.c', driver / 'WlanFirmware.h', driver / 'WlanProbe.cpp',
              driver / 'Test/main.c', probe, Path(__file__)]
    # A missing firmware directory means the real-firmware assertions never ran: record that honestly rather than
    # reporting passed=True. The compile/link steps still passed, so distinguish 'skipped' from a hard failure.
    (out / 'results.json').write_text(json.dumps(dict(
        passed=present, skipped=not present, scope=__doc__, firmware=str(fw), firmware_present=present,
        note='structural only; signed images require TrustZone PAS auth and are NOT authenticated here',
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}), indent=2) + '\n')
    if present:
        print('PASS: real-firmware structural regression (no authentication performed)')
    else:
        print('SKIP: no real firmware present; structural assertions were NOT run')


if __name__ == '__main__':
    main()
