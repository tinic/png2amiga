#!/usr/bin/env bash
# Bench PSNR for copper mode at low bitplane depths.
set -e

BIN="${BIN:-./build/png2amiga}"
TMPDIR="${TMPDIR:-/tmp/copper_bench}"
mkdir -p "$TMPDIR"

EXAMPLES=(electrichues02 fantasy fantasy1 grungy logo lovers photo space3)
DITHERS=(none floyd-steinberg)
DEPTHS=(1 2 3 4)
CHIPSETS=(ocs aga)

for img in "${EXAMPLES[@]}"; do
  src=""
  for ext in jpg png; do
    [ -f "examples/${img}.${ext}" ] && src="examples/${img}.${ext}"
  done
  [ -z "$src" ] && continue

  scaled="$TMPDIR/${img}_src.png"
  magick "$src" -resize 320x "$scaled" 2>/dev/null

  for chip in "${CHIPSETS[@]}"; do
    for d in "${DEPTHS[@]}"; do
      for dith in "${DITHERS[@]}"; do
        out="$TMPDIR/${img}_cop_${chip}_d${d}_${dith}.png"
        "$BIN" --mode lores --copper --chipset "$chip" --depth "$d" --dither "$dith" \
          "$scaled" "$out" >/dev/null 2>&1 || { echo "FAIL $img $chip d$d $dith" >&2; continue; }
        psnr=$(magick compare -metric PSNR "$scaled" "$out" null: 2>&1 || true)
        printf "%s\tcop-%s-d%s\t%s\t%s\n" "$img" "$chip" "$d" "$dith" "$psnr"
      done
    done
  done
done
