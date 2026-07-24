#!/bin/bash
set -e

# Anchor at repo root so this works whether invoked as tools/build-web.sh
# from the repo root or directly via its absolute path.
cd "$(cd "$(dirname "$0")/.." && pwd)"

echo "Building WASM..."
emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release .
# --parallel matters more since the SIMD and scalar variants are two full
# LTO links of the same sources.
cmake --build build-wasm --parallel

echo "Building web..."
cd web
# Lockfile-only deterministic install when node_modules is stale
# relative to package-lock.json. Then run the full pipeline:
# typecheck → lint → audit → vite → bundle-size.
if [ ! -d node_modules ] || [ package-lock.json -nt node_modules ]; then
  npm ci --no-audit --no-fund
fi
npm run build

echo "Done. Output in service/html/"
ls -lh ../service/html/index.html ../service/html/assets/*.wasm
