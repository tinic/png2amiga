#!/usr/bin/env bash
# Loop-play a sequence of PNG (or any image) files inline in iTerm2.
#
# Usage:
#   tools/play_loop.sh [-d delay] [-n loops] [-w width] file1.png file2.png ...
#
#   -d delay    Seconds between frames (default: 0.1, i.e. 10 fps).
#   -n loops    Number of full passes; 0 or unset = loop forever.
#   -w width    Inline image cell width in iTerm2 columns (default: 60).
#
# Examples:
#   tools/play_loop.sh examples/video/output_*.png
#   tools/play_loop.sh -d 0.04 examples/video/output_*.png   # ~25 fps
#   tools/play_loop.sh -n 1   examples/video/output_*.png   # play once
#
# Requires iTerm2 (uses its inline-image escape sequence). On other
# terminals the bytes are printed but no image renders. Press Ctrl-C
# to exit at any time.

set -euo pipefail

delay=0.1
loops=0
width=60

while getopts ":d:n:w:" opt; do
  case "$opt" in
    d) delay="$OPTARG" ;;
    n) loops="$OPTARG" ;;
    w) width="$OPTARG" ;;
    *) echo "Usage: $0 [-d delay] [-n loops] [-w width] file ..." >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))

if [[ $# -eq 0 ]]; then
  echo "Usage: $0 [-d delay] [-n loops] [-w width] file ..." >&2
  exit 2
fi

# Restore cursor on Ctrl-C.
trap 'printf "\033[?25h\n"; exit 0' INT TERM

# Hide cursor for cleaner playback.
printf '\033[?25l'

show_frame() {
  local f=$1
  # iTerm2 inline image escape: ESC ] 1337 ; File=inline=1;width=Ncols : <base64> BEL
  # `width=N` sizes in terminal cells; `preserveAspectRatio=1` keeps PAR.
  local b64
  b64=$(base64 < "$f" | tr -d '\n')
  printf '\033[H\033]1337;File=inline=1;width=%s;preserveAspectRatio=1:%s\a' \
    "$width" "$b64"
}

# Clear screen once at the start so the first frame paints to a clean canvas.
printf '\033[2J'

iter=0
while :; do
  for f in "$@"; do
    show_frame "$f"
    sleep "$delay"
  done
  iter=$((iter + 1))
  if [[ "$loops" -gt 0 && "$iter" -ge "$loops" ]]; then
    break
  fi
done

# Restore cursor.
printf '\033[?25h\n'
