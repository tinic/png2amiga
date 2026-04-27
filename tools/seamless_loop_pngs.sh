#!/usr/bin/env bash
# Convert a video into a seamlessly-looping PNG sequence.
#
# Splits the input at its midpoint and stitches the second half before the
# first half, cross-fading the join. The other junction (firsthalf-end →
# secondhalf-start when the loop wraps) is the original adjacent-frame
# cut so it's already seamless. The result plays as an endless loop with
# no visible discontinuity.
#
# Usage:
#   tools/seamless_loop_pngs.sh INPUT.mp4 [-s WxH] [-k SKIP] [-x XFADE]
#                                          [-o OUT_DIR] [-p PATTERN]
#
#   INPUT       Source video (any container ffmpeg understands).
#   -s WxH      Output frame size. Default 320x220.
#   -k SKIP     Pick every Nth frame after xfade. Default 8.
#   -x XFADE    Cross-fade duration in seconds. Default 1.0.
#   -o OUT_DIR  Output directory. Default ./loop_pngs.
#   -p PATTERN  printf-style file name. Default output_%04d.png.
#
# Length is auto-detected via ffprobe; the script clamps XFADE to no more
# than 40% of half the duration so very short clips still produce a loop.

set -euo pipefail

usage() {
  echo "Usage: $0 INPUT.mp4 [-s WxH] [-k SKIP] [-x XFADE] [-o DIR] [-p PATTERN]" >&2
  exit 2
}

[[ $# -ge 1 ]] || usage
INPUT=$1
shift

SIZE=320x220
SKIP=8
XFADE=1.0
OUT_DIR=loop_pngs
PATTERN=output_%04d.png

while getopts ":s:k:x:o:p:" opt; do
  case "$opt" in
    s) SIZE=$OPTARG ;;
    k) SKIP=$OPTARG ;;
    x) XFADE=$OPTARG ;;
    o) OUT_DIR=$OPTARG ;;
    p) PATTERN=$OPTARG ;;
    *) usage ;;
  esac
done

if [[ ! -r "$INPUT" ]]; then
  echo "Error: cannot read input '$INPUT'" >&2
  exit 1
fi

W=${SIZE%x*}
H=${SIZE#*x}
if ! [[ "$W" =~ ^[0-9]+$ && "$H" =~ ^[0-9]+$ ]]; then
  echo "Error: -s WxH expected (got '$SIZE')" >&2
  exit 2
fi

# Auto-detect duration in seconds (float). Falls back to format=duration if
# the stream-level field is missing (e.g. some MOV files).
DURATION=$(ffprobe -v error -select_streams v:0 \
  -show_entries stream=duration -of csv=p=0 "$INPUT" 2>/dev/null || true)
if [[ -z "$DURATION" || "$DURATION" == "N/A" ]]; then
  DURATION=$(ffprobe -v error -show_entries format=duration \
    -of csv=p=0 "$INPUT")
fi
if [[ -z "$DURATION" || "$DURATION" == "N/A" ]]; then
  echo "Error: could not detect duration of '$INPUT'" >&2
  exit 1
fi

MID=$(awk -v d="$DURATION" 'BEGIN { printf "%.6f", d/2 }')
# Clamp xfade so offset stays positive on short clips:
#   offset = MID - XFADE  ⇒  XFADE ≤ MID * 0.8 (leave 20% clean head)
XFADE=$(awk -v x="$XFADE" -v m="$MID" \
  'BEGIN { lim = m*0.8; if (x > lim) x = lim; if (x < 0.05) x = 0.05;
            printf "%.6f", x }')
OFFSET=$(awk -v m="$MID" -v x="$XFADE" \
  'BEGIN { printf "%.6f", m - x }')

mkdir -p "$OUT_DIR"
# Wipe stale output_*.png from prior runs so the new sequence is clean.
find "$OUT_DIR" -maxdepth 1 -type f -name 'output_*.png' -delete 2>/dev/null || true

echo "Input:    $INPUT"
echo "Duration: ${DURATION}s (mid=${MID}s, xfade=${XFADE}s, offset=${OFFSET}s)"
echo "Size:     ${W}x${H}, frame skip 1/${SKIP}"
echo "Output:   ${OUT_DIR}/${PATTERN}"

ffmpeg -y -hide_banner -loglevel error -stats \
  -i "$INPUT" \
  -filter_complex \
    "[0:v]trim=0:${MID},setpts=PTS-STARTPTS[firsthalf]; \
     [0:v]trim=${MID}:${DURATION},setpts=PTS-STARTPTS[secondhalf]; \
     [secondhalf][firsthalf]\
xfade=transition=fade:duration=${XFADE}:offset=${OFFSET},\
scale=${W}:${H},\
select='not(mod(n,${SKIP}))'[out]" \
  -map "[out]" -vsync vfr "${OUT_DIR}/${PATTERN}"

COUNT=$(find "$OUT_DIR" -maxdepth 1 -type f -name 'output_*.png' | wc -l | tr -d ' ')
echo "Wrote $COUNT frames into $OUT_DIR/"
