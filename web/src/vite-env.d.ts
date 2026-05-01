/// <reference types="vite/client" />

// Vite injects this from CMakeLists.txt's PROJECT_VERSION at build time
// (see vite.config.ts → readProjectVersion()).
declare const __APP_VERSION__: string

// Shim for Vue Single-File Components — Vite/Vue compile these to a
// DefineComponent at build time, but the TS type system needs to see them
// as importable modules.
declare module '*.vue' {
  import type { DefineComponent } from 'vue'
  const component: DefineComponent<object, object, unknown>
  export default component
}

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
    /** Required by Emscripten's pthread shim when the WASM module itself
     *  runs inside a Web Worker (nested-worker setup). Tells the shim
     *  which JS URL to load when spawning pthread child workers. */
    mainScriptUrlOrBlob?: string
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
    s2?: number
    hasTransparency?: boolean
    genesisUniqueTiles?: number
    genesisTotalCells?: number
    tileDataBytes?: number
    error?: string
  }

  export interface DitherDefaults {
    strength: number
    errorClamp: number
  }

  /** WASM-side options object. Mirrors `parse_js_options` in
   *  src/wasm_bindings.cpp — every key the C++ side reads via
   *  `js_opts.hasOwnProperty(...)`. All fields are optional (the binding
   *  individually checks each one). Extras are silently ignored by C++,
   *  but TS will reject them at the call site so we catch typos here.
   *
   *  The check:bindings build step verifies the *function* set against the
   *  C++ EMSCRIPTEN_BINDINGS block; this interface covers the *parameter*
   *  surface. Keep both in sync when adding a new option. */
  export interface WasmOptions {
    // Mode + chipset
    mode?: string
    chipset?: string
    depth?: number
    interlace?: boolean
    width?: number
    height?: number
    nativePar?: boolean

    // Dithering
    dither?: string
    ditherStrength?: number
    errorClamp?: number

    // HAM
    hamBeam?: number
    hamTriple?: number
    hamFast?: boolean
    quantizer?: string
    refineIterations?: number
    best?: boolean
    bestMetric?: string    // 'msssim' (default) | 'psnr'

    // Color adjustments (OKLab-space)
    gamma?: number
    brightness?: number
    contrast?: number
    saturation?: number
    hueShift?: number
    sharpen?: number
    blackPoint?: number
    whitePoint?: number
    matchRange?: boolean

    // Alpha / transparency
    alphaThreshold?: number
    alphaDither?: string
    alphaDitherStrength?: number
    maskInvert?: boolean

    // Crop
    cropX?: number
    cropY?: number
    cropW?: number
    cropH?: number
    cropAuto?: boolean

    // Copper / palette extras
    copper?: boolean
    copperChanges?: number
    dualPlayfield?: boolean
    scap?: boolean
    scapDebug?: boolean
    reserveColor0?: boolean
    paletteDiversity?: number
    paletteData?: Uint8Array
    paletteFile?: string

    // CGA text
    cgaTextMetric?: string

    // C64 / VIC-II palette + metric
    c64Palette?: string
    c64Metric?: string
    c64PetsciiGraphicsOnly?: boolean

    // C header export
    symbolName?: string

    // Worker injects this so the WASM encoder can post progress ticks
    // back to the main thread.
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
