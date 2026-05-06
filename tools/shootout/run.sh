#!/usr/bin/env bash
# HAM/SHAM converter shootout: encode the same source image through every
# entrant, all on Floyd-Steinberg dither for apples-to-apples, dump
# previews into output/, then compute PSNR vs the resized source.
#
# Usage:
#   ./run.sh                      # default: ../../examples/fantasy1.png
#   ./run.sh path/to/image.png    # custom source
#
# Outputs:
#   output/target.png             # source resized to TARGET_W × TARGET_H
#   output/<entry>.png            # decoded preview from each encoder
#   output/<entry>.iff            # encoder IFF (where applicable)
#   output/results.txt            # PSNR table
#
# Image dimensions: 320×213 (HAM6 PAL square-pixel, 4:3 source). All
# encoders get the same pre-resized target so PSNR doesn't measure the
# resize stage's quality, only the encoder's quality.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VENDOR="$SCRIPT_DIR/vendor"
OUT="$SCRIPT_DIR/output"
mkdir -p "$OUT"

SRC="${1:-$REPO_ROOT/examples/makena.jpg}"
if [ ! -f "$SRC" ]; then
  echo "ERROR: source image not found: $SRC" >&2
  exit 1
fi

TARGET_W=320
TARGET_H=213

JAVA="${JAVA:-/opt/homebrew/opt/openjdk/bin/java}"
[ -x "$JAVA" ] || JAVA=$(command -v java)

PNG2AMIGA="$REPO_ROOT/build/png2amiga"
HAM_CONVERT_JAR="$VENDOR/ham_convert/ham_convert.jar"
ABC2="$VENDOR/abc/abc2"
PNGQUANT="${PNGQUANT:-$(command -v pngquant || true)}"

for x in "$PNG2AMIGA" "$HAM_CONVERT_JAR" "$ABC2"; do
  if [ ! -e "$x" ]; then
    echo "ERROR: missing $x — run ./setup.sh first" >&2
    exit 1
  fi
done
if [ -z "$PNGQUANT" ] || [ ! -x "$PNGQUANT" ]; then
  echo "WARNING: pngquant (libimagequant CLI) not found — lores-d5" >&2
  echo "         comparison will skip the libimagequant entry." >&2
  echo "         Install via: brew install pngquant" >&2
fi

# --- Step 1: pre-resize source to the target dims ---------------------------
# Use png2amiga with --no-scale=off (default) just so the resize is the
# same Lanczos/bicubic engine; route via ./scripts/resize.py for an
# encoder-independent reference.
TARGET="$OUT/target.png"
python3 "$SCRIPT_DIR/scripts/resize.py" "$SRC" "$TARGET" "$TARGET_W" "$TARGET_H"
echo "==> Target image: $TARGET (${TARGET_W}×${TARGET_H})"
echo

# --- Step 2: encode each entry ---------------------------------------------
# Each block writes <entry>.png (decoded preview at TARGET dims) into $OUT.
# We use a uniform Floyd-Steinberg dither across all entries.

# Wall-clock timer: writes elapsed seconds (float, 2-decimal) to <out>.
# Uses python for sub-second precision since macOS bash 3.2 lacks
# EPOCHREALTIME and `date +%s.%N` isn't portable.
time_to() {
  local out="$1"; shift
  local t0 rc t1
  t0=$(python3 -c 'import time; print(time.time())')
  "$@"
  rc=$?
  t1=$(python3 -c 'import time; print(time.time())')
  python3 -c "import sys; print(f'{float(sys.argv[1]) - float(sys.argv[2]):.2f}')" \
    "$t1" "$t0" > "$out"
  return $rc
}

run_png2amiga() {
  local label="$1"; shift
  echo "==> [png2amiga] $label"
  # PNG preview is what PSNR reads. STRIPS modes only emit PNG / .cpp /
  # .c / .h (no IFF for STRIPS — there's no PCHG-equivalent yet for the
  # mid-line MOVE stream), so the IFF pass is a best-effort "skip on
  # failure" to keep the harness simple. Timing covers the PNG-producing
  # invocation only — the IFF pass is a duplicate encode and would
  # roughly double the figure.
  "$PNG2AMIGA" --quiet --no-scale --dither floyd-steinberg "$@" \
    "$TARGET" -o "$OUT/$label.iff" 2>/dev/null || true
  time_to "$OUT/$label.time" \
    "$PNG2AMIGA" --quiet --no-scale --dither floyd-steinberg "$@" \
      "$TARGET" -o "$OUT/$label.png"
}

