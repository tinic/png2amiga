#!/usr/bin/env bash
# Bench PSNR for all examples × mode/depth × dither variants.
# Output: tab-separated rows of: image config dither psnr_db
# Configs cover modes affected by various quantizers.
set -e

BIN="${BIN:-./build/png2amiga}"
TMPDIR="${TMPDIR:-/tmp/p2a_bench}"
mkdir -p "$TMPDIR"

EXAMPLES=(electrichues02 fantasy fantasy1 grungy logo lovers photo space3)
DITHERS=(none floyd-steinberg)

# Configs: label, args
CONFIGS=(
  "ham6-ocs|--mode ham6 --chipset ocs"
  "ham8-aga|--mode ham8 --chipset aga"
  "lores-aga-d5|--mode lores --chipset aga --depth 5"
  "lores-ocs-d5|--mode lores --chipset ocs --depth 5"
)

for img in "${EXAMPLES[@]}"; do
  src=""
  for ext in jpg png; do
    [ -f "examples/${img}.${ext}" ] && src="examples/${img}.${ext}"
  done
  [ -z "$src" ] && { echo "skip $img" >&2; continue; }

  scaled="$TMPDIR/${img}_src.png"
  magick "$src" -resize 320x "$scaled" 2>/dev/null

  for cfg in "${CONFIGS[@]}"; do
    label="${cfg%%|*}"
    args="${cfg#*|}"
    for dith in "${DITHERS[@]}"; do
      out="$TMPDIR/${img}_${label}_${dith}.png"
      "$BIN" $args --dither "$dith" "$scaled" "$out" >/dev/null 2>&1 || {
        echo "FAIL $img $label $dith" >&2
        continue
      }
      psnr=$(magick compare -metric PSNR "$scaled" "$out" null: 2>&1 || true)
      printf "%s\t%s\t%s\t%s\n" "$img" "$label" "$dith" "$psnr"
    done
  done
done
