// Web Worker: runs png2amiga WASM off the main thread.
// Messages: { id, fn, args } -> { id, result } | { id, error }

let Module = null
let ready = false
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
    ready = true
    self.postMessage({ type: 'ready' })
  } catch (e) {
    initError = e.message || String(e)
    self.postMessage({ type: 'error', error: initError })
  }
}

const initPromise = init()

self.onmessage = async (e) => {
  const { id, fn, args } = e.data
  try {
    await initPromise
    if (!Module) throw new Error(initError || 'WASM module not loaded')

    let result
    switch (fn) {
      case 'convertRGBA':
        result = Module.convertRGBA(args[0], args[1])
        break
      case 'convert':
        result = Module.convert(args[0], args[1])
        break
      case 'convertIFF':
        result = Module.convertIFF(args[0], args[1])
        break
      case 'convertHeader':
        result = Module.convertHeader(args[0], args[1], args[2])
        break
      case 'convertViewer':
        result = Module.convertViewer(args[0], args[1])
        break
      case 'convertDegas':
        result = Module.convertDegas(args[0], args[1])
        break
      case 'convertRaw':
        result = Module.convertRaw(args[0], args[1])
        break
      case 'convertMask':
        result = Module.convertMask(args[0], args[1])
        break
      case 'convertMaskRaw':
        result = Module.convertMaskRaw(args[0], args[1])
        break
      default:
        throw new Error(`Unknown function: ${fn}`)
    }

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
  } catch (err) {
    self.postMessage({ id, error: err.message || String(err) })
  }
}
