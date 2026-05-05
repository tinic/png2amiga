#!/usr/bin/env bash
# Standalone build for the GPU quantization experiment. Uses Apple
# clang and the Metal toolchain — independent of the main png2amiga
# build (which uses gcc-15 and would not find macOS framework headers).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$SCRIPT_DIR"

# Prefer Xcode toolchain if user hasn't flipped xcode-select yet.
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"

CLANG="$(xcrun -f clang++)"
METAL="$(xcrun -f metal)"
METALLIB="$(xcrun -f metallib)"
SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)"

echo "=== Compile MSL → AIR ==="
"$METAL" -O3 -ffast-math -c quant.metal   -o quant.air
"$METAL" -O3 -ffast-math -c scolorq.metal -o scolorq.air

echo "=== Link AIR → metallib ==="
"$METALLIB" quant.air scolorq.air -o quant.metallib

echo "=== Compile + link C++ host ==="
"$CLANG" -std=c++20 -O2 -fno-objc-arc \
    -isysroot "$SDK_PATH" \
    -I"$REPO_ROOT/third_party/metal-cpp" \
    -framework Foundation -framework Metal -framework QuartzCore \
    quant.cpp -o quant

echo
echo "=== Run ==="
# Run from repo root so the metallib relative path in quant.cpp resolves.
cd "$REPO_ROOT"
./experiments/gpu_quantize/quant