# 1) abc HAM6
echo "==> [abc] ham6 + floyd"
time_to "$OUT/abc-ham6.time" \
  "$ABC2" "$TARGET" -ham -floyd \
    -preview "$OUT/abc-ham6.png" -iff "$OUT/abc-ham6.iff" >/dev/null

# 2) abc SHAM6
echo "==> [abc] sham6 + floyd"
time_to "$OUT/abc-sham6.time" \
  "$ABC2" "$TARGET" -sham -floyd \
    -preview "$OUT/abc-sham6.png" -iff "$OUT/abc-sham6.iff" >/dev/null

# ham_convert is a Swing GUI app whose JVM does NOT exit after CLI work
# completes — even with positional args, the AWT event loop keeps it
# alive. Workaround: launch via nohup, poll for the output file, then
# kill the JVM. ham_convert writes <basename>_output.{iff,png} next to
# the source. We isolate each invocation in its own temp dir so output
# names don't collide.
run_ham_convert() {
  local label="$1" mode="$2"
  echo "==> [ham_convert] $mode + dither_fs"
  local tmp="$OUT/_hc_${label}"
  rm -rf "$tmp"; mkdir -p "$tmp"
  cp "$TARGET" "$tmp/in.png"
  local hc_t0
  hc_t0=$(python3 -c 'import time; print(time.time())')
  ( cd "$tmp"
    # black_bkd forces palette[0] = black, matching png2amiga's
    # --lock-color0 default. Apples-to-apples comparison.
    nohup "$JAVA" -Xms500m -Xmx2g -jar "$HAM_CONVERT_JAR" \
      in.png "$mode" dither_fs propagation_85 color_lab_cie94 black_bkd \
      > "$tmp/hc.log" 2>&1 &
    pid=$!
    # The CLI prints "Writing iff: ..." once the IFF is on disk and
    # then the GUI thread keeps the JVM up. We poll for the IFF + a
    # small grace window so the encoder has finished flushing, then
    # kill the JVM. Cap at 30 minutes — ham6_q7 (extensive palette
    # optimiser + triple mode) is genuinely slow even on 320×213.
    deadline=1800
    waited=0
    while kill -0 "$pid" 2>/dev/null; do
      if compgen -G "$tmp/in_*.iff" >/dev/null && \
         compgen -G "$tmp/in_*.png" >/dev/null; then
        sleep 2
        kill -9 "$pid" 2>/dev/null || true
        break
      fi
      sleep 2
      waited=$((waited + 2))
      if [ "$waited" -ge "$deadline" ]; then
        echo "    !! ham_convert $mode exceeded ${deadline}s, killing"
        kill -9 "$pid" 2>/dev/null || true
        break
      fi
    done
  )
  # Stop the clock once the JVM is gone. Includes the JVM-startup cost
  # plus the 2 s grace window we added before SIGKILL — both
  # unavoidable, neither huge relative to ham_convert's actual work
  # (q7 / sliced both run for many seconds).
  local hc_t1 elapsed
  hc_t1=$(python3 -c 'import time; print(time.time())')
  python3 -c "import sys; print(f'{float(sys.argv[1]) - float(sys.argv[2]):.2f}')" \
    "$hc_t1" "$hc_t0" > "$OUT/$label.time"
  # ham_convert writes <basename>_output.{png,iff} for the actual preview,
  # plus in_palettes.png (and similar) for sliced modes. Pin to "_output"
  # so the palette debug image doesn't confuse PSNR ranking.
  local hc_png="$tmp/in_output.png"
  local hc_iff="$tmp/in_output.iff"
  [ -f "$hc_png" ] && cp "$hc_png" "$OUT/$label.png"
  [ -f "$hc_iff" ] && cp "$hc_iff" "$OUT/$label.iff"
  if [ ! -f "$hc_iff" ]; then
    echo "    !! ham_convert produced no IFF — see $tmp/hc.log"
  fi
}

