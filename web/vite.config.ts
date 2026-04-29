import path from 'node:path'
import fs from 'node:fs'

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

function readProjectVersion(): string {
  const cmake = fs.readFileSync(path.resolve(__dirname, '../CMakeLists.txt'), 'utf8')
  const m = cmake.match(/project\([^)]*VERSION\s+(\d+\.\d+\.\d+)/)
  const semver = m?.[1] ?? '0.0.0'
  // Mirror CMakeLists.txt: ${PROJECT_VERSION}.${git rev-list --count HEAD}.
  // Without this, the header stays pinned to X.Y.Z while the WASM blob
  // ships every commit — users see the same version even after a rebuild.
  // We read the build number from the generated src/version.hpp (CMake
  // already populated it from `git rev-list --count HEAD`) instead of
  // shelling out, so vite needn't spawn git itself.
  let full = semver
  const versionHpp = path.resolve(__dirname, '../build/generated/version.hpp')
  if (fs.existsSync(versionHpp)) {
    const txt = fs.readFileSync(versionHpp, 'utf8')
    const m2 = txt.match(/version\s*=\s*"(\d+\.\d+\.\d+\.\d+)"/)
    if (m2 && m2[1]) full = m2[1]
  }
  return full
}

export default defineConfig({
  plugins: [vue()],
  define: {
    __APP_VERSION__: JSON.stringify(readProjectVersion()),
  },
  resolve: {
    alias: {
      '@wasm': path.resolve(__dirname, '../build-wasm')
    }
  },
  server: {
    fs: {
      allow: ['..']
    },
    // pthreads + SharedArrayBuffer require cross-origin isolation in
    // the browser. Production nginx already sets these (see
    // service/nginx.conf). Mirror them for `vite dev` so the dev
    // workflow can exercise the WASM threads. COEP=credentialless
    // matches prod so cross-origin scripts without an explicit
    // Cross-Origin-Resource-Policy still load.
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'credentialless',
      'Cross-Origin-Resource-Policy': 'same-origin',
    },
  },
  assetsInclude: ['**/*.wasm'],
  build: {
    outDir: '../service/html',
    emptyOutDir: true,
    // Emscripten's pthread glue uses top-level await; bump the build
    // target past the default es2020 to allow it. Also bumps the
    // browser baseline to versions that ship SharedArrayBuffer +
    // cross-origin-isolation (which is what pthreads need anyway):
    // Chrome 89+, Firefox 89+, Safari 15+, Edge 89+.
    target: ['chrome89', 'edge89', 'firefox89', 'safari15'],
  },
  worker: {
    format: 'es',
    // Same reason — worker chunks also load TLA-using glue.
    rollupOptions: { output: { format: 'es' } },
  },
})
