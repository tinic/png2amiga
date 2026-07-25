// Web Worker: runs png2amiga WASM off the main thread.
// Messages: { id, fn, args } -> { id, result } | { id, error }

import type {
  ConvertResult,
  Png2AmigaModule,
  WasmOptions,
} from '@wasm/png2amiga.js'

interface IncomingMessage {
  id: number
  fn: string
  args: unknown[]
  wantProgress: boolean
}

interface ReplyEnvelope {
  width: number
  height: number
  depth: number
  colors: number
  copperChanges?: number
  changesPerLine?: number
  maxMovesPerLine?: number
  aga?: boolean
  totalColors?: number
  planeBytes?: number
  copperBytes?: number
  diskBytes?: number
  chipBytes?: number
  quantError?: number
  psnr?: number
  s2?: number
  hasTransparency?: boolean
  genesisUniqueTiles?: number
  genesisTotalCells?: number
  tileDataBytes?: number
  c64CharsetData?: ArrayBuffer
  c64Mc1?: number
  c64Mc2?: number
  c64BgColor?: number
  genesisTileBytes?: ArrayBuffer
  genesisTilemapBytes?: ArrayBuffer
  genesisPaletteBytes?: ArrayBuffer
  snesTileBytes?: ArrayBuffer
  snesTilemapBytes?: ArrayBuffer
  snesPaletteBytes?: ArrayBuffer
  paletteBytes?: ArrayBuffer
  indices?: ArrayBuffer
  scanlinePaletteBytes?: ArrayBuffer
  scanlinePaletteSize?: number
  error?: string
  rgba?: ArrayBuffer
  data?: ArrayBuffer
  header?: string
}

let Module: Png2AmigaModule | null = null
let initError: string | null = null

// A 24-byte WASM module whose only function body is
// `i32.const 0; i8x16.splat; drop` — the canonical SIMD probe (same bytes
// wasm-feature-detect uses). WebAssembly.validate() rejects it when the
// engine has the SIMD proposal disabled, which V8 and SpiderMonkey do on
// CPUs without SSE4.1 — pre-Penryn Intel / pre-Bulldozer AMD, current
// browser or not. On those the SIMD module fails at instantiation, before
// any of our code runs, and the app never becomes ready. Hence the scalar
// twin (CMakeLists.txt → png2amiga_wasm_nosimd).
const WASM_SIMD_PROBE = new Uint8Array([
  0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
  0x03, 0x02, 0x01, 0x00,
  0x0A, 0x09, 0x01, 0x07, 0x00, 0x41, 0x00, 0xFD, 0x0F, 0x1A, 0x0B,
])

function hasWasmSimd(): boolean {
  try {
    return WebAssembly.validate(WASM_SIMD_PROBE)
  } catch {
    return false
  }
}

async function init(): Promise<void> {
  try {
    // Resolve the JS module URL up front so Emscripten's pthread shim
    // can spawn child workers from it. Without mainScriptUrlOrBlob
    // the pthread workers try to load `./png2amiga.js` relative to
    // the parent worker's blob URL → 404 → silent "worker sent an
    // error! undefined" in the console and the module never resolves.
    //
    // Both new URL() argument pairs have to stay string literals — Vite
    // rewrites them to hashed asset URLs at build time. Same for the two
    // import() specifiers: the branch not taken is still bundled, but only
    // the taken one is fetched.
    const simd = hasWasmSimd()
    const wasmJsUrl = simd
      ? new URL('../../../build-wasm/png2amiga.js', import.meta.url).href
      : new URL('../../../build-wasm/png2amiga-nosimd.js', import.meta.url).href
    const wasmBinUrl = simd
      ? new URL('../../../build-wasm/png2amiga.wasm', import.meta.url).href
      : new URL('../../../build-wasm/png2amiga-nosimd.wasm', import.meta.url).href
    const { default: createPng2Amiga } = simd
      ? await import('@wasm/png2amiga.js')
      : await import('@wasm/png2amiga-nosimd.js')
    if (!simd) {
      console.warn('png2amiga: WASM SIMD unavailable — loading scalar build (slower)')
    }
    Module = await createPng2Amiga({
      locateFile: (path: string) => {
        if (path.endsWith('.wasm')) return wasmBinUrl
        if (path.endsWith('.js'))   return wasmJsUrl
        return path
      },
      mainScriptUrlOrBlob: wasmJsUrl,
    })
    self.postMessage({ type: 'ready' })
  } catch (error) {
    initError = error instanceof Error ? error.message : String(error)
    self.postMessage({ type: 'error', error: initError })
  }
}

