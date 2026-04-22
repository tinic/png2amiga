import { ref } from 'vue'
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
  worker.onmessage = (e) => {
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
  }
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

  function call(fn, ...args) {
    const id = nextId++
    return new Promise((resolve, reject) => {
      pending.set(id, { resolve, reject })
      worker.postMessage({ id, fn, args })
    })
  }

  function convertRGBA(imageBytes, options) {
    return call('convertRGBA', imageBytes, options)
  }

  function convertPNG(imageBytes, options) {
    return call('convert', imageBytes, options)
  }

  function convertIFF(imageBytes, options) {
    return call('convertIFF', imageBytes, options)
  }

  function convertHeader(imageBytes, options, name) {
    return call('convertHeader', imageBytes, options, name)
  }

  function convertViewer(imageBytes, options) {
    return call('convertViewer', imageBytes, options)
  }

  function convertDegas(imageBytes, options) {
    return call('convertDegas', imageBytes, options)
  }

  function convertRaw(imageBytes, options) {
    return call('convertRaw', imageBytes, options)
  }

  function convertMask(imageBytes, options) {
    return call('convertMask', imageBytes, options)
  }

  function convertMaskRaw(imageBytes, options) {
    return call('convertMaskRaw', imageBytes, options)
  }

  return { loading, error, convertRGBA, convertPNG, convertIFF, convertHeader, convertViewer, convertDegas, convertRaw, convertMask, convertMaskRaw }
}
