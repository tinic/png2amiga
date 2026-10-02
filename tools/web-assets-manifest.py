#!/usr/bin/env python3
"""Record/check the source and output hashes of a completed production web build."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / 'service/html/build-manifest.json'
INPUTS = ['src', 'web', 'cmake', 'third_party', 'CMakeLists.txt', '.gitmodules',
          'tools/build-web.sh', 'tools/web-assets-manifest.py']


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT)


def digest(entries):
    result = hashlib.sha256()
    for name, content in sorted(entries):
        result.update(name.encode() + b'\0')
        result.update(hashlib.sha256(content).digest())
    return result.hexdigest()


def source_hash():
    entries = []
    paths = set()
    for row in git('ls-files', '--stage', '-z', '--', *INPUTS).split(b'\0'):
        if not row:
            continue
        info, path = row.split(b'\t', 1)
        name = path.decode()
        mode = info.split()[0]
        if mode == b'160000':
            # Use the checked-out dependency revision, rather than a cached index.
            revision = git('-C', name, 'rev-parse', 'HEAD').strip()
            entries.append((name, revision))
        else:
            paths.add(name)
    paths.update(p.decode() for p in
                 git('ls-files', '--others', '--exclude-standard', '-z', '--', *INPUTS).split(b'\0')
                 if p)
    for name in paths:
        path = ROOT / name
        if path.is_file():
            entries.append((name, path.read_bytes()))
    return digest(entries)


def assets_hash():
    html = MANIFEST.parent
    if not (html / 'index.html').is_file() or not list((html / 'assets').glob('*.wasm')):
        raise ValueError('Production HTML/WASM assets are missing')
    return digest((p.relative_to(ROOT).as_posix(), p.read_bytes())
                  for p in html.rglob('*') if p.is_file() and p != MANIFEST)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['write', 'check'])
    args = parser.parse_args()
    try:
        current = {'format': 1, 'source_sha256': source_hash(), 'assets_sha256': assets_hash()}
        if args.action == 'write':
            MANIFEST.write_text(json.dumps(current, indent=2) + '\n')
            print('Recorded production web build: service/html/build-manifest.json')
        elif json.loads(MANIFEST.read_text()) != current:
            raise ValueError('Committed web assets do not match the current source/output')
        else:
            print('Committed web assets match the release source and output')
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'{error}. Run tools/build-web.sh and commit the compiled assets before tagging.',
              file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