const initPromise = init()

// Dispatch table: each entry is the WASM binding name + argv shape. Most take
// (bytes, options); convertHeader takes a third symbolName arg.
type Dispatcher = (m: Png2AmigaModule, args: unknown[]) => ConvertResult

const DISPATCHERS: Record<string, Dispatcher> = {
  convertRGBA:    (m, args) => m.convertRGBA(args[0] as Uint8Array, args[1] as WasmOptions),
  convert:        (m, args) => m.convert(args[0] as Uint8Array, args[1] as WasmOptions),
  convertIFF:     (m, args) => m.convertIFF(args[0] as Uint8Array, args[1] as WasmOptions),
  convertHeader:  (m, args) => m.convertHeader(args[0] as Uint8Array, args[1] as WasmOptions, args[2] as string),
  convertViewer:  (m, args) => m.convertViewer(args[0] as Uint8Array, args[1] as WasmOptions),
  convertDegas:   (m, args) => m.convertDegas(args[0] as Uint8Array, args[1] as WasmOptions),
  convertRaw:     (m, args) => m.convertRaw(args[0] as Uint8Array, args[1] as WasmOptions),
  convertPRG:     (m, args) => m.convertPRG(args[0] as Uint8Array, args[1] as WasmOptions),
  convertKoa:     (m, args) => m.convertKoa(args[0] as Uint8Array, args[1] as WasmOptions),
  convertHir:     (m, args) => m.convertHir(args[0] as Uint8Array, args[1] as WasmOptions),
  convertMask:    (m, args) => m.convertMask(args[0] as Uint8Array, args[1] as WasmOptions),
  convertMaskRaw: (m, args) => m.convertMaskRaw(args[0] as Uint8Array, args[1] as WasmOptions),
}

function injectProgressCallback(args: unknown[], id: number, wantProgress: boolean): unknown[] {
  // Embind passes JS functions to C++ via emscripten::val; the WASM encoder
  // calls back into this worker thread. Each call posts {type:'progress'} to
  // the main thread, throttled main-side so we don't bog down the UI on a
  // torrent of ticks.
  const opts = args[1]
  if (!wantProgress || !opts || typeof opts !== 'object') return args
  const next = [...args]
  next[1] = {
    ...(opts as WasmOptions),
    onProgress: (p: number, stage: string) => {
      self.postMessage({ type: 'progress', id, p, stage })
    },
  } satisfies WasmOptions
  return next
}

