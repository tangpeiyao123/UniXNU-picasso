#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host/CI verification for the step-2 pil_modem PIL skeleton.

Compiles the REAL state machine (PicassoWlanModemPil.cpp) with a deterministic
injected-boundary harness and runs every failure-injection mode. Separately
compiles the parameterised split-ELF load plan and, when the read-only firmware
directory is present, verifies it against the REAL modem.mdt + bXX directory
(no blobs embedded; sizes/SHA only). Source-text assertions pin the board
constants (PAS 4 / 0x8c000000 / 0xf800000) and forbid step-3/4 symbols.

NOT a hardware test: no SMC is issued, no MPSS is started. results.json records
hardware_tested=false honestly. Supports --zig for the Windows host (no clang).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

MODES = ['normal', 'no-handover', 'lease', 'lease-gpu', 'lease-adsp', 'poisoned',
         'vote-fail', 'aop-fail', 'off-fail', 'init-fail', 'setup-fail', 'auth-fail',
         'reset-fail', 'fw-missing', 'metadata-short', 'metadata-hash', 'stale-peer',
         'fatal-peer', 'pre-auth-peer', 'timeout', 'lost-ready', 'restart']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--firmware', type=Path, default=Path('D:/k30i-firmware/modem-image'))
    p.add_argument('--cxx', default=shutil.which('clang++'))
    p.add_argument('--zig', action='store_true', help='use "python -m ziglang c++"')
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cxx = ['python', '-m', 'ziglang', 'c++'] if a.zig else [a.cxx]
    if not a.zig and not a.cxx:
        raise SystemExit('no C++ compiler: pass --cxx or --zig')
    inc = [x for d in (ROOT / 'package', ROOT / 'drivers/NativeWlan', ROOT / 'scripts/tests')
           for x in ('-I', str(d))]
    ext = '.exe' if (not a.zig and __import__('os').name == 'nt') or (a.zig and __import__('os').name == 'nt') else ''
    runs = []

    def run(name, cmd):
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        (out / (name + '.log')).write_text(r.stdout + r.stderr, encoding='utf8', newline='\n')
        if r.returncode:
            raise RuntimeError(name + '\n' + r.stdout + r.stderr)
        runs.append(dict(name=name, passed=True))
        return r.stdout

    # 1) Source-text assertions: pin board constants, forbid step-3/4/5 symbols.
    impl = (ROOT / 'package/PicassoWlanModemPil.cpp').read_text(encoding='utf8')
    for needle in ['cfg->pasId = 4', 'cfg->carveoutBase = 0x8c000000',
                   'cfg->carveoutBytes = 0xf800000', 'cfg->metadataBase = 0x8bc18000',
                   'pas(io->context, 1, cfg->metadataBase', 'pas(io->context, 2, cfg->carveoutBase',
                   'pas(io->context, 5, 0, 0, cfg->pasId)']:
        assert needle in impl, 'missing pinned constant/behavior: ' + needle
    for forbidden in ['0x89200000', '0x02800000', 'glink', 'qmi', 'wlfw', 'ath10k', 'Pas id 1']:
        assert forbidden.lower() not in impl.lower(), 'forbidden symbol in skeleton: ' + forbidden

    # 2) Compile + run the real state machine harness across every mode.
    exe = out / ('pil-host' + ext)
    run('pil-compile', cxx + ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
        '-fno-exceptions', '-fno-rtti', *inc,
        str(ROOT / 'scripts/tests/picasso-wlan-modem-pil-host.cpp'),
        str(ROOT / 'package/PicassoWlanModemPil.cpp'), '-o', str(exe)])
    for mode in MODES:
        run('pil-' + mode, [str(exe), mode])

    # 3) Compile + run the parameterised load-plan harness against real firmware,
    #    if present. CI has no private blobs -> honest SKIP, but compile still runs.
    lp_src = ROOT / 'scripts/tests/picasso-wlan-modem-loadplan-host.cpp'
    lp_exe = out / ('loadplan' + ext)
    run('loadplan-compile', cxx + ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
        *inc, str(lp_src), '-o', str(lp_exe)])
    fw = a.firmware.resolve()
    present = fw.is_dir()
    if present:
        stdout = run('loadplan-real-fw', [str(lp_exe), str(fw)])
        print(stdout, end='')
    else:
        (out / 'loadplan-real-fw.log').write_text('SKIP: firmware dir absent: ' + str(fw) + '\n')
        print('SKIP: real firmware dir absent:', fw)

    inputs = [ROOT / 'package/PicassoWlanModemPil.h', ROOT / 'package/PicassoWlanModemPil.cpp',
              ROOT / 'package/PicassoWlanModemFirmwareProfile.h',
              ROOT / 'drivers/NativeWlan/WlanModemLoadPlan.h',
              ROOT / 'scripts/tests/picasso-wlan-modem-pil-host.cpp',
              ROOT / 'scripts/tests/picasso-wlan-modem-loadplan-host.cpp',
              ROOT / 'scripts/gen-picasso-wlan-modem-profile.py', Path(__file__)]
    (out / 'results.json').write_text(json.dumps(dict(
        passed=True, skipped=not present, hardware_tested=False, kernel_sdk_compiled=False,
        scope=__doc__, firmware=str(fw), firmware_present=present, runs=runs,
        note='offline state machine + load plan only; no SMC, no MPSS, no authentication',
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}),
        indent=2) + '\n', encoding='utf8', newline='\n')
    print('PASS pil_modem PIL skeleton: state machine modes + source pins' +
          (' + real firmware load plan' if present else ' (real firmware SKIP)'))


if __name__ == '__main__':
    main()
