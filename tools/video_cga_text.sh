#!/usr/bin/env bash
# Stylize a video through png2amiga's cga-text80x100 mode.
#
#   1. ffmpeg extracts the source video to PNG frames.
#   2. png2amiga reskins each frame as 80×100-cell IBM CGA text-mode
#      glyphs (`--mode cga-text80x100 --gamma 1.5 --crop-auto`).
#   3. ffmpeg reassembles the stylized PNGs back into a video and
#      re-attaches the source audio track at the original frame rate.
#
# Usage:
#   ./video_cga_text.sh                             # default: ~/youtube/Dune trailer
#   ./video_cga_text.sh path/to/input.mp4
#   ./video_cga_text.sh path/to/input.mp4 path/to/output.mp4
#
# Env knobs:
#   WORK=/tmp/p2a_video   scratch dir (frames in/out + log)
#   PNG2AMIGA=…           override binary location
#   JOBS=N                parallel encodes (default: ncpu)
#   MODE=…                override mode (default: cga-text80x100)
#   GAMMA=…               override gamma  (default: 1.5)
#
# Idempotent: if frames_in/ or frames_out/ already contain the expected
# file count the corresponding step is skipped. Delete WORK to redo.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

INPUT="${1:-$HOME/youtube/Dune Part Three Official Teaser Trailer.mp4}"
WORK="${WORK:-/tmp/p2a_video}"
OUT_VIDEO="${2:-$WORK/output.mp4}"
PNG2AMIGA="${PNG2AMIGA:-$REPO_ROOT/build/png2amiga}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
MODE="${MODE:-cga-text80x100}"
GAMMA="${GAMMA:-1.5}"

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ERROR: ffmpeg not found on PATH (brew install ffmpeg)" >&2
  exit 1
fi
if [ ! -x "$PNG2AMIGA" ]; then
  echo "ERROR: png2amiga binary not found / not executable at: $PNG2AMIGA" >&2
  echo "       Build first:  cmake -B build . && cmake --build build" >&2
  exit 1
fi
if [ ! -f "$INPUT" ]; then
  echo "ERROR: input video not found: $INPUT" >&2
  exit 1
fi

IN_DIR="$WORK/frames_in"
OUT_DIR="$WORK/frames_out"
mkdir -p "$IN_DIR" "$OUT_DIR" "$(dirname "$OUT_VIDEO")"

# --- 1. Probe source ------------------------------------------------------
echo "==> Probing $INPUT"
FPS=$(ffprobe -v error -select_streams v:0 \
              -show_entries stream=r_frame_rate -of csv=p=0 "$INPUT")
DURATION=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$INPUT")
HAS_AUDIO=$(ffprobe -v error -select_streams a:0 \
              -show_entries stream=codec_type -of csv=p=0 "$INPUT" || true)
printf "    fps=%s  duration=%ss  audio=%s\n" "$FPS" "$DURATION" \
       "${HAS_AUDIO:-none}"

# --- 2. Extract frames ----------------------------------------------------
EXISTING_IN=$(find "$IN_DIR" -maxdepth 1 -name 'frame_*.png' | wc -l | tr -d ' ')
if [ "$EXISTING_IN" -eq 0 ]; then
  echo "==> Extracting frames -> $IN_DIR/"
  ffmpeg -hide_banner -loglevel warning -y \
         -i "$INPUT" "$IN_DIR/frame_%06d.png"
  EXISTING_IN=$(find "$IN_DIR" -maxdepth 1 -name 'frame_*.png' | wc -l | tr -d ' ')
fi
echo "    $EXISTING_IN input frames"

# --- 3. Stylize each frame through png2amiga ------------------------------
echo "==> Encoding frames (mode=$MODE gamma=$GAMMA, $JOBS in parallel)"
export PNG2AMIGA MODE GAMMA OUT_DIR
convert_one() {
  local in="$1"
  local out="$OUT_DIR/$(basename "$in")"
  [ -f "$out" ] && return 0   # already done — idempotent skip
  "$PNG2AMIGA" --quiet --mode "$MODE" --gamma "$GAMMA" --crop-auto \
               "$in" "$out" >/dev/null 2>&1
}
export -f convert_one

# xargs -P parallelism. Bare `find … -print0 | xargs -0` keeps spaces in
# paths intact (frames_in/ is under /tmp so this is mostly belt-and-braces).
find "$IN_DIR" -maxdepth 1 -name 'frame_*.png' -print0 \
  | xargs -0 -n 1 -P "$JOBS" -I {} bash -c 'convert_one "$@"' _ {}

DONE_OUT=$(find "$OUT_DIR" -maxdepth 1 -name 'frame_*.png' | wc -l | tr -d ' ')
if [ "$DONE_OUT" -ne "$EXISTING_IN" ]; then
  echo "WARNING: only $DONE_OUT/$EXISTING_IN frames encoded successfully" >&2
fi

# --- 4. Reassemble video --------------------------------------------------
echo "==> Reassembling -> $OUT_VIDEO"
# CGA-text frames are tiny; libx264 + yuv420p needs even dimensions, so
# the scale filter pads to the nearest even pair (no upscale).
if [ -n "$HAS_AUDIO" ]; then
  ffmpeg -hide_banner -loglevel warning -y \
    -framerate "$FPS" -i "$OUT_DIR/frame_%06d.png" \
    -i "$INPUT" \
    -map 0:v -map 1:a \
    -vf "pad=ceil(iw/2)*2:ceil(ih/2)*2" \
    -c:v libx264 -pix_fmt yuv420p -crf 18 -preset slow \
    -c:a aac -b:a 192k -shortest \
    "$OUT_VIDEO"
else
  ffmpeg -hide_banner -loglevel warning -y \
    -framerate "$FPS" -i "$OUT_DIR/frame_%06d.png" \
    -vf "pad=ceil(iw/2)*2:ceil(ih/2)*2" \
    -c:v libx264 -pix_fmt yuv420p -crf 18 -preset slow \
    "$OUT_VIDEO"
fi

echo "==> Done."
echo "    $OUT_VIDEO  ($(du -h "$OUT_VIDEO" | cut -f1))"
