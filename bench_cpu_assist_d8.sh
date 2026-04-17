#!/usr/bin/env bash
set -e
BIN="${BIN:-./build/png2amiga}"
TMPDIR="${TMPDIR:-/tmp/cpu_assist8}"
mkdir -p "$TMPDIR"
EXAMPLES=(electrichues02 fantasy fantasy1 grungy logo lovers photo space3)
KS=(3 6 13 20 30 60)
for img in "${EXAMPLES[@]}"; do
  src=""
  for ext in jpg png; do [ -f "examples/${img}.${ext}" ] && src="examples/${img}.${ext}"; done
  [ -z "$src" ] && continue
  scaled="$TMPDIR/${img}_src.png"
  magick "$src" -resize 320x "$scaled" 2>/dev/null
  for k in "${KS[@]}"; do
    out="$TMPDIR/${img}_k${k}.png"
    "$BIN" --mode lores --copper --chipset aga --depth 8 --dither floyd-steinberg \
      --copper-changes "$k" "$scaled" "$out" >/dev/null 2>&1 || continue
    psnr=$(magick compare -metric PSNR "$scaled" "$out" null: 2>&1 || true)
    printf "%s\tk%s\t%s\n" "$img" "$k" "$psnr"
  done
done