# 3) ham_convert HAM6 max-quality (q7) + Floyd-Steinberg
run_ham_convert hc-ham6  ham6_q7

# 3a) ham_convert HAM6 fast profile (q1) + Floyd-Steinberg
run_ham_convert hc-ham6-fast ham6_q1

# 3b) ham_convert EHB (64-color OCS, hardware half-brite) + dither_fs
run_ham_convert hc-ehb ehb

# 4) ham_convert SHAM6 (ham6_sliced) + dither_fs
run_ham_convert hc-sham6 ham6_sliced

# 5) png2amiga HAM6 (no SLICED, no --best — baseline for time/quality)
run_png2amiga "p2a-ham6"             --mode ham6

# 6) png2amiga HAM6 + best (no SLICED, multi-restart sweep)
run_png2amiga "p2a-ham6-noncap-best" --mode ham6 --best

# 7) png2amiga HAM6 + SLICED (no --best — single-pass per-line palette)
run_png2amiga "p2a-ham6-cap"         --mode ham6 --sliced

# 8) png2amiga HAM6 + SLICED + best
run_png2amiga "p2a-ham6-best"        --mode ham6 --sliced --best

# 9) png2amiga EHB + STRIPS + best
run_png2amiga "p2a-ehb-best"         --mode ehb --strips --best

# 10) png2amiga plain EHB (no copper) + best — head-to-head with hc-ehb
run_png2amiga "p2a-ehb-plain-best"   --mode ehb --best

# 11) png2amiga plain EHB (no copper, no best) — fast baseline
run_png2amiga "p2a-ehb-plain"        --mode ehb

# 12) png2amiga lores 5bpp (32-color indexed, no copper) — head-to-head
#     with ham_convert ocs32 and libimagequant in the same colour-count
#     regime.
run_png2amiga "p2a-lores-d5"         --mode lores --depth 5

# 13) png2amiga lores 5bpp + --best (multi-restart sweep)
run_png2amiga "p2a-lores-d5-best"    --mode lores --depth 5 --best

# 14) ham_convert ocs32 (32-colour OCS lores indexed) + dither_fs.
#     ham_convert's full mode list includes indexed OCS/AGA modes
#     (ocs2/4/8/16/32, aga64/128/256), not just HAM/EHB.
run_ham_convert hc-lores-d5  ocs32

# 15) libimagequant via pngquant CLI: 32 colours, max quality (--speed 1),
#     Floyd-Steinberg (the default for pngquant). pngquant outputs an
#     indexed PNG-8 (PLTE chunk) at the input dimensions — same shape
#     as our --mode lores --depth 5 output, so the PSNR script reads
#     it identically. Skipped when pngquant isn't installed.
if [ -n "$PNGQUANT" ] && [ -x "$PNGQUANT" ]; then
    echo "==> [pngquant] 32 colours + floyd"
    time_to "$OUT/libimagequant-32.time" \
      "$PNGQUANT" --speed 1 --strip --output "$OUT/libimagequant-32.png" \
        --force 32 "$TARGET"
fi

# 16) png2amiga lores 8bpp (256-color AGA indexed). 32-image probe
#     showed our quantizer with the new default palette_diversity=4
#     beats libimagequant by ~5 S2 at depth 8 — adding the entry here
#     so the leaderboard shows where we land at 256 colours too.
run_png2amiga "p2a-lores-d8"         --mode lores --depth 8 --chipset aga

# 17) png2amiga lores 8bpp + --best (multi-restart sweep)
run_png2amiga "p2a-lores-d8-best"    --mode lores --depth 8 --chipset aga --best

# 18) libimagequant 256 colours head-to-head with d=8.
if [ -n "$PNGQUANT" ] && [ -x "$PNGQUANT" ]; then
    echo "==> [pngquant] 256 colours + floyd"
    time_to "$OUT/libimagequant-256.time" \
      "$PNGQUANT" --speed 1 --strip --output "$OUT/libimagequant-256.png" \
        --force 256 "$TARGET"
fi

# --- Step 3: PSNR table ----------------------------------------------------
echo
echo "==> Computing PSNR..."
python3 "$SCRIPT_DIR/scripts/psnr.py" "$TARGET" "$OUT" \
  | tee "$OUT/results.txt"
