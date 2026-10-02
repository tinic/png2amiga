#!/usr/bin/env python3
"""Independently decode ILBM bitplanes and PCHG changes against the PNG preview."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
from PIL import Image


def replay(stem, dpf):
    data = stem.with_suffix('.iff').read_bytes()
    chunks = {}
    pos = 12
    while pos < len(data):
        name = data[pos:pos + 4]
        size = int.from_bytes(data[pos + 4:pos + 8], 'big')
        chunks[name] = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    header = chunks[b'BMHD']
    width, height = struct.unpack('>HH', header[:4])
    depth, compression = header[8], header[10]
    bpr = ((width + 15) // 16) * 2
    cmap = chunks[b'CMAP']
    palette = [tuple(cmap[i:i + 3]) for i in range(0, len(cmap), 3)]
    changes = chunks.get(b'PCHG', b'')
    flags = int.from_bytes(changes[2:4], 'big') if changes else 0
    mask = changes[20:20 + 4 * ((height + 31) // 32)] if changes else b''
    offset = 20 + len(mask)
    body, cursor = chunks[b'BODY'], 0
    pixels = []
    for y in range(height):
        if mask and mask[y // 8] & (128 >> (y % 8)):
            if flags == 1:
                low, high = changes[offset:offset + 2]
                offset += 2
                for base, count in ((0, low), (16, high)):
                    for _ in range(count):
                        value = int.from_bytes(changes[offset:offset + 2], 'big')
                        offset += 2
                        palette[base + (value >> 12)] = tuple(
                            ((value >> shift) & 15) * 17 for shift in (8, 4, 0))
            else:
                assert flags == 2
                count = int.from_bytes(changes[offset:offset + 2], 'big')
                offset += 2
                for _ in range(count):
                    reg, alpha, red, blue, green = struct.unpack(
                        '>HBBBB', changes[offset:offset + 6])
                    offset += 6
                    palette[reg] = (red, green, blue)
        rows = []
        for _ in range(depth):
            row = bytearray()
            while len(row) < bpr:
                if compression == 0:
                    row.extend(body[cursor:cursor + bpr])
                    cursor += bpr
                    break
                assert compression == 1
                count = body[cursor]
                cursor += 1
                if count < 128:
                    row.extend(body[cursor:cursor + count + 1])
                    cursor += count + 1
                elif count > 128:
                    row.extend([body[cursor]] * (257 - count))
                    cursor += 1
            assert len(row) == bpr
            rows.append(row)
        for x in range(width):
            planes = rows[1::2] if dpf else rows
            index = sum(((row[x // 8] >> (7 - x % 8)) & 1) << i
                        for i, row in enumerate(planes))
            if dpf and index:
                index += 1 << (depth // 2)
            pixels.append(palette[index])
    preview = Image.open(stem.with_suffix('.png')).convert('RGB')
    preview = preview.resize((width, height), Image.Resampling.NEAREST)
    expected = list(preview.getdata())
    mismatches = sum(a != b for a, b in zip(pixels, expected))
    assert mismatches == 0, f'{mismatches}/{len(pixels)} pixels differ'
    return flags


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--in', dest='source', required=True)
    parser.add_argument('--case', choices=('aga', 'ocs', 'dpf', 'dpf-sliced'), required=True)
    args = parser.parse_args()
    dpf = args.case.startswith('dpf')
    flags = ['--mode', 'lores', '--width', '320', '--height', '16']
    if dpf:
        flags += ['--dpf', '--no-lock-color0']
        if args.case == 'dpf-sliced':
            flags += ['--sliced']
    else:
        flags += ['--chipset', args.case, '--depth', '5', '--sliced']
    with tempfile.TemporaryDirectory() as directory:
        stem = Path(directory) / 'image'
        for suffix in ('.iff', '.png'):
            subprocess.run([args.cli, *flags, args.source, str(stem.with_suffix(suffix))],
                           check=True, capture_output=True)
        precision = replay(stem, dpf)
        if not dpf:
            assert precision == (2 if args.case == 'aga' else 1)
    print(f'PASS: {args.case} exported pixels match preview')


if __name__ == '__main__':
    main()
