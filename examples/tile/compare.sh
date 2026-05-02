#!/usr/bin/env bash
# Run each tile demo source through both raw and --tile encoders, then
# stitch a 3x3 layout of each output side-by-side so you can eyeball
# whether seams disappear with --tile. Outputs land in `out/`.
#
# Usage: examples/tile/compare.sh [path/to/png2amiga]   (default: ./build/png2amiga)

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${1:-${HERE%/examples/tile}/build/png2amiga}"
OUT="${HERE}/out"
mkdir -p "${OUT}"

# Common encoder settings: indexed lores, depth 5, FS dither.
COMMON=(--mode lores --depth 5 --dither floyd-steinberg --width 128 --height 128)

for src in "${HERE}"/*.png; do
    name="$(basename "${src%.png}")"
    # Skip our own generated comparison artifacts.
    [[ "$name" == *_raw || "$name" == *_tile || "$name" == *_compare ]] && continue
    echo "=== ${name} ==="
    "${BIN}" "${COMMON[@]}"        "${src}" "${OUT}/${name}_raw.png"  > /dev/null
    "${BIN}" "${COMMON[@]}" --tile "${src}" "${OUT}/${name}_tile.png" > /dev/null
done

# Build a side-by-side per source: 3x3 layout for raw vs 3x3 layout
# for --tile, with a 4-pixel gap between them.
python3 - "${HERE}" "${OUT}" <<'PY'
import sys, glob, os
from PIL import Image, ImageDraw, ImageFont
HERE = sys.argv[1]
OUT  = sys.argv[2]

def tile3x3(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    big = Image.new("RGB", (w*3, h*3))
    for ty in range(3):
        for tx in range(3):
            big.paste(im, (tx*w, ty*h))
    return big

for src in sorted(glob.glob(os.path.join(HERE, "*.png"))):
    name = os.path.splitext(os.path.basename(src))[0]
    raw_path  = os.path.join(OUT, f"{name}_raw.png")
    tile_path = os.path.join(OUT, f"{name}_tile.png")
    if not (os.path.exists(raw_path) and os.path.exists(tile_path)):
        continue
    raw_g  = tile3x3(raw_path)
    tile_g = tile3x3(tile_path)
    gap = 16
    label_h = 24
    W, H = raw_g.size
    canvas = Image.new("RGB", (W*2 + gap, H + label_h*2 + gap), (24, 24, 28))
    d = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 18)
    except Exception:
        font = ImageFont.load_default()
    d.text((10, 4), f"{name}  —  raw (no --tile)", fill=(220, 220, 220), font=font)
    d.text((W + gap + 10, 4), f"{name}  —  --tile", fill=(80, 220, 130), font=font)
    canvas.paste(raw_g,  (0,        label_h))
    canvas.paste(tile_g, (W + gap,  label_h))
    d.text((10, label_h + H + 4), "look at the tile boundaries: vertical/horizontal", fill=(180, 180, 180), font=font)
    d.text((W + gap + 10, label_h + H + 4), "lines = seam, smooth continuation = clean.", fill=(180, 180, 180), font=font)
    out = os.path.join(OUT, f"{name}_compare.png")
    canvas.save(out)
    print(f"wrote {out}")
PY

echo
echo "Done. Inspect ${OUT}/<name>_compare.png for each source."
