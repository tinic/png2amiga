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
  worker: {
    format: 'es',
  },
  server: {
    fs: {
      allow: ['..']
    }
  },
  assetsInclude: ['**/*.wasm'],
  build: {
    outDir: '../service/html',
    emptyOutDir: true,
  }
})
