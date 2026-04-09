import { ref } from 'vue'
import WasmWorker from '../workers/wasm.worker.js?worker'

let worker = null
let nextId = 0
const pending = new Map()

export function useWasm() {
  const loading = ref(true)
  const error = ref('')

  if (!worker) {
    worker = new WasmWorker()
    worker.onmessage = (e) => {
      const msg = e.data
      if (msg.type === 'ready') {
        loading.value = false
        return
      }
      if (msg.type === 'error') {
        error.value = msg.error
        loading.value = false
        return
      }
      const cb = pending.get(msg.id)
      if (cb) {
        pending.delete(msg.id)
        if (msg.error) {
          cb.reject(new Error(msg.error))
        } else {
          // Reconstruct typed arrays from transferred buffers
          const r = msg.result
          if (r.rgba) r.rgba = new Uint8Array(r.rgba)
          if (r.data) r.data = new Uint8Array(r.data)
          cb.resolve(r)
        }
      }
    }
  }

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
