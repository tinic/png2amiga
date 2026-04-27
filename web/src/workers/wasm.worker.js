// Web Worker: runs png2amiga WASM off the main thread.
// Messages: { id, fn, args } -> { id, result } | { id, error }

let Module = null
let initError = null

async function init() {
  try {
    const { default: createPng2Amiga } = await import('@wasm/png2amiga.js')
    Module = await createPng2Amiga({
      locateFile: (path) => {
        if (path.endsWith('.wasm')) {
          return new URL('../../../build-wasm/png2amiga.wasm', import.meta.url).href
        }
        return path
      },
    })
    self.postMessage({ type: 'ready' })
  } catch (error) {
    initError = error.message || String(error)
    self.postMessage({ type: 'error', error: initError })
  }
}

const initPromise = init()

// Dispatch table: each entry is the WASM binding name + argv shape. Most take
// (bytes, options); convertHeader takes a third symbolName arg.
const DISPATCHERS = {
  convertRGBA:    (m, args) => m.convertRGBA(args[0], args[1]),
  convert:        (m, args) => m.convert(args[0], args[1]),
  convertIFF:     (m, args) => m.convertIFF(args[0], args[1]),
  convertHeader:  (m, args) => m.convertHeader(args[0], args[1], args[2]),
  convertViewer:  (m, args) => m.convertViewer(args[0], args[1]),
  convertDegas:   (m, args) => m.convertDegas(args[0], args[1]),
  convertRaw:     (m, args) => m.convertRaw(args[0], args[1]),
  convertMask:    (m, args) => m.convertMask(args[0], args[1]),
  convertMaskRaw: (m, args) => m.convertMaskRaw(args[0], args[1]),
}

function injectProgressCallback(args, id, wantProgress) {
  // Embind passes JS functions to C++ via emscripten::val; the WASM encoder
  // calls back into this worker thread. Each call posts {type:'progress'} to
  // the main thread, throttled main-side so we don't bog down the UI on a
  // torrent of ticks.
  if (!wantProgress || !args[1] || typeof args[1] !== 'object') return args
  const next = [...args]
  next[1] = {
    ...args[1],
    onProgress: (p, stage) => {
      self.postMessage({ type: 'progress', id, p, stage })
    },
  }
  return next
}

globalThis.addEventListener('message', async (e) => {
  const { id, fn, args, wantProgress } = e.data
  try {
    await initPromise
    if (!Module) throw new Error(initError || 'WASM module not loaded')

    const argv = injectProgressCallback(args, id, wantProgress)

    // ditherDefaults is a synchronous one-shot lookup that returns
    // { strength, errorClamp } directly — no transfer-buffer plumbing.
    if (fn === 'ditherDefaults') {
      self.postMessage({ id, result: Module.ditherDefaults(argv[0]) })
      return
    }

    const dispatch = DISPATCHERS[fn]
    if (!dispatch) throw new Error(`Unknown function: ${fn}`)
    const result = dispatch(Module, argv)

    // Build plain object reply with transferable buffers
    const reply = {
      width: result.width,
      height: result.height,
      depth: result.depth,
      colors: result.colors,
      copperChanges: result.copperChanges,
      changesPerLine: result.changesPerLine,
      maxMovesPerLine: result.maxMovesPerLine,
      aga: result.aga,
      totalColors: result.totalColors,
      planeBytes: result.planeBytes,
      copperBytes: result.copperBytes,
      diskBytes: result.diskBytes,
      chipBytes: result.chipBytes,
      quantError: result.quantError,
      psnr: result.psnr,
      hasTransparency: result.hasTransparency,
      error: result.error,
    }
    const transfers = []

    if (result.rgba) {
      const arr = new Uint8Array(result.rgba)
      const buf = arr.buffer.slice(arr.byteOffset, arr.byteOffset + arr.byteLength)
      reply.rgba = buf
      transfers.push(buf)
    }
    if (result.data) {
      const arr = new Uint8Array(result.data)
      const buf = arr.buffer.slice(arr.byteOffset, arr.byteOffset + arr.byteLength)
      reply.data = buf
      transfers.push(buf)
    }
    if (result.header) {
      reply.header = result.header
    }

    self.postMessage({ id, result: reply }, transfers)
  } catch (error) {
    self.postMessage({ id, error: error.message || String(error) })
  }
})
