#!/bin/bash
# Run an Amiga executable in Amiberry
#
# Usage: ./run-amiga.sh viewer.exe
#        ./run-amiga.sh viewer.cpp    (compiles first, then runs)
#        ./run-amiga.sh viewer.adf A500 ntsc
# Set AMIBERRY_BIN to override executable discovery.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN="$SCRIPT_DIR/third_party/vscode-amiga-debug/bin"

case "$(uname -s)" in
    Darwin*) PLATFORM=darwin ;;
    Linux*)  PLATFORM=linux ;;
    *) echo "Unsupported platform"; exit 1 ;;
esac

EXE2ADF="$TOOLCHAIN/$PLATFORM/exe2adf"
# Prefer an explicit override, then PATH, then the native macOS app.
if [ -n "${AMIBERRY_BIN:-}" ]; then
    AMIBERRY="$AMIBERRY_BIN"
elif command -v amiberry &>/dev/null; then
    AMIBERRY="$(command -v amiberry)"
elif [ "$PLATFORM" = "darwin" ] && [ -x /Applications/Amiberry.app/Contents/MacOS/Amiberry ]; then
    AMIBERRY=/Applications/Amiberry.app/Contents/MacOS/Amiberry
elif [ "$PLATFORM" = "darwin" ] && [ -x "$HOME/Applications/Amiberry.app/Contents/MacOS/Amiberry" ]; then
    AMIBERRY="$HOME/Applications/Amiberry.app/Contents/MacOS/Amiberry"
else
    echo "Error: Amiberry not found. Install it or set AMIBERRY_BIN." >&2
    exit 1
fi

if [ $# -lt 1 ]; then
    echo "Usage: $0 <file.exe|file.cpp|file.c|file.adf> [A500|A1200] [ntsc]"
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
    EXT="exe"
fi

if [ "$EXT" = "adf" ]; then
    ADF="$INPUT"
else
    ADF="${BASENAME}.adf"
    echo "Creating ADF: $ADF"
    "$EXE2ADF" -i "$INPUT" -a "$ADF" -l "png2amiga"
fi

MODEL=A1200
NTSC=false
for arg in "${@:2}"; do
    case "$arg" in
        a500|A500) MODEL="A500" ;;
        a1200|A1200) MODEL="A1200" ;;
        ntsc|NTSC) NTSC=true ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

# Memory configuration per model
case "$MODEL" in
    A500)
        # Stock A500: 512K chip + 512K slow (trapdoor expansion)
        CHIP=512
        SLOW=512
        FAST=0
        ;;
    *)
        # A1200+: 2MB chip + 8MB fast
        CHIP=2048
        SLOW=0
        FAST=8192
        ;;
esac

# Amiberry can change its working directory during startup.
ADF="$(cd "$(dirname "$ADF")" && pwd)/$(basename "$ADF")"
echo "Launching Amiberry ($MODEL: ${CHIP}K chip, ${SLOW}K slow, ${FAST}K fast, ntsc=$NTSC)..."
AMIBERRY_ARGS=(
    --model "$MODEL" -G -0 "$ADF"
    -c "$((CHIP / 512))" -b "$((SLOW / 256))" -F "$((FAST / 1024))"
    -w 0 -s cachesize=0
    -s "ntsc=$NTSC"
    -s gfx_width_windowed=800 -s gfx_height_windowed=600
    -s gfx_fullscreen_amiga=false
    -s floppy_speed=0
)
if [ "$PLATFORM" = darwin ] && [[ "$AMIBERRY" == */Contents/MacOS/Amiberry ]]; then
    # LaunchServices keeps the native app alive after the calling shell exits.
    # A new instance also ensures arguments are applied if Amiberry is open.
    open -n -a "${AMIBERRY%/Contents/MacOS/Amiberry}" --args "${AMIBERRY_ARGS[@]}"
else
    nohup "$AMIBERRY" "${AMIBERRY_ARGS[@]}" >"${BASENAME}.amiberry.log" 2>&1 </dev/null &
    echo "Emulator log: ${BASENAME}.amiberry.log"
fi

echo "Amiberry started. Press left mouse button to exit the viewer."