function buildReply(result: ConvertResult): { reply: ReplyEnvelope; transfers: ArrayBuffer[] } {
  // Build the envelope by spreading optional fields conditionally — under
  // exactOptionalPropertyTypes, we can't write `key: undefined` for an
  // optional field, so include only present keys.
  const opt = <K extends string, V>(k: K, v: V | undefined): Partial<Record<K, V>> =>
    v === undefined ? {} : ({ [k]: v } as Record<K, V>)
  const reply: ReplyEnvelope = {
    width: result.width,
    height: result.height,
    depth: result.depth,
    colors: result.colors,
    ...opt('copperChanges', result.copperChanges),
    ...opt('changesPerLine', result.changesPerLine),
    ...opt('maxMovesPerLine', result.maxMovesPerLine),
    ...opt('aga', result.aga),
    ...opt('totalColors', result.totalColors),
    ...opt('planeBytes', result.planeBytes),
    ...opt('copperBytes', result.copperBytes),
    ...opt('diskBytes', result.diskBytes),
    ...opt('chipBytes', result.chipBytes),
    ...opt('quantError', result.quantError),
    ...opt('psnr', result.psnr),
    ...opt('s2', result.s2),
    ...opt('hasTransparency', result.hasTransparency),
    ...opt('genesisUniqueTiles', result.genesisUniqueTiles),
    ...opt('genesisTotalCells', result.genesisTotalCells),
    ...opt('tileDataBytes', result.tileDataBytes),
    ...opt('c64Mc1', result.c64Mc1),
    ...opt('c64Mc2', result.c64Mc2),
    ...opt('c64BgColor', result.c64BgColor),
    ...opt('scanlinePaletteSize', result.scanlinePaletteSize),
    ...opt('error', result.error),
  }
  const transfers: ArrayBuffer[] = []

  const forwardArrayBuffer = (
      src: Uint8Array | ArrayBuffer | undefined,
      dst: 'c64CharsetData' | 'genesisTileBytes' | 'genesisTilemapBytes'
        | 'genesisPaletteBytes' | 'snesTileBytes' | 'snesTilemapBytes'
        | 'snesPaletteBytes' | 'paletteBytes' | 'indices'
        | 'scanlinePaletteBytes'
  ): void => {
    if (!src) return
    const arr = new Uint8Array(src as ArrayBuffer)
    const buf = arr.buffer.slice(arr.byteOffset, arr.byteOffset + arr.byteLength)
    reply[dst] = buf
    transfers.push(buf)
  }
  forwardArrayBuffer(result.c64CharsetData, 'c64CharsetData')
  forwardArrayBuffer(result.genesisTileBytes, 'genesisTileBytes')
  forwardArrayBuffer(result.genesisTilemapBytes, 'genesisTilemapBytes')
  forwardArrayBuffer(result.genesisPaletteBytes, 'genesisPaletteBytes')
  forwardArrayBuffer(result.snesTileBytes, 'snesTileBytes')
  forwardArrayBuffer(result.snesTilemapBytes, 'snesTilemapBytes')
  forwardArrayBuffer(result.snesPaletteBytes, 'snesPaletteBytes')
  forwardArrayBuffer(result.paletteBytes, 'paletteBytes')
  forwardArrayBuffer(result.indices, 'indices')
  forwardArrayBuffer(result.scanlinePaletteBytes, 'scanlinePaletteBytes')

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
  return { reply, transfers }
}

async function handleMessage(e: MessageEvent<IncomingMessage>): Promise<void> {
  const { id, fn, args, wantProgress } = e.data
  try {
    await initPromise
    if (!Module) throw new Error(initError ?? 'WASM module not loaded')

    const argv = injectProgressCallback(args, id, wantProgress)

    // ditherDefaults is a synchronous one-shot lookup that returns
    // { strength, errorClamp } directly — no transfer-buffer plumbing.
    if (fn === 'ditherDefaults') {
      self.postMessage({ id, result: Module.ditherDefaults(argv[0] as string) })
      return
    }

    const dispatch = DISPATCHERS[fn]
    if (!dispatch) throw new Error(`Unknown function: ${fn}`)
    const result = dispatch(Module, argv)

    const { reply, transfers } = buildReply(result)
    self.postMessage({ id, result: reply }, transfers)
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error)
    self.postMessage({ id, error: message })
  }
}

// Origin check is N/A here: this is a same-origin Vite-imported worker
// (`?worker` import in useWasm.ts). The browser only delivers messages
// from the parent realm; sonarjs/post-message can't tell that apart from
// cross-window postMessage flows.
addEventListener('message', (e: MessageEvent<IncomingMessage>) => {
  void handleMessage(e)
})
