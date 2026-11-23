#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host/CI verification for the step-3 WLFW-over-QMI injected transport skeleton.

Compiles the REAL state machine (PicassoWlanWlfw.cpp) with a deterministic
injected-boundary harness and runs every failure-injection mode. Source-text
checks pin the honesty contract: no WCN6750/ath11k service id, no ADSP servreg
special-case, no fabricated identifiers, hardware_tested=false.

NOT a hardware test: no QRTR/QMI packet is sent to a device, no MPSS is started,
no MMIO is written. results.json records hardware_tested=false honestly.
Supports --zig for the Windows host (no clang).
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

MODES = [
    'normal', 'discovery-no-answer', 'discovery-timeout', 'wrong-service',
    'wrong-instance', 'wrong-node-port', 'fabricated-provisional', 'presence-missing',
    'cap-fail', 'config-fail', 'indreg-fail', 'ini-fail', 'msa-info-fail', 'mode-fail',
    'retransmit-exhausted', 'fifo-full', 'kick-fail', 'fw-ready-timeout',
    'msa-ready-timeout', 'stale-indication', 'tlv-overrun', 'fw-ready-malformed-tlv',
    'msa-misaligned', 'no-transport', 'restart',
]


def check(cond, msg):
    # Explicit raise (not assert) so python -O cannot disable the guard.
    if not cond:
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
    hdr = (ROOT / 'package/PicassoWlanWlfw.h').read_text(encoding='utf8')
    impl = (ROOT / 'package/PicassoWlanWlfw.cpp').read_text(encoding='utf8')
    for needle in ['cfg->service = 0; cfg->instance = 0', 'cfg->provisional = true',
                   '0x8ba00000ULL', '0x200000ULL', '0xa0000000ULL', '0x10000000ULL',
                   'PICASSO_WLAN_WLFW_SID       0xc0u', 'PICASSO_WLAN_WLFW_SID_IPA   0x521u']:
        check(needle in hdr or needle in impl, 'missing pinned honesty constant: ' + needle)
    # Forbid fabricating a Picasso service id from the WCN6750 reference and
    # forbid reusing the ADSP servreg special-case in the implementation body.
    for forbidden in ['ATH11K_QMI_WLFW_SERVICE_ID_V01', 'WCN6750', 'service==0x42',
                      '0x89200000', '0x02800000', 'ath10k']:
        check(forbidden.lower() not in impl.lower(), 'forbidden symbol in skeleton: ' + forbidden)
    check('svc.service != cfg->service || svc.instance != cfg->instance' in impl,
          'missing four-dimensional discovery match (modem binding)')
    check('cfg->provisional || unresolved' in impl, 'missing fabrication guard')
    check('hardwareTouched = false' in impl, 'missing hardwareTouched=false invariant')

    # 2) Compile + run the real skeleton across every mode.
    exe = out / ('wlfw-host' + ext)
    run('wlfw-compile', cxx + ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
        '-fno-exceptions', '-fno-rtti', *inc,
        str(ROOT / 'scripts/tests/picasso-wlan-wlfw-host.cpp'),
        str(ROOT / 'package/PicassoWlanWlfw.cpp'), '-o', str(exe)])
    for mode in MODES:
        run('wlfw-' + mode, [str(exe), mode])

    inputs = [ROOT / 'package/PicassoWlanWlfw.h', ROOT / 'package/PicassoWlanWlfw.cpp',
              ROOT / 'scripts/tests/picasso-wlan-wlfw-host.cpp', Path(__file__)]
    (out / 'results.json').write_text(json.dumps(dict(
        passed=True, skipped=False, hardware_tested=False, kernel_sdk_compiled=False,
        service_id_provisional=True, scope=__doc__, runs=runs,
        note='offline injected WLFW/QMI transport only; no QRTR/QMI to device, no MPSS, no MMIO',
        inputs={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}),
        indent=2) + '\n', encoding='utf8', newline='\n')
    print('PASS WLFW/QMI skeleton: %d modes + source honesty pins' % len(MODES))


if __name__ == '__main__':
    main()
