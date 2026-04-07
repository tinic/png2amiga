import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import path from 'path'

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@wasm': path.resolve(__dirname, '../build-wasm')
    }
  },
  server: {
    fs: {
      allow: ['..']
    }
  },
  assetsInclude: ['**/*.wasm'],
  build: {
    outDir: '../docs',
    emptyOutDir: true,
  }
})
