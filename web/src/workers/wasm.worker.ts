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
  hasTransparency?: boolean
  genesisUniqueTiles?: number
  genesisTotalCells?: number
  error?: string
  rgba?: ArrayBuffer
  data?: ArrayBuffer
  header?: string
}

let Module: Png2AmigaModule | null = null
let initError: string | null = null

async function init(): Promise<void> {
  try {
    const { default: createPng2Amiga } = await import('@wasm/png2amiga.js')
    Module = await createPng2Amiga({
      locateFile: (path: string) => {
        if (path.endsWith('.wasm')) {
          return new URL('../../../build-wasm/png2amiga.wasm', import.meta.url).href
        }
        return path
      },
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
    ...opt('hasTransparency', result.hasTransparency),
    ...opt('genesisUniqueTiles', result.genesisUniqueTiles),
    ...opt('genesisTotalCells', result.genesisTotalCells),
    ...opt('error', result.error),
  }
  const transfers: ArrayBuffer[] = []

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
// eslint-disable-next-line sonarjs/post-message
globalThis.addEventListener('message', (e: MessageEvent<IncomingMessage>) => {
  void handleMessage(e)
})
