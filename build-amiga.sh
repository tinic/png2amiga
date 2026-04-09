#!/bin/bash
# Build an AmigaOS executable (.exe) or bootable floppy image (.adf) from a
# png2amiga-generated .cpp/.c file. Output format is inferred from the
# output filename's extension (.exe or .adf). Defaults to .exe.
#
# Usage:
#   ./build-amiga.sh viewer.cpp                  # -> viewer.exe
#   ./build-amiga.sh viewer.cpp output.exe       # -> output.exe
#   ./build-amiga.sh viewer.cpp output.adf       # -> output.adf (bootable)
#
# Requires: vscode-amiga-debug submodule (third_party/vscode-amiga-debug)
# The toolchain includes m68k-amiga-elf-gcc 14, elf2hunk, and exe2adf.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN="$SCRIPT_DIR/third_party/vscode-amiga-debug/bin"
TEMPLATE="$SCRIPT_DIR/third_party/vscode-amiga-debug/template"

# Detect platform
case "$(uname -s)" in
    Darwin*) PLATFORM=darwin ;;
    Linux*)  PLATFORM=linux ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM=win32 ;;
    *) echo "Unsupported platform"; exit 1 ;;
esac

GCC="$TOOLCHAIN/$PLATFORM/opt/bin/m68k-amiga-elf-gcc"
ELF2HUNK="$TOOLCHAIN/$PLATFORM/elf2hunk"
EXE2ADF="$TOOLCHAIN/$PLATFORM/exe2adf"
SUPPORT="$TEMPLATE/support"

if [ ! -x "$GCC" ]; then
    echo "Error: Cross-compiler not found at $GCC"
    echo "Run: git submodule update --init"
    exit 1
fi

if [ $# -lt 1 ]; then
    echo "Usage: $0 <source.cpp|source.c> [output.exe|output.adf]"
    exit 1
fi

SOURCE="$1"
BASENAME="${SOURCE%.*}"
OUTPUT="${2:-${BASENAME}.exe}"
ELF="${BASENAME}.elf"

# Determine output format from extension (default: exe)
OUT_EXT="${OUTPUT##*.}"
case "$OUT_EXT" in
    adf|ADF) FORMAT=adf ;;
    *)       FORMAT=exe ;;
esac

# For ADF output, compile to an intermediate .exe in the same directory
# first, then convert. For EXE output, the final file IS the .exe.
if [ "$FORMAT" = "adf" ]; then
    if [ ! -x "$EXE2ADF" ]; then
        echo "Error: exe2adf not found at $EXE2ADF"
        echo "Run: git submodule update --init"
        exit 1
    fi
    EXE_TMP="${BASENAME}.exe"
else
    EXE_TMP="$OUTPUT"
fi

echo "Compiling: $SOURCE"
"$GCC" -m68000 -Ofast -nostdlib -fomit-frame-pointer -fno-exceptions \
    -I"$TEMPLATE" \
    -Wa,--register-prefix-optional \
    -Wl,--emit-relocs,-Ttext=0 \
    -o "$ELF" \
    "$SOURCE" \
    "$SUPPORT/gcc8_a_support.s" \
    "$SUPPORT/gcc8_c_support.c"

echo "Converting: $ELF -> $EXE_TMP"
"$ELF2HUNK" "$ELF" "$EXE_TMP"
rm -f "$ELF"

if [ "$FORMAT" = "adf" ]; then
    # exe2adf produces a bootable ADF that auto-loads and runs the exe.
    # -l sets the disk volume label.
    LABEL="$(basename "$BASENAME")"
    echo "Packaging: $EXE_TMP -> $OUTPUT (label: $LABEL)"
    "$EXE2ADF" -i "$EXE_TMP" -a "$OUTPUT" -l "$LABEL"
    rm -f "$EXE_TMP"
fi

SIZE=$(wc -c < "$OUTPUT" | tr -d ' ')
echo "Done: $OUTPUT ($SIZE bytes)"
