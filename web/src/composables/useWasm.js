import { ref } from 'vue'

// eslint-disable-next-line import-x/default -- `?worker` import is provided by Vite at build time; declared in src/vite-env.d.ts
import WasmWorker from '../workers/wasm.worker.js?worker'

let worker = null
let nextId = 0
const pending = new Map()

// Shared across all useWasm() calls. `loading` flips to false once the worker
// posts {type:'ready'} after WASM instantiation; prewarmWasm() below sets up
// that listener before Vue mounts so the user sees live preview faster.
const sharedLoading = ref(true)
const sharedError = ref('')

function ensureWorker() {
  if (worker) return
  worker = new WasmWorker()
  worker.addEventListener('message', (e) => {
    const msg = e.data
    if (msg.type === 'ready') {
      sharedLoading.value = false
      return
    }
    if (msg.type === 'error') {
      sharedError.value = msg.error
      sharedLoading.value = false
      return
    }
    if (msg.type === 'progress') {
      const cb = pending.get(msg.id)
      if (cb && cb.onProgress) cb.onProgress(msg.p, msg.stage)
      return
    }
    const cb = pending.get(msg.id)
    if (cb) {
      pending.delete(msg.id)
      if (msg.error) {
        cb.reject(new Error(msg.error))
      } else {
        const r = msg.result
        if (r.rgba) r.rgba = new Uint8Array(r.rgba)
        if (r.data) r.data = new Uint8Array(r.data)
        cb.resolve(r)
      }
    }
  })
}

// Call this as early as possible (e.g. from main.js before app.mount) so the
// worker — and therefore the WASM fetch + streaming compile — starts in
// parallel with Vue/PrimeVue bootstrapping, rather than after Converter.vue's
// setup runs. Idempotent; safe to call multiple times.
export function prewarmWasm() { ensureWorker() }

export function useWasm() {
  const loading = sharedLoading
  const error = sharedError
  ensureWorker()

  // The optional `onProgress` arg (last position on each wrapper) lets
  // callers receive progress ticks from the encoder during long operations
  // (HAM6 + --ham-cap-best, large beam widths, etc.). The worker injects a
  // JS callback into the WASM options at args[1]; ticks come back as
  // {type:'progress'} messages and are routed by id.
  function call(fn, args, onProgress) {
    const id = nextId++
    return new Promise((resolve, reject) => {
      pending.set(id, { resolve, reject, onProgress })
      worker.postMessage({ id, fn, args, wantProgress: Boolean(onProgress) })
    })
  }

  function convertRGBA(imageBytes, options, onProgress) {
    return call('convertRGBA', [imageBytes, options], onProgress)
  }

  function convertPNG(imageBytes, options, onProgress) {
    return call('convert', [imageBytes, options], onProgress)
  }

  function convertIFF(imageBytes, options, onProgress) {
    return call('convertIFF', [imageBytes, options], onProgress)
  }

  function convertHeader(imageBytes, options, name, onProgress) {
    return call('convertHeader', [imageBytes, options, name], onProgress)
  }

  function convertViewer(imageBytes, options, onProgress) {
    return call('convertViewer', [imageBytes, options], onProgress)
  }

  function convertDegas(imageBytes, options, onProgress) {
    return call('convertDegas', [imageBytes, options], onProgress)
  }

  function convertRaw(imageBytes, options, onProgress) {
    return call('convertRaw', [imageBytes, options], onProgress)
  }

  function convertMask(imageBytes, options, onProgress) {
    return call('convertMask', [imageBytes, options], onProgress)
  }

  function convertMaskRaw(imageBytes, options, onProgress) {
    return call('convertMaskRaw', [imageBytes, options], onProgress)
  }

  // Returns the per-mode dither defaults the C++ encoder would use:
  // { strength, errorClamp }. Used to refresh the UI on mode change.
  function ditherDefaults(options) {
    return call('ditherDefaults', [options])
  }

  return { loading, error, convertRGBA, convertPNG, convertIFF, convertHeader, convertViewer, convertDegas, convertRaw, convertMask, convertMaskRaw, ditherDefaults }
}
