#!/bin/bash
# Build an AmigaOS executable from a png2amiga-generated .cpp/.c file
#
# Usage: ./build-amiga.sh viewer.cpp [output.exe]
#
# Requires: vscode-amiga-debug submodule (third_party/vscode-amiga-debug)
# The toolchain includes m68k-amiga-elf-gcc 14 and elf2hunk.

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
SUPPORT="$TEMPLATE/support"

if [ ! -x "$GCC" ]; then
    echo "Error: Cross-compiler not found at $GCC"
    echo "Run: git submodule update --init"
    exit 1
fi

if [ $# -lt 1 ]; then
    echo "Usage: $0 <source.cpp|source.c> [output.exe]"
    exit 1
fi

SOURCE="$1"
BASENAME="${SOURCE%.*}"
OUTPUT="${2:-${BASENAME}.exe}"
ELF="${BASENAME}.elf"

echo "Compiling: $SOURCE"
"$GCC" -m68000 -Ofast -nostdlib -fomit-frame-pointer -fno-exceptions \
    -I"$TEMPLATE" \
    -Wa,--register-prefix-optional \
    -Wl,--emit-relocs,-Ttext=0 \
    -o "$ELF" \
    "$SOURCE" \
    "$SUPPORT/gcc8_a_support.s" \
    "$SUPPORT/gcc8_c_support.c"

echo "Converting: $ELF -> $OUTPUT"
"$ELF2HUNK" "$ELF" "$OUTPUT"

# Cleanup ELF
rm -f "$ELF"

SIZE=$(wc -c < "$OUTPUT" | tr -d ' ')
echo "Done: $OUTPUT ($SIZE bytes)"
