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
    // c64 charset diagnostic — concatenated raw_frame bytes:
    // charset (unique_glyphs * 8) + screen (cells) + color (cells).
    c64CharsetData?: Uint8Array
    c64Mc1?: number
    c64Mc2?: number
    c64BgColor?: number
    // Genesis tile diagnostic — 4bpp 8×8 unique tiles + u16 tilemap +
    // 4-line BGR333 palette. tilemap / palette are 2-byte LE per entry.
    genesisTileBytes?: Uint8Array
    genesisTilemapBytes?: Uint8Array
    genesisPaletteBytes?: Uint8Array
    // SNES Mode 7 tile diagnostic — 8bpp 8×8 unique tiles + 128×128
    // 1-byte-per-cell tilemap. snesPaletteBytes is 256×3 RGB for the
    // 256-mode variant, missing for Direct.
    snesTileBytes?: Uint8Array
    snesTilemapBytes?: Uint8Array
    snesPaletteBytes?: Uint8Array
    // Final per-mode palette as sRGB bytes, 3 per entry. Empty if the
    // mode emits per-line / per-tile palettes instead (genesis, snes).
    paletteBytes?: Uint8Array
    // Per-scanline palette swatch grid for sliced / strips / copper-HAM
    // modes. Packed `height × scanlinePaletteSize × 3` sRGB bytes — row y
    // starts at offset `y * scanlinePaletteSize * 3`. Used to render a
    // vertical strip beside the preview showing the palette's per-line
    // evolution. Missing for modes without per-line palettes.
    scanlinePaletteBytes?: Uint8Array
    scanlinePaletteSize?: number
    // Per-pixel palette index map (non-HAM modes that emit a 1:1 index
    // grid). Used by the web swatch's hover-isolate feature so two
    // palette slots with the same rendered RGB (e.g. EHB slot 0
    // black-base vs slot 32 black-halfbrite) can be distinguished.
    // Missing for HAM, sliced, strips, and tile-based modes where no
    // single per-pixel index exists.
    indices?: Uint8Array
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
    hamTriple?: number
    hamFast?: boolean
    quantizer?: string
    refineIterations?: number
    best?: boolean

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
    slicedVerticalDither?: boolean
    dualPlayfield?: boolean
    scap?: boolean
    scapDebug?: boolean
    lockColor0?: boolean
    // Per-slot palette reserves. Each entry removes the slot from the
    // dither candidate set — the encoder never routes image pixels
    // through it, but the slot keeps its color for display (CMAP,
    // runtime needs). Wired by the Reserve palette panel in Advanced
    // settings.
    reserves?: { index: number; r: number; g: number; b: number }[]
    // Per-slot palette locks. Each entry pins palette[index] to the
    // given sRGB color but keeps the slot in the dither candidate
    // set. The web "Reserve palette" panel sends its toggled slots
    // through here (lock semantics — pin across re-encodes, dither
    // still uses them) even though the UI label says "reserve".
    locks?: { index: number; r: number; g: number; b: number }[]
    paletteData?: Uint8Array
    paletteFile?: string

    // CGA text
    cgaTextMetric?: string
    cgaTextKernel?: string

    // C64 / VIC-II palette + metric
    c64Palette?: string
    c64Metric?: string
    c64PetsciiGraphicsOnly?: boolean
    tileBudget?: number
    tileReserve?: number

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
    convertPRG(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertKoa(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertHir(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertMask(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    convertMaskRaw(bytes: Uint8Array, opts: WasmOptions): ConvertResult
    ditherDefaults(method: string): DitherDefaults
  }

  const createPng2Amiga: (options?: Png2AmigaModuleOptions) => Promise<Png2AmigaModule>
  export default createPng2Amiga
}
