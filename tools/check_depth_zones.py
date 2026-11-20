#!/usr/bin/env python3
"""Exact sliced planner and depth zones on generated pixel art.

Builds OCS-exact test images, runs the CLI with --sliced --dither none
--report-rows, and checks the rows reported 1:1, the depth zones picked,
and that every --output-zones IFF decodes to its PNG. Prints key=value
lines; exit status 0 on success.
"""
import argparse
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_amiga_export_replay import replay  # noqa: E402


def write_png(path, width, rows):
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
    raw = b''.join(b'\0' + b''.join(bytes(c) for c in row) for row in rows)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', width, len(rows), 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def ocs(rng, low=0):
    return tuple(rng.randrange(low, 16) * 17 for _ in range(3))


def bands(path):
    """Four 32-row bands of black + 7 random OCS colours."""
    rng = random.Random(3)
    rows = []
    for y in range(128):
        if y % 32 == 0:
            pal = [(0, 0, 0)] + [ocs(rng) for _ in range(7)]
        rows.append([pal[x * 8 // 320] for x in range(320)])
    write_png(path, 320, rows)


def zones(path):
    """Rows 0-63: 4 colours. 64-179: 24 colours, new set every 29 rows.
    180-255: 2 colours."""
    rng = random.Random(5)
    top = [(0, 0, 0), (0x22, 0x44, 0xaa), (0x44, 0x88, 0xee), (0xee, 0xee, 0xff)]
    rows = []
    for y in range(256):
        if y < 64:
            rows.append([top[(x // 20 + y // 16) % 4] for x in range(320)])
        elif y < 180:
            if (y - 64) % 29 == 0:
                mid = [(0, 0, 0)] + [ocs(rng, 1) for _ in range(23)]
            rows.append([mid[x * 24 // 320] for x in range(320)])
        else:
            rows.append([(0xcc, 0x66, 0) if (x // 8 + y // 8) % 2 else (0, 0, 0)
                         for x in range(320)])
    write_png(path, 320, rows)


def run(cli, args):
    proc = subprocess.run([cli, *args], capture_output=True, text=True)
    text = proc.stdout + proc.stderr
    if proc.returncode:
        raise SystemExit(f'error=cli exit {proc.returncode}: {text.strip()}')
    return text


def field(text, name):
    m = re.search(rf'^{name}:\s+(.*)$', text, re.M)
    return m.group(1).strip() if m else ''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', required=True)
    cli = parser.parse_args().cli
    failures = []

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        bands(tmp / 'bands.png')
        zones(tmp / 'zones.png')

        # Exact planner: 8 colours per band fit depth 3; 7 changes a line.
        text = run(cli, [str(tmp / 'bands.png'), '--mode', 'lores', '--depth', '3',
                         '--sliced', '--dither', 'none', '--report-rows',
                         '-o', str(tmp / 'b.png')])
        rows = field(text, 'Rows')
        print(f'bands_rows={rows!r}')
        if rows != 'all 128 rows 1:1':
            failures.append('bands not 1:1')

        # Depth zones picked from the image, written per zone.
        text = run(cli, [str(tmp / 'zones.png'), '--mode', 'lores', '--depth', 'auto',
                         '--sliced', '--dither', 'none', '--report-rows',
                         '--output-zones', str(tmp / 'oz'), '-o', str(tmp / 'z.h')])
        picked, rows = field(text, 'Zones'), field(text, 'Rows')
        print(f'zones_picked={picked!r}')
        print(f'zones_rows={rows!r}')
        if picked != '0-63 2bpl, 64-179 5bpl, 180-255 1bpl':
            failures.append('zones picked')
        # Rows 93 and 151 load 23 new colours at once: more than the line
        # budget plus the idle slots above can take.
        if rows != '2 of 256 rows not 1:1: 93, 151':
            failures.append('zone rows')
        header = (tmp / 'z.h').read_text()
        if 'const UWORD z_zone_depth[] = { 2, 5, 1 };' not in header:
            failures.append('.h zone table')
        for z in range(3):
            replay(tmp / f'oz_zone{z}', False)  # asserts IFF == PNG
        print('zone_iff_replay=ok')

    print(f'failures={len(failures)}')
    for f in failures:
        print(f'failure={f}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
