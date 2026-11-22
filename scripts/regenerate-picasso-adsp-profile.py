#!/usr/bin/env python3
"""Regenerate package/PicassoAdspFirmwareProfile.h from the board firmware image.

Reads the device modem.img, runs the same Fat16/Hexagon-ELF/carveout pipeline as
picasso_adsp_firmware.prepare(), and rewrites the profile header. Before writing
anything it re-derives the expected profile and checks the update shape exactly:
only entries whose (bytes, sha256) differ get rewritten; everything else must be
byte-identical to the existing file. A single transcribed hex digit off by one
would repeat the -8 firmware mismatch, so any mismatch aborts without writing.
"""
import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import picasso_adsp_firmware as fw  # noqa: E402

PROFILE = ROOT / 'package' / 'PicassoAdspFirmwareProfile.h'


def load_profile():
    """Parse the existing profile into {path: (bytes, sha_hex)} preserving order."""
    text = PROFILE.read_text(encoding='utf-8')
    pattern = re.compile(r'\{"image/([a-z0-9.]+)",\s*(\d+),\s*\{([^}]*)\}\}')
    entries, order = {}, []
    for match in pattern.finditer(text):
        name, size, blob = match.groups()
        sha = ''.join(f'{int(x, 16):02x}' for x in blob.split(','))
        entries[f'image/{name}'] = (int(size), sha)
        order.append(f'image/{name}')
    return text, entries, order


def line_for(name, size, sha):
    hex_list = ', '.join(f'0x{sha[i:i+2]}' for i in range(0, 64, 2))
    return f'    {{"{name}", {size}, {{{hex_list}}}}},'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', default='D:/k30i-firmware/modem.img')
    parser.add_argument('--write', action='store_true', help='Apply (default: verify only)')
    args = parser.parse_args()

    image, files, report = fw.prepare(args.source)
    generated = {f'image/{name}': (len(data), fw.digest(data)) for name, data in sorted(files.items())}
    print(f"GENERATED entries={len(generated)} image_bytes={report['image_bytes']} "
          f"metadata_bytes={report['metadata_bytes']}")

    text, existing, order = load_profile()
    if len(existing) != len(generated):
        raise SystemExit(f'PROFILE_COUNT_MISMATCH existing={len(existing)} generated={len(generated)}')
    if order != sorted(generated):
        raise SystemExit('PROFILE_ORDER_MISMATCH: generated keys differ from profile order')

    updated, changed_sizes = [], {}
    for name in order:
        new_size, new_sha = generated[name]
        old_size, old_sha = existing[name]
        if (new_size, new_sha) == (old_size, old_sha):
            continue
        updated.append(name)
        if new_size != old_size:
            changed_sizes[name] = (old_size, new_size)

    print(f"UPDATED entries={len(updated)}: {', '.join(updated)}")
    for name, (old, new) in changed_sizes.items():
        print(f"SIZE CHANGE {name}: {old} -> {new} ({new - old:+d})")

    # Reviewer-pinned update shape: b04 grows by exactly one 32-byte record.
    expected_sizes = {'image/adsp.b04': (1997228, 1997260)}
    for name, (old, new) in expected_sizes.items():
        got = changed_sizes.get(name)
        if got != (old, new):
            raise SystemExit(f'SIZE_SHAPE_MISMATCH {name}: expected {old}->{new}, got {got}')
    unexpected_sizes = {n: v for n, v in changed_sizes.items() if n not in expected_sizes}
    if unexpected_sizes:
        raise SystemExit(f'UNEXPECTED_SIZE_CHANGES: {unexpected_sizes}')

    # b00 keeps its size; its new hash is pinned by the review.
    pinned = {'image/adsp.b00': (1140, '94241d41a3a0b24c7a929cc28b3117c5441003304e8b66f63d142b4522c95e55')}
    for name, (size, sha) in pinned.items():
        if generated[name] != (size, sha):
            raise SystemExit(f'PINNED_HASH_MISMATCH {name}: got {generated[name]}, expected {(size, sha)}')

    new_lines = {name: line_for(name, *generated[name]) for name in updated}
    out_lines = []
    for line in text.splitlines():
        stripped = line.strip()
        match = re.match(r'\{"image/([a-z0-9.]+)",', stripped)
        if match:
            name = f'image/{match.group(1)}'
            out_lines.append(new_lines.get(name, line))
        else:
            out_lines.append(line)
    result = '\n'.join(out_lines) + '\n'

    if not args.write:
        print('DRY_RUN_OK (use --write to apply)')
        return
    PROFILE.write_text(result, encoding='utf-8', newline='\n')
    print(f'WROTE {PROFILE}')
    # Re-read and confirm the written file parses back to exactly `generated`.
    _, written, written_order = load_profile()
    if written != generated or written_order != order:
        raise SystemExit('WRITE_ROUNDTRIP_MISMATCH')
    print('WRITE_ROUNDTRIP_OK')


if __name__ == '__main__':
    main()
