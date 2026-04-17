#!/usr/bin/env bash
# Bench PSNR for HAM modes across all examples × dither variants.
# Output: tab-separated rows of: image mode dither psnr_db
set -e

BIN="${BIN:-./build/png2amiga}"
TMPDIR="${TMPDIR:-/tmp/ham_bench}"
mkdir -p "$TMPDIR"

EXAMPLES=(electrichues02 fantasy fantasy1 grungy logo lovers photo space3)
MODES=(ham6 ham8)
DITHERS=(none floyd-steinberg)

# Default mode width 320 — preserve aspect ratio from source.
for img in "${EXAMPLES[@]}"; do
  src=""
  for ext in jpg png; do
    [ -f "examples/${img}.${ext}" ] && src="examples/${img}.${ext}"
  done
  [ -z "$src" ] && { echo "skip $img" >&2; continue; }

  # Pre-scale source to width 320 with proper aspect ratio (matches default mode width).
  scaled="$TMPDIR/${img}_src.png"
  magick "$src" -resize 320x "$scaled" 2>/dev/null

  for mode in "${MODES[@]}"; do
    for dith in "${DITHERS[@]}"; do
      out="$TMPDIR/${img}_${mode}_${dith}.png"
      "$BIN" --mode "$mode" --dither "$dith" "$scaled" "$out" >/dev/null 2>&1 || {
        echo "FAIL $img $mode $dith" >&2
        continue
      }
      # PSNR vs. scaled source (same dimensions).
      psnr=$(magick compare -metric PSNR "$scaled" "$out" null: 2>&1 || true)
      printf "%s\t%s\t%s\t%s\n" "$img" "$mode" "$dith" "$psnr"
    done
  done
done
