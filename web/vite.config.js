import path from 'node:path'
import fs from 'node:fs'

import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

function readProjectVersion() {
  const cmake = fs.readFileSync(path.resolve(__dirname, '../CMakeLists.txt'), 'utf8')
  const m = cmake.match(/project\([^)]*VERSION\s+(\d+\.\d+\.\d+)/)
  return m ? m[1] : '0.0.0'
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
