import { ref, type Ref } from 'vue'
import type { ConvertResult, DitherDefaults, WasmOptions } from '@wasm/png2amiga.js'

import WasmWorker from '../workers/wasm.worker.js?worker'


type ProgressCallback = (p: number, stage: string) => void

interface PendingEntry {
  resolve: (result: ConvertResult) => void
  reject: (err: Error) => void
  onProgress?: ProgressCallback
}

interface DitherDefaultsPendingEntry {
  resolve: (result: DitherDefaults) => void
  reject: (err: Error) => void
}

type WorkerOutboundMessage =
  | { id: number; fn: string; args: unknown[]; wantProgress: boolean }

// Shape of the worker's reply envelope. `rgba` / `data` cross the
// thread boundary as ArrayBuffer; we wrap them as Uint8Array on this side.
interface ReplyEnvelope extends Omit<ConvertResult, 'rgba' | 'data'> {
  rgba?: ArrayBuffer
  data?: ArrayBuffer
}

type WorkerInboundMessage =
  | { type: 'ready' }
  | { type: 'error'; error: string }
  | { type: 'progress'; id: number; p: number; stage: string }
  | { id: number; result: ReplyEnvelope }
  | { id: number; result: DitherDefaults }
  | { id: number; error: string }

let worker: Worker | null = null
let nextId = 0
const pending = new Map<number, PendingEntry | DitherDefaultsPendingEntry>()

// Shared across all useWasm() calls. `loading` flips to false once the worker
// posts {type:'ready'} after WASM instantiation; prewarmWasm() below sets up
// that listener before Vue mounts so the user sees live preview faster.
const sharedLoading = ref(true)
const sharedError = ref('')

function unwrapConvertEnvelope(raw: ReplyEnvelope): ConvertResult {
  // Conditional spreads (rather than `rgba: ... ?? undefined`) so we don't
  // explicitly set optional keys to undefined under exactOptionalPropertyTypes.
  const { rgba, data, ...rest } = raw
  return {
    ...rest,
    ...(rgba ? { rgba: new Uint8Array(rgba) } : {}),
    ...(data ? { data: new Uint8Array(data) } : {}),
  }
}

function handleProgress(id: number, p: number, stage: string): void {
  const cb = pending.get(id)
  if (cb && 'onProgress' in cb && cb.onProgress) cb.onProgress(p, stage)
}

function handleReply(id: number, result: ReplyEnvelope | DitherDefaults): void {
  const cb = pending.get(id)
  if (!cb) return
  pending.delete(id)
  if ('width' in result) {
    ;(cb.resolve as (r: ConvertResult) => void)(unwrapConvertEnvelope(result))
  } else {
    ;(cb.resolve as (r: DitherDefaults) => void)(result)
  }
}

function handleError(id: number, error: string): void {
  const cb = pending.get(id)
  if (!cb) return
  pending.delete(id)
  cb.reject(new Error(error))
}

function handleStatus(msg: { type: string } & Partial<{ id: number; error: string; p: number; stage: string }>): void {
  if (msg.type === 'ready')    { sharedLoading.value = false; return }
  if (msg.type === 'error')    { sharedError.value = msg.error ?? ''; sharedLoading.value = false; return }
  if (msg.type === 'progress') handleProgress(msg.id ?? -1, msg.p ?? 0, msg.stage ?? '')
}

function handleMessage(msg: WorkerInboundMessage): void {
  if ('type' in msg) { handleStatus(msg); return }
  if ('error' in msg) handleError(msg.id, msg.error)
  else handleReply(msg.id, msg.result)
}

function ensureWorker(): void {
  if (worker) return
  worker = new WasmWorker()
  worker.addEventListener('message', (e: MessageEvent<WorkerInboundMessage>) => {
    handleMessage(e.data)
  })
}

