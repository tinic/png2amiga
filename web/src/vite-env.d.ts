/// <reference types="vite/client" />

// Vite injects this from CMakeLists.txt's PROJECT_VERSION at build time
// (see vite.config.ts → readProjectVersion()).
declare const __APP_VERSION__: string

// Vite's `?worker` import suffix: gives us a Worker class constructor.
declare module '*?worker' {
  const Worker: new () => Worker
  export default Worker
}

// Embind-generated module from Emscripten. Shape matches src/wasm_bindings.cpp
// exports — keep in sync. Returned objects are plain JS values produced by
// emscripten::val; we treat them as opaque-ish here and let callers narrow.
declare module '@wasm/png2amiga.js' {
  export interface Png2AmigaModuleOptions {
    locateFile?: (path: string) => string
  }

  /** Result shape from convert/convertRGBA/convertIFF/etc. */
  export interface ConvertResult {
    width: number
    height: number
    depth: number
    colors: number
    rgba?: Uint8Array
    data?: Uint8Array
    header?: string
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
    error?: string
  }

  export interface DitherDefaults {
    strength: number
    errorClamp: number
  }

  /** WASM-side options object. Free-form because every encode mode adds
   *  more keys; bindings ignore extras. The onProgress callback is injected
   *  by the worker. */
  export type WasmOptions = Record<string, unknown> & {
    onProgress?: (p: number, stage: string) => void
  }

  export interface Png2AmigaModule {
    convertRGBA(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convert(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertIFF(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertHeader(bytes: Uint8Array, opts: WasmOptions, symbolName: string): ConvertResult
    convertViewer(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertDegas(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertRaw(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertMask(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertMaskRaw(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    ditherDefaults(method: string): DitherDefaults
  }

  const createPng2Amiga: (options?: Png2AmigaModuleOptions) => Promise<Png2AmigaModule>
  export default createPng2Amiga
}
