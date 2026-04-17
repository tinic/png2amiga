#!/usr/bin/env bash
# Sweep --copper-changes K to gauge PSNR gain from CPU-assisted palette
# writes on AGA. Stock A1200 (~10-15 extra writes/HBlank) corresponds to
# K ≈ 13-18; accelerated machines push higher.
set -e

BIN="${BIN:-./build/png2amiga}"
TMPDIR="${TMPDIR:-/tmp/cpu_assist}"
mkdir -p "$TMPDIR"

EXAMPLES=(electrichues02 fantasy fantasy1 grungy logo lovers photo space3)
DEPTHS=(5 6 7)
KS=(3 6 13 20 30)

for img in "${EXAMPLES[@]}"; do
  src=""
  for ext in jpg png; do
    [ -f "examples/${img}.${ext}" ] && src="examples/${img}.${ext}"
  done
  [ -z "$src" ] && continue
  scaled="$TMPDIR/${img}_src.png"
  magick "$src" -resize 320x "$scaled" 2>/dev/null

  for d in "${DEPTHS[@]}"; do
    for k in "${KS[@]}"; do
      out="$TMPDIR/${img}_d${d}_k${k}.png"
      "$BIN" --mode lores --copper --chipset aga --depth "$d" --dither floyd-steinberg \
        --copper-changes "$k" "$scaled" "$out" >/dev/null 2>&1 || { echo "FAIL $img d$d k$k" >&2; continue; }
      psnr=$(magick compare -metric PSNR "$scaled" "$out" null: 2>&1 || true)
      printf "%s\td%s\tk%s\t%s\n" "$img" "$d" "$k" "$psnr"
    done
  done
done
