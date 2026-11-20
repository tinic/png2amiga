#!/usr/bin/env python3
"""Replay exported DPF/EHB/HAM6/lores bitplanes/COLOR writes and compare with the PNG preview.

Uses the established 320px OCS slot calibration, not a cycle-level emulator.
Checks export/preview agreement and the fixed copper instruction budget.
Lores additionally checks the black frame: COLOR00 is black at frame start,
in hblank and after x=319 on every line. --check-idx also writes -o .idx and
requires each byte to be the bitplane value the replay renders that pixel from.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from PIL import Image


SLOTS = (0, 8, 23, 40, 55, 71, 88, 104, 120, 135,
         152, 168, 184, 200, 215, 232, 247, 263, 280, 295)
LORES_SLOTS = tuple(range(0, 320, 8))


def words(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return [int(n, 16) for n in re.findall(r'0x([0-9a-fA-F]+)', text)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('dpf', 'ehb', 'ham6', 'lores'), default='dpf')
    parser.add_argument('--depth', type=int, choices=range(1, 5), default=3)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--in', dest='source', required=True)
    parser.add_argument('--expect-black-border', action='store_true')
    parser.add_argument('--height', type=int, default=200)
    parser.add_argument('--slot-shift', type=int, choices=range(-1, 2), default=0)
    parser.add_argument('--check-idx', action='store_true')
    parser.add_argument('extra', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    extra = args.extra[1:] if args.extra[:1] == ['--'] else args.extra
    is_ehb = args.mode == 'ehb'
    is_ham = args.mode == 'ham6'
    is_dpf = args.mode == 'dpf'
    is_lores = args.mode == 'lores'
    slots = {'ham6': tuple(range(7, 296, 16)), 'ehb': tuple(range(7, 280, 16)),
             'dpf': SLOTS, 'lores': LORES_SLOTS}[args.mode]
    # Lores colour 0 is unlocked with --no-lock-color0 and always at depth 1.
    frame_color0 = is_lores and ('--no-lock-color0' in extra or args.depth == 1)
    gate_hpos, hblank_budget = {'dpf': (0x38, 14),
                                'lores': (0x3C, 13 if frame_color0 else 14)}.get(args.mode,
                                                                                 (0x3C, 13))
    plane_count = args.depth if is_lores else 6
    with tempfile.TemporaryDirectory() as directory:
        header = Path(directory) / 'image.h'
        preview = Path(directory) / 'image.png'
        chunky = Path(directory) / 'image.idx'
        mode_flags = {'dpf': ['--mode', 'lores', '--dpf'],
                      'lores': ['--mode', 'lores', '--depth', str(args.depth)]}.get(
                          args.mode, ['--mode', args.mode])
        command = [args.cli, *mode_flags, '--strips',
                   '--width', '320', '--height', str(args.height), '--symbol', 'replay', *extra]
        outputs = (header, preview, chunky) if args.check_idx else (header, preview)
        for output in outputs:
            subprocess.run([*command, args.source, str(output)], check=True, capture_output=True)
        text = header.read_text()
        idx = chunky.read_bytes() if args.check_idx else None
        if idx is not None:
            assert len(idx) == 320 * args.height, f'.idx has {len(idx)} bytes'

        def array(name):
            return words(re.search(r'\b' + name + r'\[.*?\]\s*=\s*\{(.*?)\};',
                                   text, re.S).group(1))

        planes = [array(f'replay_plane{p}') for p in range(plane_count)]
        assert all(len(p) == 20 * args.height for p in planes), 'Unexpected bitplane dimensions'
        if is_dpf:
            assert not any(planes[0] + planes[2] + planes[4]), 'PF1 must remain empty'
        registers = (array('replay_palette') + [0] * 32)[:32]  # COLOR00..31
        if is_lores:
            assert registers[0] == 0, 'Frame must start with COLOR00 black'
        copper = re.search(r'replay_strips_copper_list\[.*?\]\s*=\s*\{(.*?)\};',
                           text, re.S).group(1)
        rows = [words(line) for line in copper.splitlines() if '/* y=' in line]
        assert len(rows) == args.height
        rendered = []
        for y, row in enumerate(rows):
            ops = list(zip(row[::2], row[1::2]))
            waits = [i for i, (address, _) in enumerate(ops) if address & 1]
            assert waits[0] <= hblank_budget, 'Hblank budget exceeded'
            assert ops[waits[0]] == ((((44 + y) & 255) << 8) | gate_hpos | 1, 0xFFFE)
            if is_lores:
                # No end-of-line WAIT; unlocked colour 0 ends with the restore.
                assert len(waits) == 1
                visible = ops[waits[0] + 1:]
                if frame_color0:
                    assert visible[-1] == (0x180, 0), 'Missing COLOR00 restore at x=320'
                    assert visible[0][0] == 0x180, 'Slot 0 must load COLOR00'
                    visible = visible[:-1]
            else:
                assert len(waits) == 2 and waits[-1] == len(ops) - 1
                expected_end = (((44 + y) & 255) << 8) | 0xE1
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
            if is_lores:
                assert registers[0] == 0, 'COLOR00 not black in the left border'
            slot = 0
            held = registers[0]
            # --slot-shift models a +/-1 px landing error on guarded slots.
            # Lores slot 0 is unguarded: it lands exactly at x=0.
            shifts = [0 if is_lores and s == 0 else args.slot_shift for s in range(len(slots))]
            for x in range(320):
                while slot < len(slots) and x >= max(0, slots[slot] + shifts[slot]):
                    move(visible[slot])
                    slot += 1
                word = y * 20 + x // 16
                shift = 15 - x % 16
                if idx is not None:
                    value = sum(((planes[bit][word] >> shift) & 1) << bit
                                for bit in range(plane_count))
                    assert idx[y * 320 + x] == value, f'.idx differs at ({x}, {y})'
                if is_ham:
                    value = sum(((planes[bit][word] >> shift) & 1) << bit for bit in range(6))
                    ctrl, data = value >> 4, value & 15
                    if ctrl == 0:
                        held = registers[data]
                    elif ctrl == 1:
                        held = (held & 0xFF0) | data
                    elif ctrl == 2:
                        held = (held & 0x0FF) | (data << 8)
                    else:
                        held = (held & 0xF0F) | (data << 4)
                    color = held
                elif is_lores:
                    index = sum(((planes[bit][word] >> shift) & 1) << bit
                                for bit in range(plane_count))
                    color = registers[index]
                elif is_ehb:
                    index = sum(((planes[bit][word] >> shift) & 1) << bit for bit in range(6))
                    color = registers[index & 31]
                    if index >= 32:
                        color = (color >> 1) & 0x777
                else:
                    index = sum(((planes[2 * bit + 1][word] >> shift) & 1) << bit
                                for bit in range(3))
                    color = registers[8 + index if index else 0]
                rendered.append(tuple(((color >> shift) & 15) * 17 for shift in (8, 4, 0)))
            if is_lores:
                if frame_color0:
                    move((0x180, 0))
                assert registers[0] == 0, 'COLOR00 not black in the right border'
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
              f'(slot shift {args.slot_shift:+d}); write budget unchanged'
              + ('; .idx matches' if idx is not None else ''))


if __name__ == '__main__':
    main()
