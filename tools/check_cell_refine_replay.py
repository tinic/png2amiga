#!/usr/bin/env python3
"""Replay refined native attribute-mode bytes and compare to the API preview."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
import numpy as np
from PIL import Image

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--api', required=True)
p.add_argument('--mode', required=True)
p.add_argument('--input', required=True)
p.add_argument('--baseline', action='store_true')
p.add_argument('--dither')
p.add_argument('--tile-budget', type=int, default=0)
p.add_argument('--tile-reserve', type=int, default=0)
p.add_argument('--graphics-only', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
palette_source = (root / 'src/palette.hpp').read_text()
with tempfile.TemporaryDirectory() as folder:
    raw, png = Path(folder) / 'frame.bin', Path(folder) / 'preview.png'
    metadata = Path(folder) / 'meta.json'
    subprocess.run([a.api, '--apply-tuning', '--mode', a.mode,
                    *([] if a.baseline else ['--cell-refine']),
                    *(['--dither', a.dither] if a.dither else []),
                    '--tile-budget', str(a.tile_budget), '--tile-reserve', str(a.tile_reserve),
                    *(['--c64-petscii-graphics'] if a.graphics_only else []), '--meta-out', str(metadata),
                    '--raw-out', str(raw), a.input, str(png)],
                   capture_output=True, text=True, check=True)
    data = raw.read_bytes()
    meta = json.loads(metadata.read_text())
    preview = np.asarray(Image.open(png).convert('RGB'))
    h, w = preview.shape[:2]
    decoded = np.empty_like(preview)
    if a.mode in ('thomson-to7-320x16', 'thomson-to8-320x16'):
        assert (w, h) == (320, 200) and len(data) == 16000
        intens = [0, 96, 124, 143, 159, 172, 183, 194, 203, 212, 220, 228, 235, 242, 248, 255]
        idx = [(0,0,0),(15,0,0),(0,15,0),(15,15,0),(0,0,15),(15,0,15),(0,15,15),(15,15,15),
               (7,7,7),(10,3,3),(3,10,3),(10,10,3),(3,3,10),(10,3,10),(7,14,14),(11,7,0)]
        pal = meta['palette'] if a.mode == 'thomson-to8-320x16' else [[intens[c] for c in rgb] for rgb in idx]
        for cell in range(8000):
            attr, mask = data[cell], data[cell + 8000]
            bg = (attr & 7) | (((~attr) >> 4) & 8)
            fg = ((attr >> 3) & 7) | (((~attr) >> 3) & 8)
            y, x = cell // 40, (cell % 40) * 8
            for bit in range(8):
                decoded[y, x + bit] = pal[fg if mask & (0x80 >> bit) else bg]
    elif a.mode.startswith('c64-'):
        mc = a.mode in ('c64-multicolor', 'c64-fli', 'c64-charset-multicolor')
        charset = a.mode.startswith('c64-charset-')
        pets = a.mode == 'c64-petscii'
        sliced = a.mode in ('c64-fli', 'c64-afli')
        cw = 4 if mc else 8
        cols, rows = w // cw, h // 8
        count = cols * rows
        bitmap_size = meta['glyphs'] * 8 if charset else (0 if pets else count * 8)
        screen_size = count * (8 if sliced else 1)
        color_size = count if mc or charset or pets else 0
        assert len(data) == bitmap_size + screen_size + color_size
        bitmap, screen = data[:bitmap_size], data[bitmap_size:bitmap_size+screen_size]
        cr = data[bitmap_size+screen_size:]
        if charset:
            assert 1 <= meta['glyphs'] <= 256 and max(screen) < meta['glyphs']
            if a.tile_budget:
                assert meta['glyphs'] <= max(1, a.tile_budget - a.tile_reserve)
            if mc:
                assert all(8 <= c <= 15 for c in cr)
        pal = meta['palette']
        font = []
        if pets:
            if a.graphics_only:
                assert all(c in (32,160) or 64 <= c <= 127 or 192 <= c <= 255 for c in screen)
            text = (root / 'src/petscii_rom.hpp').read_text().split('character_rom = {', 1)[1].split('};', 1)[0]
            font = [int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{2})',text)]
            assert len(font) == 2048
        for cell in range(count):
            for line in range(8):
                if charset or pets:
                    glyph = screen[cell]
                    bits = (font if pets else bitmap)[glyph*8+line]
                    slots = [meta['bg'], meta['mc1'], meta['mc2'], cr[cell]&7] if mc else [meta['bg'], cr[cell]&15]
                else:
                    bits = bitmap[cell*8+line]
                    attr = screen[(line*count if sliced else 0)+cell]
                    slots = [meta['bg'], attr>>4, attr&15, cr[cell]&15] if mc else [attr&15, attr>>4]
                for x in range(cw):
                    q = (bits >> ((cw-1-x)*(2 if mc else 1))) & (3 if mc else 1)
                    color = slots[q]
                    if sliced and cell%cols < 3:
                        assert color == 15
                    decoded[(cell//cols)*8+line, (cell%cols)*cw+x] = pal[color]
    elif a.mode.startswith('ted-'):
        mc = a.mode == 'ted-multicolor'
        cw = 4 if mc else 8
        assert len(data) == (10002 if mc else 10000)
        constants = palette_source.split('kTedPalette = {', 1)[1].split('};', 1)[0]
        pal = [[(v>>16)&255,(v>>8)&255,v&255] for v in
               [int(v,16) for v in re.findall(r'0x([0-9a-fA-F]{6})',constants)]]
        assert len(pal) == 128
        for cell in range(1000):
            luma, chroma = data[8000+cell], data[9000+cell]
            low, high = ((luma&7)<<4)|(chroma>>4), (luma&0x70)|(chroma&15)
            slots = [data[10000],low,high,data[10001]] if mc else [high,low]
            for line in range(8):
                bits = data[cell*8+line]
                for x in range(cw):
                    q = (bits >> ((cw-1-x)*(2 if mc else 1))) & (3 if mc else 1)
                    decoded[(cell//40)*8+line, (cell%40)*cw+x] = pal[slots[q]]
    else:
        cols, rows = map(int, re.fullmatch(r'cga-text(\d+)x(\d+)', a.mode).groups())
        assert w == cols * 8 and h == 200 and len(data) == cols * rows * 2
        cell_h = h // rows
        font = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})',
                (root / 'src/cga_font_data.inc').read_text().split('{', 1)[1])]
        assert len(font) == 2048
        constants = palette_source.split('kCgaHw = {', 1)[1].split('};', 1)[0]
        pal = [[(v >> 16) & 255, (v >> 8) & 255, v & 255]
               for v in [int(s, 16) for s in re.findall(r'0x([0-9A-Fa-f]{6})', constants)]]
        assert len(pal) == 16
        for cell in range(cols * rows):
            ch, attr = data[2 * cell:2 * cell + 2]
            x, y = (cell % cols) * 8, (cell // cols) * cell_h
            for line in range(cell_h):
                mask = font[ch * 8 + line]  # Hardware CRTC always starts at ROM row zero.
                for bit in range(8):
                    decoded[y + line, x + bit] = pal[(attr & 15) if mask & (0x80 >> bit) else attr >> 4]
    error = np.abs(decoded.astype(np.int16) - preview.astype(np.int16))
    if error.max() > 1:
        raise SystemExit(f'Replay mismatch: max channel error {error.max()}')
    print(f'PASS: {a.mode}, {len(data)} native bytes replay to the refined preview')
