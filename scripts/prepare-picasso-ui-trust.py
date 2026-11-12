#!/usr/bin/env python3
"""Add the compiled status bar library to the original trust bundle.

The bundle always trusts the freshly built library. --compat-file additionally
keeps the CDHashes of libraries that already live in the device's persistent
Data seed, so an image stays valid whether Compose reuses that seed or formats a
new one. See configs/picasso-trust-compat.json.
"""
import argparse
import json
from pathlib import Path
from picasso_ui_trust import load_compat_cdhashes, make_bundle


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compat-file', type=Path,
                        help='JSON list of already-installed library CDHashes to keep trusted')
    parser.add_argument('--compat-cdhash', action='append', default=[],
                        help='Repeatable single CDHash, merged with --compat-file')
    args = parser.parse_args()
    compat = load_compat_cdhashes(args.compat_file) + list(args.compat_cdhash)
    bundle, report = make_bundle(args.original.read_bytes(), args.library.read_bytes(), compat)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bundle)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: unchanged Apple modules plus one exact UI library CDHash', report['library']['cdhash'],
          'and', len(report['compat_cdhashes']), 'installed compat CDHash(es)')


if __name__ == '__main__':
    main()
