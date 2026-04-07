#!/bin/bash
# Run an Amiga executable in fs-uae
#
# Usage: ./run-amiga.sh viewer.exe
#        ./run-amiga.sh viewer.cpp    (compiles first, then runs)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN="$SCRIPT_DIR/third_party/vscode-amiga-debug/bin"

case "$(uname -s)" in
    Darwin*) PLATFORM=darwin ;;
    Linux*)  PLATFORM=linux ;;
    *) echo "Unsupported platform"; exit 1 ;;
esac

EXE2ADF="$TOOLCHAIN/$PLATFORM/exe2adf"
FSUAE="$TOOLCHAIN/$PLATFORM/fs-uae/fs-uae"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <file.exe|file.cpp|file.c>"
    exit 1
fi

INPUT="$1"
BASENAME="${INPUT%.*}"
EXT="${INPUT##*.}"

# If source file, compile first
if [ "$EXT" = "cpp" ] || [ "$EXT" = "c" ]; then
    EXE="${BASENAME}.exe"
    "$SCRIPT_DIR/build-amiga.sh" "$INPUT" "$EXE"
    INPUT="$EXE"
    BASENAME="${INPUT%.*}"
fi

ADF="${BASENAME}.adf"

echo "Creating ADF: $ADF"
"$EXE2ADF" -i "$INPUT" -a "$ADF" -l "png2amiga"

MODEL="${2:-A1200}"
NTSC=""
for arg in "${@:2}"; do
    case "$arg" in
        a500|A500) MODEL="A500" ;;
        a1200|A1200) MODEL="A1200" ;;
        ntsc|NTSC) NTSC="--ntsc_mode=1" ;;
    esac
done

echo "Launching fs-uae ($MODEL${NTSC:+ NTSC})..."
"$FSUAE" \
    --floppy_drive_0="$ADF" \
    --amiga_model="$MODEL" \
    --chip_memory=2048 \
    --fast_memory=8192 \
    --window_width=800 \
    --window_height=600 \
    $NTSC \
    2>/dev/null &

echo "fs-uae started ($MODEL${NTSC:+ NTSC}). Press left mouse button to exit."
