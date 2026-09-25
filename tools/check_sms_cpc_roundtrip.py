#!/usr/bin/env python3
"""Decode png2amiga's Master System / Game Gear / Amstrad CPC raw output with
an independent decoder and require a bit-exact match with the preview.

    check_sms_cpc_roundtrip.py --smoke <api_pipeline_smoke> --mode <mode>
                               --input <image> --work <dir>

The decoder here is written from the hardware documentation, not from the
encoder:
  SMS/GG  tiles: 32 bytes, byte (row*4 + plane), bit 7 = leftmost pixel.
          tilemap u16 LE: bits 0-8 tile, 9 hflip, 10 vflip, 11 palette.
          CRAM: SMS --BBGGRR bytes; GG u16 LE ----BBBBGGGGRRRR.
  CPC     line y at (y//8)*80 + (y%8)*2048; pen bits per the Gate Array
          (MAME amstrad_init_lookups); inks from the companion .pal:
          classic = 0x40|hardware number, Plus = (R<<4|B, G).
"""
import argparse
import subprocess
import sys
from pathlib import Path

from PIL import Image

# Gate Array hardware color number -> (r, g, b) levels 0/1/2 (MAME
# amstrad_palette[] order: white, white, sea green, pastel yellow, ...).
HW_LEVELS = [
    (1, 1, 1), (1, 1, 1), (0, 2, 1), (2, 2, 1), (0, 0, 1), (2, 0, 1), (0, 1, 1), (2, 1, 1),
    (2, 0, 1), (2, 2, 1), (2, 2, 0), (2, 2, 2), (2, 0, 0), (2, 0, 2), (2, 1, 0), (2, 1, 2),
    (0, 0, 1), (0, 2, 1), (0, 2, 0), (0, 2, 2), (0, 0, 0), (0, 0, 2), (0, 1, 0), (0, 1, 2),
    (1, 0, 1), (1, 2, 1), (1, 2, 0), (1, 2, 2), (1, 0, 0), (1, 0, 2), (1, 1, 0), (1, 1, 2),
]
LEVEL = [0x00, 0x80, 0xFF]

SMS_GEOM = {"sms-mode4": (32, 24, 32), "gg-mode4": (20, 18, 64)}
CPC_MODE = {
    "cpc-mode0": 0, "cpc-mode1": 1, "cpc-mode2": 2,
    "cpc-plus-mode0": 0, "cpc-plus-mode1": 1, "cpc-plus-mode2": 2,
}
# [pixel][pen bit] -> byte bit
CPC_BITS = {
    0: [[7, 3, 5, 1], [6, 2, 4, 0]],
    1: [[7, 3], [6, 2], [5, 1], [4, 0]],
    2: [[7 - n] for n in range(8)],
}


def decode_sms(mode, raw):
    cols, rows, cram_len = SMS_GEOM[mode]
    cells = cols * rows
    cram = raw[-cram_len:]
    tilemap = raw[-cram_len - cells * 2:-cram_len]
    tiles = raw[:-cram_len - cells * 2]
    assert len(tiles) % 32 == 0, "tile block not a multiple of 32"
    ntiles = len(tiles) // 32
    assert ntiles <= 448, f"{ntiles} tiles exceed the 448-tile VRAM budget"
    pal = []
    for i in range(32):
        if mode == "gg-mode4":
            w = cram[i * 2] | (cram[i * 2 + 1] << 8)
            pal.append(((w & 15) * 17, ((w >> 4) & 15) * 17, ((w >> 8) & 15) * 17))
        else:
            v = cram[i]
            pal.append(((v & 3) * 0x55, ((v >> 2) & 3) * 0x55, ((v >> 4) & 3) * 0x55))
    img = Image.new("RGB", (cols * 8, rows * 8))
    px = img.load()
    for cy in range(rows):
        for cx in range(cols):
            e = tilemap[(cy * cols + cx) * 2] | (tilemap[(cy * cols + cx) * 2 + 1] << 8)
            t, hf, vf, p = e & 0x1FF, (e >> 9) & 1, (e >> 10) & 1, (e >> 11) & 1
            assert t < ntiles, f"tilemap references tile {t} of {ntiles}"
            for y in range(8):
                row = 7 - y if vf else y
                planes = tiles[t * 32 + row * 4: t * 32 + row * 4 + 4]
                for x in range(8):
                    col = 7 - x if hf else x
                    idx = sum(((planes[b] >> (7 - col)) & 1) << b for b in range(4))
                    px[cx * 8 + x, cy * 8 + y] = pal[p * 16 + idx]
    return img


def decode_cpc(mode, screen, pal):
    assert len(screen) == 16384, f"screen is {len(screen)} bytes"
    m = CPC_MODE[mode]
    bits = CPC_BITS[m]
    ppb = len(bits)
    if mode.startswith("cpc-plus"):
        inks = [((pal[i * 2] >> 4) * 17, (pal[i * 2 + 1] & 15) * 17, (pal[i * 2] & 15) * 17)
                for i in range(len(pal) // 2)]
    else:
        inks = []
        for b in pal:
            assert 0x40 <= b <= 0x5F, f"bad Gate Array color byte {b:#x}"
            r, g, bl = HW_LEVELS[b & 0x1F]
            inks.append((LEVEL[r], LEVEL[g], LEVEL[bl]))
    w = 80 * ppb
    img = Image.new("RGB", (w, 200))
    px = img.load()
    for y in range(200):
        base = (y // 8) * 80 + (y % 8) * 2048
        for xb in range(80):
            byte = screen[base + xb]
            for p in range(ppb):
                pen = sum(((byte >> bits[p][k]) & 1) << k for k in range(len(bits[p])))
                px[xb * ppb + p, y] = inks[pen]
    # The 48 bytes after each 2 KB block's 25 lines must stay zero.
    for blk in range(8):
        tail = screen[blk * 2048 + 2000: blk * 2048 + 2048]
        assert not any(tail), f"gap bytes in block {blk} are not zero"
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--smoke", required=True)
    ap.add_argument("--mode", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--work", required=True)
    args = ap.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    png = work / f"{args.mode}.png"
    raw = work / f"{args.mode}.raw"
    pal = work / f"{args.mode}.pal"
    cmd = [args.smoke, "--mode", args.mode, "--raw-out", str(raw), "--pal-out", str(pal),
           args.input, str(png)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        return 1
    data = raw.read_bytes()
    if args.mode in SMS_GEOM:
        dec = decode_sms(args.mode, data)
    else:
        dec = decode_cpc(args.mode, data, pal.read_bytes())
    ref = Image.open(png).convert("RGB")
    if ref.size != dec.size:
        print(f"size mismatch: preview {ref.size} vs decoded {dec.size}")
        return 1
    a, b = ref.tobytes(), dec.tobytes()
    diff = sum(1 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3])
    if diff:
        print(f"{args.mode}: {diff} pixels differ between preview and decoded raw")
        return 1
    print(f"{args.mode}: decoded raw matches preview ({dec.size[0]}x{dec.size[1]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
