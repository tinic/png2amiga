#!/bin/bash
set -e

echo "Building WASM..."
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release .
cmake --build build-wasm

echo "Building web..."
cd web
npm run build

echo "Done. Output in service/html/"
ls -lh ../service/html/index.html ../service/html/assets/*.wasm