// Call this as early as possible (e.g. from main.ts before app.mount) so the
// worker — and therefore the WASM fetch + streaming compile — starts in
// parallel with Vue/PrimeVue bootstrapping, rather than after Converter.vue's
// setup runs. Idempotent; safe to call multiple times.
export function prewarmWasm(): void { ensureWorker() }

export interface UseWasmReturn {
  loading: Ref<boolean>
  error: Ref<string>
  convertRGBA: (bytes: Uint8Array, opts: WasmOptions, onProgress?: ProgressCallback) => Promise<ConvertResult>
  convertPNG: (bytes: Uint8Array, opts: WasmOptions, onProgress?: ProgressCallback) => Promise<ConvertResult>
  convertIFF: (bytes: Uint8Array, opts: WasmOptions, onProgress?: ProgressCallback) => Promise<ConvertResult>
  convertHeader: (bytes: Uint8Array, opts: WasmOptions, name: string, onProgress?: ProgressCallback) => Promise<ConvertResult>
  convertViewer: (bytes: Uint8Array, opts: WasmOptions, onProgress?: ProgressCallback) => Promise<ConvertResult>
  convertDegas: (bytes: Uint8Array, opts: WasmOptions, onProgress?: ProgressCallback) => Promise<ConvertResult>
  convertRaw: (bytes: Uint8Array, opts: WasmOptions, onProgress?: ProgressCallback) => Promise<ConvertResult>
  convertMask: (bytes: Uint8Array, opts: WasmOptions, onProgress?: ProgressCallback) => Promise<ConvertResult>
  convertMaskRaw: (bytes: Uint8Array, opts: WasmOptions, onProgress?: ProgressCallback) => Promise<ConvertResult>
  ditherDefaults: (opts: WasmOptions) => Promise<DitherDefaults>
}

export function useWasm(): UseWasmReturn {
  const loading = sharedLoading
  const error = sharedError
  ensureWorker()

  // The optional `onProgress` arg (last position on each wrapper) lets
  // callers receive progress ticks from the encoder during long operations
  // (HAM6 + --ham-cap-best, large beam widths, etc.). The worker injects a
  // JS callback into the WASM options at args[1]; ticks come back as
  // {type:'progress'} messages and are routed by id.
  function postWorker(msg: WorkerOutboundMessage): void {
    if (!worker) throw new Error('worker not initialised')
    worker.postMessage(msg)
  }

  function callConvert(fn: string, args: unknown[], onProgress?: ProgressCallback): Promise<ConvertResult> {
    const id = nextId++
    return new Promise((resolve, reject) => {
      pending.set(id, onProgress ? { resolve, reject, onProgress } : { resolve, reject })
      postWorker({ id, fn, args, wantProgress: Boolean(onProgress) })
    })
  }

  function callDitherDefaults(args: unknown[]): Promise<DitherDefaults> {
    const id = nextId++
    return new Promise((resolve, reject) => {
      pending.set(id, { resolve, reject })
      postWorker({ id, fn: 'ditherDefaults', args, wantProgress: false })
    })
  }

  return {
    loading,
    error,
    convertRGBA:    (b, o, p) => callConvert('convertRGBA',    [b, o],    p),
    convertPNG:     (b, o, p) => callConvert('convert',        [b, o],    p),
    convertIFF:     (b, o, p) => callConvert('convertIFF',     [b, o],    p),
    convertHeader:  (b, o, n, p) => callConvert('convertHeader', [b, o, n], p),
    convertViewer:  (b, o, p) => callConvert('convertViewer',  [b, o],    p),
    convertDegas:   (b, o, p) => callConvert('convertDegas',   [b, o],    p),
    convertRaw:     (b, o, p) => callConvert('convertRaw',     [b, o],    p),
    convertMask:    (b, o, p) => callConvert('convertMask',    [b, o],    p),
    convertMaskRaw: (b, o, p) => callConvert('convertMaskRaw', [b, o],    p),
    // Returns the per-mode dither defaults the C++ encoder would use.
    ditherDefaults: (o)       => callDitherDefaults([o]),
  }
}
