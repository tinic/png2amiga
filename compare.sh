#!/usr/bin/env bash
# Compare png2amiga vs abc output for all example images
# Usage: ./compare.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ABC="/tmp/abc/abc2"
TMPDIR="/tmp/compare"
OUTDIR="$SCRIPT_DIR/compare"

mkdir -p "$TMPDIR" "$OUTDIR"

declare -A sizes
sizes[electrichues02]="320x160"
sizes[fantasy]="320x214"
sizes[lovers]="320x180"
sizes[space3]="320x200"

for img in electrichues02 fantasy lovers space3; do
  src=""
  [ -f "$SCRIPT_DIR/examples/${img}.jpg" ] && src="$SCRIPT_DIR/examples/${img}.jpg"
  [ -f "$SCRIPT_DIR/examples/${img}.png" ] && src="$SCRIPT_DIR/examples/${img}.png"

  dims="${sizes[$img]}"
  w="${dims%x*}"
  h="${dims#*x}"

  echo "=== $img (${w}x${h}) ==="

  # Scale input with ImageMagick (same for both tools)
  magick "$src" -resize ${w}x${h}! "$TMPDIR/${img}_input.png"

  # png2amiga
  "$SCRIPT_DIR/build/png2amiga" --mode lores --depth 5 --dither none "$TMPDIR/${img}_input.png" "$TMPDIR/${img}_p2a_lores.png" 2>&1 | grep 'error:'
  "$SCRIPT_DIR/build/png2amiga" --mode ham6 --dither none "$TMPDIR/${img}_input.png" "$TMPDIR/${img}_p2a_ham6.png" 2>&1 | grep 'error:'

  # abc
  "$ABC" "$TMPDIR/${img}_input.png" -bpc 5 -quantize -preview "$TMPDIR/${img}_abc_lores.png" 2>&1 | tail -1
  "$ABC" "$TMPDIR/${img}_input.png" -ham -preview "$TMPDIR/${img}_abc_ham6.png" 2>&1 | tail -1

  # Stitch comparisons: png2amiga top-left, abc bottom-left, original right
  for mode in lores ham6; do
    abc_size=$(magick identify -format "%wx%h" "$TMPDIR/${img}_abc_${mode}.png")
    w="${abc_size%x*}"
    h="${abc_size#*x}"

    magick \( \( "$TMPDIR/${img}_p2a_${mode}.png" -filter Point -resize ${abc_size}! \) \
              "$TMPDIR/${img}_abc_${mode}.png" \
              -append \) \
           \( "$TMPDIR/${img}_input.png" "$TMPDIR/${img}_input.png" -append \) \
           +append "$OUTDIR/${img}_${mode}_compare.png"
  done
done

echo "---"
echo "Results in $OUTDIR/"
ls -lh "$OUTDIR"/*_compare.png
