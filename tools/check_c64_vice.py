#!/usr/bin/env python3
"""Compare C64 previews with PAL VICE VIC-II pixels (requires x64sc and ROMs).

Bitmap/PETSCII tests execute the actual exported PRG. Charset PRG export is
not supported, so those tests load the exported native bytes using a minimal
standard text-mode test program, with no raster interrupts or extra colors.
"""
import argparse
import json
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image

MODES = ['c64-hires', 'c64-multicolor', 'c64-fli', 'c64-afli',
         'c64-petscii', 'c64-charset-hires', 'c64-charset-multicolor']


def charset_prg(raw, meta, multicolor):
    glyph_bytes = meta['glyphs'] * 8
    assert len(raw) == glyph_bytes + 2000 and glyph_bytes <= 2048
    memory = bytearray(0x3000)
    # BASIC 10 SYS2061, followed by code at $080d.
    memory[0x801:0x80d] = bytes.fromhex('0b080a009e32303631000000')
    code = bytearray.fromhex('78a9358501')  # SEI; map RAM and I/O
    def reg(address, value):
        code.extend([0xa9, value, 0x8d, address & 255, address >> 8])
    reg(0xdd00, 3)  # VIC bank $0000
    reg(0xd015, 0)
    reg(0xd020, 0)
    reg(0xd021, meta['bg'])
    reg(0xd022, meta['mc1'])
    reg(0xd023, meta['mc2'])
    reg(0xd011, 0x1b)
    reg(0xd016, 0x18 if multicolor else 8)
    reg(0xd018, 0x18)  # screen $0400, writable glyphs $2000
    code.extend([0xa2, 0])
    loop = len(code)
    for source, target in ((0x2800, 0x0400), (0x2c00, 0xd800)):
        for page in range(4):
            code.extend([0xbd, 0, (source >> 8) + page,
                         0x9d, 0, (target >> 8) + page])
    code.extend([0xe8, 0xd0, (loop - len(code) - 3) & 255])
    spin = 0x80d + len(code)
    code.extend([0x4c, spin & 255, spin >> 8])
    memory[0x80d:0x80d+len(code)] = code
    memory[0x2000:0x2000+glyph_bytes] = raw[:glyph_bytes]
    memory[0x2800:0x2800+1000] = raw[glyph_bytes:glyph_bytes+1000]
    memory[0x2c00:0x2c00+1000] = raw[glyph_bytes+1000:]
    return bytes([1, 8]) + memory[0x801:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cli', type=Path, default=Path('build/png2amiga'))
    p.add_argument('--api', type=Path, default=Path('build/api_pipeline_smoke'))
    p.add_argument('--vice', default='x64sc')
    p.add_argument('--input', type=Path, default=Path('examples/maui.jpg'))
    p.add_argument('--out', type=Path, default=Path('build/cell-refine-extended/vice'))
    p.add_argument('--modes', nargs='+', choices=MODES, default=MODES)
    p.add_argument('--dither', help='Override the initial dither for both variants')
    p.add_argument('--model', choices=['c64', 'c64c'], default='c64')
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    results = []
    for mode in args.modes:
        for refined in (False, True):
            stem = f'{mode}-{"refined" if refined else "default"}'
            base = args.out / stem
            flags = (['--mode', mode] + (['--cell-refine'] if refined else [])
                     + (['--dither', args.dither] if args.dither else []))
            def run(command, suffix):
                r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=120)
                base.with_suffix(suffix).write_text(r.stdout + r.stderr)
                return r
            r = run([args.api.resolve(), '--apply-tuning', *flags,
                     '--raw-out', base.with_suffix('.bin'), '--meta-out', base.with_suffix('.json'),
                     args.input, base.with_suffix('.png')], '.api.log')
            assert r.returncode == 0, r.stderr
            meta = json.loads(base.with_suffix('.json').read_text())
            if mode.startswith('c64-charset-'):
                base.with_suffix('.prg').write_bytes(charset_prg(
                    base.with_suffix('.bin').read_bytes(), meta, mode.endswith('multicolor')))
            else:
                r = run([args.cli.resolve(), *flags, args.input, base.with_suffix('.prg')], '.cli.log')
                assert r.returncode == 0, r.stderr
            palette = base.with_suffix('.vpl')
            palette.write_text('\n'.join(' '.join(f'{c:02x}' for c in rgb) + ' 0'
                                         for rgb in meta['palette']) + '\n')
            screenshot = base.with_suffix('.vice.png')
            screenshot.unlink(missing_ok=True)
            r = run([args.vice, '-default', '-console', '+sound', '-warp', '-model', args.model,
                     '-VICIIfilter', '0', '-VICIIextpal', '-VICIIpalette', palette.resolve(),
                     '-VICIIgamma', '1000', '-VICIIcontrast', '1000', '-VICIIbrightness', '1000',
                     '-VICIIsaturation', '1000', '-VICIItint', '1000',
                     '-autostartprgmode', '1', '-autostart', base.with_suffix('.prg').resolve(),
                     '-limitcycles', '12000000', '-exitscreenshot', screenshot.resolve()], '.vice.log')
            # VICE exits with status 1 when the intentional cycle limit is reached.
            assert 'cycle limit reached' in r.stdout + r.stderr and screenshot.exists(), r.stderr
            actual = np.asarray(Image.open(screenshot).convert('RGB')).astype(int)
            expected = np.asarray(Image.open(base.with_suffix('.png')).convert('RGB')).astype(int)
            if expected.shape[1] == 160:
                expected = np.repeat(expected, 2, axis=1)
            assert expected.shape == (200, 320, 3)
            # FLI is vertically shifted by its border-opening routine. Locate
            # the full 320x200 area; never mask or discard its problematic edge.
            candidates = []
            for y in range(actual.shape[0] - 199):
                crop = actual[y:y+200, 32:352]
                mismatches = int(np.any(np.abs(crop - expected) > 1, axis=2).sum())
                candidates.append((mismatches, y))
            mismatches, y = min(candidates)
            row = dict(mode=mode, refined=refined, model=args.model, dither=args.dither,
                       mismatched_pixels=mismatches, crop=[32, y, 320, 200])
            results.append(row)
            (args.out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(f'{stem}: {mismatches} mismatched pixels at (32,{y})', flush=True)
            assert mismatches == 0, row


if __name__ == '__main__':
    main()
