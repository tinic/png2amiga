#!/usr/bin/env python3
"""Replay exported DPF/EHB bitplanes/COLOR writes and compare with the PNG preview.

Uses the established 320px OCS slot calibration, not a cycle-level emulator.
Checks export/preview agreement and the fixed copper instruction budget.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from PIL import Image


SLOTS = (0, 8, 23, 40, 55, 71, 88, 104, 120, 135,
         152, 168, 184, 200, 215, 232, 247, 263, 280, 295)


def words(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return [int(n, 16) for n in re.findall(r'0x([0-9a-fA-F]+)', text)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('dpf', 'ehb'), default='dpf')
    parser.add_argument('--cli', required=True)
    parser.add_argument('--in', dest='source', required=True)
    parser.add_argument('--expect-black-border', action='store_true')
    parser.add_argument('--height', type=int, default=200)
    parser.add_argument('--slot-shift', type=int, choices=range(-3, 4), default=0)
    parser.add_argument('extra', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    extra = args.extra[1:] if args.extra[:1] == ['--'] else args.extra
    is_ehb = args.mode == 'ehb'
    slots = tuple(range(9, 282, 16)) if is_ehb else SLOTS
    with tempfile.TemporaryDirectory() as directory:
        header = Path(directory) / 'image.h'
        preview = Path(directory) / 'image.png'
        mode_flags = ['--mode', 'ehb'] if is_ehb else ['--mode', 'lores', '--dpf']
        command = [args.cli, *mode_flags, '--strips',
                   '--width', '320', '--height', str(args.height), '--symbol', 'replay', *extra]
        for output in (header, preview):
            subprocess.run([*command, args.source, str(output)], check=True, capture_output=True)
        text = header.read_text()

        def array(name):
            return words(re.search(r'\b' + name + r'\[.*?\]\s*=\s*\{(.*?)\};',
                                   text, re.S).group(1))

        planes = [array(f'replay_plane{p}') for p in range(6)]
        assert all(len(p) == 20 * args.height for p in planes), 'Unexpected bitplane dimensions'
        if not is_ehb:
            assert not any(planes[0] + planes[2] + planes[4]), 'PF1 must remain empty'
        registers = array('replay_palette') + [0] * 16
        copper = re.search(r'replay_strips_copper_list\[.*?\]\s*=\s*\{(.*?)\};',
                           text, re.S).group(1)
        rows = [words(line) for line in copper.splitlines() if '/* y=' in line]
        assert len(rows) == args.height
        rendered = []
        for y, row in enumerate(rows):
            ops = list(zip(row[::2], row[1::2]))
            waits = [i for i, (address, _) in enumerate(ops) if address & 1]
            assert len(waits) == 2 and waits[-1] == len(ops) - 1
            assert waits[0] <= (13 if is_ehb else 14), 'Hblank budget exceeded'
            assert ops[waits[0]] == ((((44 + y) & 255) << 8) | (0x3D if is_ehb else 0x39), 0xFFFE)
            expected_end = (((44 + y) & 255) << 8) | (0xE1 if is_ehb else 0xDD)
            if 44 + y == 255 and 44 + args.height > 256:
                expected_end = max(expected_end, 0xFFDF)
            assert ops[-1] == (expected_end, 0xFFFE)
            visible = ops[waits[0] + 1:waits[1]]
            assert len(visible) == len(slots), 'Visible MOVE schedule changed'

            def move(op):
                address, color = op
                assert 0x180 <= address <= 0x1BE and not address & 1
                assert color <= 0xFFF, 'Color outside RGB444'
                registers[(address - 0x180) // 2] = color
                if args.expect_black_border:
                    assert registers[0] == 0, 'Copper changed the locked black border register'

            for op in ops[:waits[0]]:
                move(op)
            slot = 0
            for x in range(320):
                while slot < len(slots) and x >= max(0, slots[slot] + args.slot_shift):
                    move(visible[slot])
                    slot += 1
                word = y * 20 + x // 16
                shift = 15 - x % 16
                if is_ehb:
                    index = sum(((planes[bit][word] >> shift) & 1) << bit for bit in range(6))
                    color = registers[index & 31]
                    if index >= 32:
                        color = (color >> 1) & 0x777
                else:
                    index = sum(((planes[2 * bit + 1][word] >> shift) & 1) << bit
                                for bit in range(3))
                    color = registers[8 + index if index else 0]
                rendered.append(tuple(((color >> shift) & 15) * 17 for shift in (8, 4, 0)))
        with Image.open(preview) as image:
            assert image.width % 320 == 0 and image.height % args.height == 0
            sx, sy = image.width // 320, image.height // args.height
            raw = image.convert('RGB').tobytes()
            actual = list(zip(raw[::3], raw[1::3], raw[2::3]))
            # CLI PNG previews are integer-upscaled; require exact replication.
            rendered = [rendered[(y // sy) * 320 + x // sx]
                        for y in range(image.height) for x in range(image.width)]
        mismatches = sum(a != b for a, b in zip(actual, rendered))
        assert len(actual) == len(rendered)
        assert mismatches == 0, f'{mismatches} preview pixels differ from exported hardware colors'
        print(f'PASS ({args.mode}): {320 * args.height} pixels match exported bitplanes/copper '
              f'(slot shift {args.slot_shift:+d}); write budget unchanged')


if __name__ == '__main__':
    main()
