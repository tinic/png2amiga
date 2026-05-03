#!/usr/bin/env bash
# Mode-output survey. Runs png2amiga --preview against a single image for
# every supported mode, captures the CLI status block (Input / Target /
# Chipset / Dither / Mode / Encoded), strips ANSI + carriage returns, and
# emits a single Markdown-ish table for review.
#
# Usage:
#   tools/mode_survey.sh                       # uses examples/fantasy.png
#   tools/mode_survey.sh path/to/image.png
#
# Output: tools/mode_survey.txt (one block per mode, separated by ---)
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$REPO/build/png2amiga"
SRC="${1:-$REPO/examples/fantasy.png}"
OUT="$REPO/tools/mode_survey.txt"

if [[ ! -x "$BIN" ]]; then
  echo "ERROR: build $BIN first (cmake --build build -j)" >&2
  exit 1
fi
if [[ ! -f "$SRC" ]]; then
  echo "ERROR: source image not found: $SRC" >&2
  exit 1
fi

MODES=(
  # Amiga
  lores hires
  ham4 ham5 ham6 ham7 ham8
  ehb
  # Atari ST/STE
  stf-low stf-med stf-hi
  ste-low ste-med ste-hi
  # IBM PC
  cga-320 cga-640 cga-composite cga-text80x100
  ega-320 ega-640 ega-hi
  vga-13h vga-10h vga-12h
  # C64
  c64-multicolor c64-hires c64-fli c64-afli c64-petscii
  c64-charset-hires c64-charset-multicolor
  # SNES / Genesis
  snes-mode7-256 snes-mode7-direct
  genesis-h32 genesis-h40 genesis-h32-sh genesis-h40-sh
)

# Variant flags worth surveying for the modes that support them.
# Each entry is "<name>|<flags>"
VARIANTS=(
  "plain|"
  "best|--best"
  "sliced|--sliced"
  "sliced-best|--sliced --best"
  "strips|--strips"
  "strips-best|--strips --best"
  "dpf|--dpf"
  "scap|--scap"
  "copper|--copper"
  "interlace|--interlace"
)

# strip-ansi: drop CSI escapes + carriage returns and keep only the human
# status lines ("Input:", "Target:", "Chipset:", "Dither:", "Mode:",
# "Palette:", "Tiles:", "Encoded:").
strip_status() {
  perl -pe 's/\x1B\[[0-?]*[ -\/]*[@-~]//g; s/\r/\n/g' \
    | grep -E '^(Input|Target|Chipset|Dither|Mode|Palette|Tiles|Encoded):' \
    || true
}

echo "png2amiga mode survey — $(date)" > "$OUT"
echo "Source: $SRC" >> "$OUT"
echo "Binary: $BIN" >> "$OUT"
echo >> "$OUT"

run_one() {
  local label="$1"
  local mode="$2"
  shift 2
  local extra=("$@")
  echo "### $label" >> "$OUT"
  echo '```' >> "$OUT"
  printf '$ png2amiga --preview --mode %s' "$mode" >> "$OUT"
  for f in "${extra[@]}"; do printf ' %s' "$f" >> "$OUT"; done
  echo >> "$OUT"
  local out
  out=$("$BIN" --preview --mode "$mode" "${extra[@]}" "$SRC" 2>&1 || true)
  echo "$out" | strip_status >> "$OUT"
  echo '```' >> "$OUT"
  echo >> "$OUT"
}

for mode in "${MODES[@]}"; do
  echo "=== $mode ===" >> "$OUT"
  for v in "${VARIANTS[@]}"; do
    name="${v%%|*}"
    flags_str="${v#*|}"
    # Skip variants that don't apply to this mode family.
    case "$mode:$name" in
      # --best is wired for the modes listed in CLAUDE.md memory; for the
      # rest it's a no-op but harmless.
      # --sliced / --strips / --dpf / --scap / --copper only apply to
      # Amiga lores / hires / ham6 / ehb. Skip others entirely.
      *:plain|*:best|*:interlace) ;;
      lores:sliced|lores:sliced-best|lores:strips|lores:strips-best|lores:dpf|lores:scap|lores:copper) ;;
      hires:sliced|hires:sliced-best|hires:strips|hires:strips-best|hires:copper) ;;
      ham6:sliced|ham6:sliced-best|ham6:strips|ham6:strips-best|ham6:copper) ;;
      ham8:sliced|ham8:sliced-best|ham8:copper) ;;
      ehb:sliced|ehb:sliced-best|ehb:strips|ehb:strips-best|ehb:copper) ;;
      *:sliced|*:sliced-best|*:strips|*:strips-best|*:dpf|*:scap|*:copper) continue ;;
    esac
    if [[ -z "$flags_str" ]]; then
      run_one "$mode / $name" "$mode"
    else
      # shellcheck disable=SC2206
      flags=($flags_str)
      run_one "$mode / $name" "$mode" "${flags[@]}"
    fi
  done
  echo >> "$OUT"
done

echo "Survey written to: $OUT"
echo "Lines: $(wc -l < "$OUT")"
