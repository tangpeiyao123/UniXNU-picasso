#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host/CI verification for the step-4 SNOC/IRQ/DMA ownership skeleton.

Compiles the REAL state machine (PicassoWlanSnoc.cpp) with a deterministic
injected-boundary harness and runs every failure-injection mode. Source-text
checks pin the 12 first-hand SPI values and forbid fabricating a CE<->SPI map or
copying WCN6750/ath11k MSI/CE constants. scan_proven is always false.

NOT a hardware test: no MMIO, no real DMA, no IRQ line touched. results.json
records hardware_tested=false and scan_proven=false honestly. Supports --zig.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

MODES = [
    'normal', 'irq-claim-fail', 'irq-unclaimed', 'irq-poison', 'irq-stale-spi',
    'irq-ack-fail', 'irq-mask-fail', 'map-fail', 'unmap-fail',
    'cache-flush-missing', 'cache-invalidate-missing', 'ring-owner-conflict',
    'ring-head-overrun', 'iova-out-of-range', 'ring-count-zero',
    'worker-stall', 'worker-io-error', 'worker-batch-overflow',
    'ce-unresolved-after-resolve', 'resolved-ce-map', 'no-transport', 'restart',
]


def check(cond, msg):
    if not cond:  # raise, not assert: python -O cannot disable
        raise SystemExit('CHECK failed: ' + msg)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
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
    ext = '.exe' if __import__('os').name == 'nt' else ''
    runs = []

    def run(name, cmd):
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        (out / (name + '.log')).write_text(r.stdout + r.stderr, encoding='utf8', newline='\n')
        if r.returncode:
            raise RuntimeError(name + '\n' + r.stdout + r.stderr)
        runs.append(dict(name=name, passed=True))
        return r.stdout

    # 1) Source-text honesty checks (raise, not assert).
    hdr = (ROOT / 'package/PicassoWlanSnoc.h').read_text(encoding='utf8')
    impl = (ROOT / 'package/PicassoWlanSnoc.cpp').read_text(encoding='utf8')
    # Pin the 12 first-hand SPI values verbatim in the implementation.
    for v in ['0x084', '0x051', '0x19f', '0x1a0', '0x1a1', '0x056',
              '0x05e', '0x060', '0x052', '0x077', '0x078', '0x057']:
        check(v in impl, 'missing pinned first-hand SPI value: ' + v)
    for needle in ['cfg->iovaBase = 0xa0000000ULL', 'cfg->iovaSize = 0x10000000ULL',
                   'cfg->ceMap.provisional = true', 'state->scan_proven = false',
                   'state->hardwareTouched = false',
                   'state->scan_preconditions_ok = invariants && !cfg->ceMap.provisional']:
        check(needle in impl or needle in hdr, 'missing pinned honesty invariant: ' + needle)
    # Forbidden: WCN6750/ath11k constants, MSI, fabricated CE binding. Scan
    # only CODE (comments stripped): honest comments legitimately SAY "ath10k
    # format unknown here". Strip // and /* */ before scanning so a real code
    # constant cannot hide, without rejecting those honesty statements.
    import re
    code = re.sub(r'/\*.*?\*/', '', impl, flags=re.S)
    code = re.sub(r'//[^\n]*', '', code)
    for forbidden in ['0x1b80000', '0x17a10040', 'WCN6750', 'ath11k', 'ath10k',
                      'RADIO_MSI', '768', 'msi_']:
        check(forbidden.lower() not in code.lower(), 'forbidden symbol in skeleton code: ' + forbidden)

    # 2) Compile + run the real skeleton across every mode.
    exe = out / ('snoc-host' + ext)
    run('snoc-compile', cxx + ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
        '-fno-exceptions', '-fno-rtti', *inc,
        str(ROOT / 'scripts/tests/picasso-wlan-snoc-host.cpp'),
        str(ROOT / 'package/PicassoWlanSnoc.cpp'), '-o', str(exe)])
    for mode in MODES:
        run('snoc-' + mode, [str(exe), mode])

    inputs = [ROOT / 'package/PicassoWlanSnoc.h', ROOT / 'package/PicassoWlanSnoc.cpp',
              ROOT / 'scripts/tests/picasso-wlan-snoc-host.cpp', Path(__file__)]
    (out / 'results.json').write_text(json.dumps(dict(
        passed=True, skipped=False, hardware_tested=False, kernel_sdk_compiled=False,
        ce_map_provisional=True, scan_proven=False, scope=__doc__, runs=runs,
        note='offline injected SNOC/IRQ/DMA ownership only; no MMIO, no DMA, no real scan',
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}),
        indent=2) + '\n', encoding='utf8', newline='\n')
    print('PASS SNOC/IRQ/DMA skeleton: %d modes + 12-SPI source pins (scan_proven=false)' % len(MODES))


if __name__ == '__main__':
    main()
