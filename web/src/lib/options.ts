// Type-side declarations.

export type Chipset = 'ocs' | 'aga' | 'stf' | 'ste' | 'vga' | 'ega' | 'cga' | 'snes' | 'genesis' | 'c64'

export interface ModeOption {
  value: string
  label: string
  chipset: Chipset
}

export interface ChipsetOption {
  value: Chipset
  label: string
}

export interface NamedItem {
  value: string
  label: string
}

export interface DitherGroup {
  group: string
  items: NamedItem[]
}

export interface Slider {
  key: SliderKey | DiffusionSliderKey
  label: string
  min: number
  max: number
  step: number
  default: number
  tip: string
}

export type SliderKey =
  | 'gamma' | 'brightness' | 'contrast' | 'saturation'
  | 'hueShift' | 'sharpen' | 'blackPoint' | 'whitePoint'
// Defaults-only sliders (populated via sliderDefaults() but NOT rendered
// in the Adjustments panel). The auto-tuning table
// (dither_tuning::defaults_for) sets these whenever mode/method changes,
// and they reach the encoder via runConvert's options dict; manual
// override needs CLI / scripted callers.
export type DiffusionSliderKey = 'errorClamp' | 'ditherStrength'

export interface Example {
  name: string
  file: string
  opts?: Partial<Options>
}

export type HamType = 'ham6' | 'ham8' | null

export interface PreviewScale {
  sx: number
  sy: number
}

export interface DecomposedMode {
  mode: string
  width: number
  interlace: boolean
}

// Options shape — Converter.vue mutates this reactively. Slider keys
// (gamma/brightness/etc.) are written via dynamic [key] in defaultOptions();
// the index signature lets that type-check.
export interface Options {
  mode: string
  chipset: Chipset
  depth: number
  interlace: boolean
  copper: boolean
  dither: string
  width: number
  height: number
  best: boolean
  alphaThreshold: number
  alphaDither: string
  alphaDitherStrength: number
  cropX: number
  cropY: number
  cropW: number
  cropH: number
  cropAuto: boolean
  copperChanges: number
  slicedVerticalDither: boolean
  symbolName: string
  maskInvert: boolean
  nativePar: boolean
  lockColor0: boolean
  reserves: { index: number; r: number; g: number; b: number }[]
  dualPlayfield: boolean
  scap: boolean
  cgaTextMetric: string
  cgaTextKernel: string
  cgaCompositeNewCga: boolean
  c64Palette: string
  c64Metric: string
  c64PetsciiGraphicsOnly: boolean
  // Tile-based modes (c64 charset; future SNES / Genesis / Amiga 16x16
  // / PS1 64x64). 0 = mode default (256 for c64 charset).
  tileBudget: number
  tileReserve: number
  matchRange: boolean
  paletteData?: Uint8Array | null
  // Slider numeric fields (declared explicitly so options[s.key] is typed
  // as number rather than the index-signature wildcard).
  gamma: number
  ditherStrength: number
  brightness: number
  contrast: number
  saturation: number
  hueShift: number
  sharpen: number
  blackPoint: number
  whitePoint: number
  errorClamp: number
}

// All modes with chipset availability (ocs = available on both, aga = AGA only)
const ALL_MODES: ModeOption[] = [
  { value: 'lores',            label: 'Lores',                        chipset: 'ocs' },
  { value: 'ehb',              label: 'EHB',                          chipset: 'ocs' },
  { value: 'ham6',             label: 'HAM6',                         chipset: 'ocs' },
  { value: 'ham8',             label: 'HAM8',                         chipset: 'aga' },
  { value: 'hires',            label: 'Hires',                        chipset: 'ocs' },
  { value: 'ham6-hires',       label: 'HAM6 Hires',                   chipset: 'aga' },
  { value: 'ham8-hires',       label: 'HAM8 Hires',                   chipset: 'aga' },
  { value: 'lores-lace',       label: 'Lores Interlace',              chipset: 'ocs' },
  { value: 'ehb-lace',         label: 'EHB Interlace',                chipset: 'ocs' },
  { value: 'ham6-lace',        label: 'HAM6 Interlace',               chipset: 'ocs' },
  { value: 'ham8-lace',        label: 'HAM8 Interlace',               chipset: 'aga' },
  { value: 'hires-lace',       label: 'Hires Interlace',              chipset: 'ocs' },
  { value: 'ham6-hires-lace',  label: 'HAM6 Hires Interlace',         chipset: 'aga' },
  { value: 'ham8-hires-lace',  label: 'HAM8 Hires Interlace',         chipset: 'aga' },
  // Commodore 64 / VIC-II — fixed 16-color palette, per-cell color
  // constraints. Multicolor first / default; sprite / charset modes
  // follow on the merge branch.
  { value: 'c64-multicolor', label: 'Multicolor (160x200, 4/cell)',
                             chipset: 'c64' },
  { value: 'c64-hires',      label: 'Hires (320x200, 2/cell)',
                             chipset: 'c64' },
  { value: 'c64-fli',        label: 'FLI (multicolor + per-row screen)',
                             chipset: 'c64' },
  { value: 'c64-afli',       label: 'AFLI (hires + per-row screen)',
                             chipset: 'c64' },
  { value: 'c64-petscii',    label: 'PETSCII (40x25 text-mode glyphs)',
                             chipset: 'c64' },
  { value: 'c64-charset-hires',
                             label: 'Charset Hires (custom 256-glyph charset)',
                             chipset: 'c64' },
  { value: 'c64-charset-multicolor',
                             label: 'Charset Multicolor (shared mc + per-cell fg)',
                             chipset: 'c64' },
  { value: 'stf-low',          label: 'ST Low (320x200, 16 colors)',   chipset: 'stf' },
  { value: 'stf-med',          label: 'ST Medium (640x200, 4 colors)', chipset: 'stf' },
  { value: 'stf-hi',           label: 'ST High (640x400, mono)',       chipset: 'stf' },
  { value: 'ste-low',          label: 'STE Low (320x200, 16 colors)',  chipset: 'ste' },
  { value: 'ste-med',          label: 'STE Medium (640x200, 4 colors)',chipset: 'ste' },
  { value: 'ste-hi',           label: 'STE High (640x400, mono)',      chipset: 'ste' },
  // IBM PC VGA.
  { value: 'vga-13h',          label: 'Mode 13h (320x200, 256 colors)',chipset: 'vga' },
  { value: 'vga-10h',          label: 'Mode 10h (640x350, 16 colors)', chipset: 'vga' },
  { value: 'vga-12h',          label: 'Mode 12h (640x480, 16 colors)', chipset: 'vga' },
  // IBM PC EGA (fixed 64-color IrgbIRGB gamut, 16 active slots).
  { value: 'ega-320',          label: 'EGA 320x200 (16 colors)',       chipset: 'ega' },
  { value: 'ega-640',          label: 'EGA 640x200 (16 colors)',       chipset: 'ega' },
  { value: 'ega-hi',           label: 'EGA 640x350 (16 colors)',       chipset: 'ega' },
  // IBM PC CGA.
  { value: 'cga-320',          label: 'CGA 320x200',           chipset: 'cga' },
  { value: 'cga-640',          label: 'CGA 640x200 mono',      chipset: 'cga' },
  { value: 'cga-composite-hires', label: 'CGA Comp 640',       chipset: 'cga' },
  { value: 'cga-text80x200',   label: 'CGA Text 80x200', chipset: 'cga' },
  { value: 'cga-text80x100',   label: 'CGA Text 80x100', chipset: 'cga' },
  { value: 'cga-text80x50',    label: 'CGA Text 80x50',  chipset: 'cga' },
  { value: 'cga-text80x25',    label: 'CGA Text 80x25',  chipset: 'cga' },
  { value: 'cga-text40x200',   label: 'CGA Text 40x200', chipset: 'cga' },
  { value: 'cga-text40x100',   label: 'CGA Text 40x100', chipset: 'cga' },
  // SNES Mode 7 — packed tile + tilemap output, ≤ 256 unique tiles via
  // greedy distance-merging when content has more.
  { value: 'snes-mode7-256',    label: 'Mode 7 (256 BGR555 palette)',
                                chipset: 'snes' },
  { value: 'snes-mode7-direct', label: 'Mode 7 Direct (2048-color gamut)',
                                chipset: 'snes' },
  // Sega Genesis / Mega Drive — 8x8 4bpp tiles + 4 palettes × 16 BGR333.
  // -sh modes use the VDP's Shadow/Highlight (priority bit per tile,
  // ~128 effective colors; runtime sets VDP_setHilightShadow(TRUE)).
  { value: 'genesis-h32',    label: 'H32 (256x224)',    chipset: 'genesis' },
  { value: 'genesis-h40',    label: 'H40 (320x224)',    chipset: 'genesis' },
  { value: 'genesis-h32-sh', label: 'H32 + Shadow',     chipset: 'genesis' },
  { value: 'genesis-h40-sh', label: 'H40 + Shadow',     chipset: 'genesis' },
]

// Chipsets whose mode list is exactly `m.chipset === chipset`.
const FIXED_CHIPSETS = new Set<Chipset>(
  ['stf', 'ste', 'vga', 'ega', 'cga', 'snes', 'genesis', 'c64'])

// Filter modes available for a given chipset
export function modesForChipset(chipset: Chipset): ModeOption[] {
  if (FIXED_CHIPSETS.has(chipset)) return ALL_MODES.filter(m => m.chipset === chipset)
  // OCS-tied modes that should be hidden when chipset == aga:
  //   - EHB: 32 base + hardware half-brite generator is OCS-only,
  //     output is OCS-quantised regardless of chipset.
  //   - HAM6: canonical OCS HAM (12-bit base + 4-bit MODIFY); on AGA
  //     HAM8 (24-bit base + 6-bit MODIFY) is strictly better, so the
  //     HAM6 variants just clutter the AGA picker with inferior
  //     choices.
  const HIDE_ON_AGA = new Set([
    'ehb', 'ehb-lace',
    'ham6', 'ham6-lace', 'ham6-hires', 'ham6-hires-lace',
  ])
  return ALL_MODES.filter(m => {
    if (FIXED_CHIPSETS.has(m.chipset)) return false
    if (chipset === 'aga' && HIDE_ON_AGA.has(m.value)) return false
    return chipset === 'aga' || m.chipset === 'ocs'
  })
}

export const MODES: ModeOption[] = ALL_MODES

export const CHIPSETS: ChipsetOption[] = [
  { value: 'ocs',     label: 'Amiga OCS (12-bit)' },
  { value: 'aga',     label: 'Amiga AGA (24-bit)' },
  { value: 'c64',     label: 'Commodore 64 / VIC-II' },
  { value: 'stf',     label: 'Atari STF (9-bit)' },
  { value: 'ste',     label: 'Atari STE (12-bit)' },
  { value: 'vga',     label: 'IBM PC VGA (18-bit DAC)' },
  { value: 'ega',     label: 'IBM PC EGA (6-bit IrgbIRGB)' },
  { value: 'cga',     label: 'IBM PC CGA (fixed palette)' },
  { value: 'snes',    label: 'SNES Mode 7' },
  { value: 'genesis', label: 'Sega Genesis / Mega Drive' },
]

// VIC-II palette options — only meaningful when chipset is 'c64'.
// Default is Colodore (measurement-based; matches png2c64).
export interface C64PaletteOption { value: string; label: string }
export const C64_PALETTES: C64PaletteOption[] = [
  { value: 'colodore', label: 'Colodore (default)' },
  { value: 'pepto',    label: 'Pepto' },
  { value: 'vice',     label: 'VICE emulator' },
  { value: 'deekay',   label: 'Deekay' },
  { value: 'godot',    label: 'Godot' },
  { value: 'c64wiki',  label: 'C64 Wiki' },
  { value: 'levy',     label: 'Levy' },
]

// C64 per-cell error metric. Default = blur — on c64-petscii against
// petsciiator's 57 examples blur+S2-outer scores +10.82 ΔS2 vs +6.66
// for mse+S2-outer, with markedly better detail preservation.
export interface C64MetricOption { value: string; label: string }
export const C64_METRICS: C64MetricOption[] = [
  { value: 'blur', label: 'Blur (Pappas-Neuhoff 3x3, default)' },
  { value: 'mse',  label: 'Per-pixel MSE' },
]

// C64 palette hex values (16 entries each, 0xRRGGBB) — mirrors
// src/palette.hpp's kC64* tables. Used by the charset diagnostic
// renderer so JS can paint glyphs with the same colors the encoder
// chose.
const C64_PALETTE_HEX: Record<string, readonly number[]> = {
  pepto:    [
    0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2,
    0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
    0x6F4F25, 0x433900, 0x9A6759, 0x444444,
    0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
  ],
  vice: [
    0x000000, 0xFDFEFC, 0xBE1A24, 0x30E6C6,
    0xB41AE2, 0x1FD21E, 0x211BAE, 0xDFF60A,
    0xB84104, 0x6A3304, 0xFE4A57, 0x424540,
    0x70746F, 0x59FE59, 0x5F53FE, 0xA4A7A2,
  ],
  colodore: [
    0x000000, 0xFFFFFF, 0x813338, 0x75CEC8,
    0x8E3C97, 0x56AC4D, 0x2E2C9B, 0xEDF171,
    0x8E5029, 0x553800, 0xC46C71, 0x4A4A4A,
    0x7B7B7B, 0xA9FF9F, 0x706DEB, 0xB2B2B2,
  ],
  deekay: [
    0x000000, 0xFFFFFF, 0x882000, 0x68D0A8,
    0xA838A0, 0x50B818, 0x181090, 0xF0E858,
    0xA04800, 0x472B1B, 0xC87870, 0x484848,
    0x808080, 0x98FF98, 0x5090D0, 0xB8B8B8,
  ],
  godot: [
    0x000000, 0xFFFFFF, 0x880000, 0xAAFFEE,
    0xCC44CC, 0x00CC55, 0x0000AA, 0xEEEE77,
    0xDD8855, 0x664400, 0xFF7777, 0x333333,
    0x777777, 0xAAFF66, 0x0088FF, 0xBBBBBB,
  ],
  c64wiki: [
    0x000000, 0xFFFFFF, 0x880000, 0xAAFFEE,
    0xCC44CC, 0x00CC55, 0x0000AA, 0xEEEE77,
    0xDD8855, 0x664400, 0xFF7777, 0x333333,
    0x777777, 0xAAFF66, 0x0088FF, 0xBBBBBB,
  ],
  levy: [
    0x000000, 0xFFFFFF, 0x68372B, 0x70A4B2,
    0x6F3D86, 0x588D43, 0x352879, 0xB8C76F,
    0x6F4F25, 0x433900, 0x9A6759, 0x444444,
    0x6C6C6C, 0x9AD284, 0x6C5EB5, 0x959595,
  ],
}

export function c64PaletteRgb(name: string, idx: number): [number, number, number] {
  const palette = C64_PALETTE_HEX[name] ?? C64_PALETTE_HEX.colodore ?? []
  const v = palette[idx & 0xF] ?? 0
  return [(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF]
}

export const DITHER_METHODS: DitherGroup[] = [
  { group: 'None', items: [
    { value: 'none', label: 'None' },
  ]},
  // Default ED method is plain Floyd-Steinberg. Structure-aware
  // variants intentionally sacrifice PSNR for perceptual quality.
  // Palette-aware methods (yliluoma family / knoll / opt-* / tri-tone)
  // follow.
  { group: 'Error Diffusion', items: [
    { value: 'floyd-steinberg', label: 'Floyd–\nSteinberg' },
    { value: 'sierra-lite',     label: 'Sierra Lite' },
    { value: 'atkinson',        label: 'Atkinson' },
    { value: 'jarvis',          label: 'Jarvis' },
    { value: 'stucki',          label: 'Stucki' },
    { value: 'gilbert',         label: 'Gilbert' },
    { value: 'riemersma',       label: 'Riemersma' },
    { value: 'dbs',             label: 'DBS\n(slow)' },
    { value: 'opt-checker',     label: 'Optimal\nChecker' },
    { value: 'opt-line',        label: 'Optimal\nLine' },
    { value: 'opt-line-checker',label: 'Optimal\nLine-Chk' },
    { value: 'opt-vline',       label: 'Optimal\nVLine' },
    { value: 'opt-vline-checker',label: 'Optimal\nVLine-Chk' },
    { value: 'tri-tone',        label: 'Tri-tone' },
    { value: 'knoll',           label: 'Knoll' },
    // Naming aligns with Yliluoma's article: Algorithm 1 is the exhaustive
    // pair × ratio search; Algorithm 2 is the greedy N=64 plan-builder.
    // Internal IDs (`yliluoma`, `yliluoma2`) predate this clean-up — IDs
    // stay stable (don't break existing CLI/URL state), labels truth-up.
    { value: 'yliluoma1',       label: 'Yliluoma 1' },     // Alg 1 (exhaustive)
    { value: 'yliluoma',        label: 'Yliluoma 2' },     // Alg 2 greedy
    { value: 'yliluoma2',       label: 'Yliluoma 2\nluma' }, // Alg 2 luma-weighted variant
    { value: 'structure-fs',    label: 'Structure\nFS' },
    { value: 'contrast-fs',     label: 'Contrast\nFS' },
    { value: 'zhoufang',        label: 'Zhou–\nFang' },
  ]},
  { group: 'Bayer', items: [
    { value: 'bayer2x2', label: 'Bayer 2×2' },
    { value: 'bayer4x4', label: 'Bayer 4×4' },
    { value: 'bayer8x8', label: 'Bayer 8×8' },
    { value: 'bayer4x2', label: 'Bayer 4×2' },
    { value: 'bayer2x4', label: 'Bayer 2×4' },
    { value: 'bayer3x3', label: 'Bayer 3×3' },
    { value: 'aseprite-old', label: 'Aseprite\nOld 4×4' },
    { value: 'libcaca3', label: 'libcaca\n3×3' },
    { value: 'libcaca6', label: 'libcaca\n6×6' },
    { value: 'pegasus',  label: 'Pegasus\n8×8' },
    { value: 'cranley-bayer', label: 'Cranley\nBayer' },
    { value: 'bayer5x5', label: 'Bayer 5×5' },
    { value: 'bayer6x6', label: 'Bayer 6×6' },
    { value: 'bayer7x7', label: 'Bayer 7×7' },
  ]},
  { group: 'Halftone', items: [
    { value: 'halftone8x8',   label: 'Halftone 8×8' },
    { value: 'diagonal8x8',   label: 'Diagonal\nNewspaper' },
    { value: 'spiral5x5',     label: 'Spiral 5×5' },
    { value: 'clustered-dot', label: 'Clustered\nDot' },
  ]},
  { group: 'Hatching', items: [
    { value: 'line2',         label: 'Lines 2' },
    { value: 'line4',         label: 'Lines 4' },
    { value: 'line8',         label: 'Lines 8' },
    { value: 'line-checker',  label: 'Line\nChecker' },
    { value: 'vline2',        label: 'VLines 2' },
    { value: 'vline4',        label: 'VLines 4' },
    { value: 'vline8',        label: 'VLines 8' },
    { value: 'vline-checker', label: 'VLine\nChecker' },
    { value: 'crosshatch',    label: 'Crosshatch' },
  ]},
  { group: 'Pattern', items: [
    { value: 'checker', label: 'Checker' },
    { value: 'h2x4',    label: 'Wide 2×4' },
    { value: 'v4x2',    label: 'Tall 4×2' },
    { value: 'hex8x8',  label: 'Hexagonal\n8×8' },
    { value: 'hex5x5',  label: 'Hexagonal\n5×5' },
    { value: 'radial',  label: 'Radial' },
    { value: 'quasicrystal',label: 'Quasi\nCrystal' },
    { value: 'truchet', label: 'Truchet' },
  ]},
  { group: 'Noise', items: [
    { value: 'blue-noise',  label: 'Blue\nNoise' },
    { value: 'void-cluster',label: 'Void &\nCluster' },
    { value: 'cluster-noise',label: 'Cluster\nNoise' },
    { value: 'fractal16',   label: 'Fractal\n16×16' },
    { value: 'ign',         label: 'Interleaved\nGradient' },
    { value: 'ign-tri',     label: 'IGN\nTriangle' },
    { value: 'r2',          label: 'R2\nSequence' },
    { value: 'r2-tri',      label: 'R2\nTriangle' },
    { value: 'value-noise', label: 'Value\nNoise' },
    { value: 'white-noise', label: 'White\nNoise' },
  ]},
]

// Dithers that target non-square pixel ratios (C64-lineage 2:1/1:2 cells).
// They produce mis-proportioned output on Amiga square-pixel modes when
// HAM is selected (HAM is square-pixel only); the dither selector hides
// these in HAM modes. Membership-by-value is more stable than by group
// name as the taxonomy evolves.
const NON_SQUARE_DITHERS: ReadonlySet<string> = new Set([
  'h2x4', 'v4x2', 'bayer4x2', 'bayer2x4',
])

export function isNonSquareDither(value: string): boolean {
  return NON_SQUARE_DITHERS.has(value)
}

export const ALPHA_DITHER_METHODS: NamedItem[] = [
  { value: 'none',         label: 'Hard Threshold' },
  { value: 'bayer4x4',    label: 'Bayer 4x4' },
  { value: 'bayer8x8',    label: 'Bayer 8x8' },
  { value: 'checker',     label: 'Checker' },
]

export const SLIDERS: Slider[] = [
  { key: 'gamma',          label: 'Gamma',       min: 0.1, max: 5, step: 0.05, default: 1,
    tip: 'Power curve applied before color matching. >1 darkens midtones, <1 brightens them.' },
  { key: 'brightness',     label: 'Brightness',  min: -1,  max: 1, step: 0.05, default: 0,
    tip: 'Additive lightness shift in perceptual OKLab space.' },
  { key: 'contrast',       label: 'Contrast',    min: 0,   max: 3, step: 0.05, default: 1,
    tip: 'Scale around perceptual mid-gray. 1.0 = no change, >1 increases contrast.' },
  { key: 'saturation',     label: 'Saturation',  min: 0,   max: 3, step: 0.05, default: 1,
    tip: 'Chroma scaling in OKLab space. 0 = grayscale, 1 = original, >1 = boosted color.' },
  { key: 'hueShift',       label: 'Hue',         min: -180, max: 180, step: 1,  default: 0,
    tip: 'Rotate all colors in OKLab. Shifts hues to better match the Amiga palette.' },
  { key: 'sharpen',        label: 'Sharpen',     min: -1,  max: 2, step: 0.05, default: 0,
    tip: 'Negative = blur (reduces noise), positive = sharpen (enhances edges).' },
  { key: 'blackPoint',     label: 'Black Pt',    min: 0,   max: 0.5, step: 0.01, default: 0,
    tip: 'Clip the darkest fraction of the image. Deepens blacks.' },
  { key: 'whitePoint',     label: 'White Pt',    min: 0,   max: 0.5, step: 0.01, default: 0,
    tip: 'Clip the brightest fraction of the image. Cleans up highlights.' },
]

// Defaults-only sliders. The per-mode tuning table
// (dither_tuning::defaults_for) auto-applies the empirical optimum
// whenever mode/method changes — strength and error_clamp are tuned
// per (mode, depth, dpf, scap, copper, chipset, method) bucket so the
// auto values are the right values 99% of the time. The web UI used
// to expose both as sliders; both got removed (errorClamp first, then
// ditherStrength) because the slider invited "fiddle until it looks
// vaguely right" rather than "change mode/method to a better tuned
// preset". CLI / scripted callers can still override via
// --dither-strength / --error-clamp.
export const DIFFUSION_SLIDERS: Slider[] = [
  // Default = -1 sentinel: the C++ encoder (api::run_pipeline) resolves
  // it via dither_tuning::defaults_for(ctx) at entry. Single source of
  // truth for per-mode tuning; web doesn't need its own watcher to
  // refresh these on mode change.
  { key: 'errorClamp',     label: 'Error Clamp', min: -1,  max: 1, step: 0.025, default: -1,
    tip: 'Max error accumulation per channel. -1 = auto-tune via the C++ tuning table.' },
  { key: 'ditherStrength', label: 'Strength',    min: -1,  max: 3, step: 0.05,  default: -1,
    tip: 'Dithering intensity. -1 = auto-tune via the C++ tuning table.' },
]

// CGA-text-mode-only options. Shown only when mode === 'cga-text80x100'.
export const CGA_TEXT_METRICS: NamedItem[] = [
  { value: 'blur', label: 'Pappas-Neuhoff' },
  { value: 'mse',  label: 'Per-pixel MSE' },
]
// Blur-kernel shape — only visible when metric = blur. `auto` resolves
// per cga-text mode using bench-tuned defaults: aniso53 (8×1 cells),
// wide55 (8×2), wide77 (8×4 / 8×8). Manual override exposed for users
// who want to A/B.
export const CGA_TEXT_KERNELS: NamedItem[] = [
  { value: 'auto',     label: 'Auto' },
  { value: 'binomial', label: '3×3 binomial' },
  { value: 'wide55',   label: '5×5 wider Gaussian' },
  { value: 'wide77',   label: '7×7 wider Gaussian' },
  { value: 'aniso53',  label: '5×3 anisotropic horizontal' },
  { value: 'aniso73',  label: '7×3 anisotropic horizontal' },
  { value: 'aniso35',  label: '3×5 anisotropic vertical' },
  { value: 'aniso37',  label: '3×7 anisotropic vertical' },
]
export const CGA_TEXT_DEFAULTS = {
  cgaTextMetric: 'blur',
  cgaTextKernel: 'auto',
}

// CGA composite card revision — shifts the artifact-colour palette.
//   old: 1981 IBM 5150 (default)
//   new: 1983+ revised card (different chroma-burst phase + per-channel
//        intensity term)
export const CGA_COMPOSITE_CARDS = [
  { value: false, label: 'Old (1981 IBM 5150)' },
  { value: true,  label: 'New (1983+ revision)' },
]

// Platform-grouped example sets. Each chipset within a platform group
// shares the same physical image files but each set tags its examples
// with mode + chipset appropriate to that group. Same source assets
// are reused across Amiga / Atari / IBM PC for now; c64 has its own
// curated pack.

// Amiga (ocs + aga) — copper / sliced / HAM modes valid for both.
const AMIGA_EXAMPLES: Example[] = [
  { name: 'makena',   file: 'makena.jpg',   opts: { mode: 'lores', depth: 5, dither: 'floyd-steinberg', ditherStrength: 0.5 } },
  { name: 'fantasy',  file: 'fantasy.jpg',  opts: { mode: 'ham6', dither: 'atkinson', copper: true } },
  { name: 'lovers',   file: 'lovers.jpg',   opts: { mode: 'ehb', dither: 'sierra-lite', copper: true, scap: true } },
  { name: 'logo',     file: 'logo.png',     opts: { mode: 'lores', depth: 5, dither: 'floyd-steinberg', alphaThreshold: 0 } },
  { name: 'space',    file: 'space3.jpg',   opts: { mode: 'lores', depth: 5, dither: 'opt-checker' } },
  { name: 'photo',    file: 'photo.jpg',    opts: { mode: 'ham6', dither: 'atkinson', copper: true } },
  { name: 'grungy',   file: 'grungy.jpg',   opts: { mode: 'lores', depth: 3, dither: 'sierra-lite', ditherStrength: 0.9, brightness: -0.05, contrast: 0.9, gamma: 1, copper: true } },
  { name: 'fantasy1', file: 'fantasy1.jpg', opts: { mode: 'lores', depth: 3, dither: 'floyd-steinberg', copper: true } },
  { name: 'fromthe',  file: 'fromthe.jpg',  opts: { mode: 'lores', depth: 3, dither: 'opt-checker', dualPlayfield: true, scap: true } },
  { name: 'asterix',  file: 'asterix.jpg',  opts: { mode: 'lores', depth: 5, dither: 'floyd-steinberg' } },
]

// Atari (stf + ste) — same source files; mixes stf-low (9-bit palette)
// with ste-low (12-bit), plus a stf-med entry for the high-res 4-color
// mode. Clicking flips chipset to whatever the example specifies;
// users can manually flip stf↔ste after picking an image to compare
// palette precision.
const ATARI_EXAMPLES: Example[] = [
  { name: 'makena',   file: 'makena.jpg',   opts: { chipset: 'stf', mode: 'stf-low' } },
  { name: 'fantasy',  file: 'fantasy.jpg',  opts: { chipset: 'ste', mode: 'ste-low' } },
  { name: 'lovers',   file: 'lovers.jpg',   opts: { chipset: 'ste', mode: 'ste-low' } },
  { name: 'logo',     file: 'logo.png',     opts: { chipset: 'stf', mode: 'stf-low', alphaThreshold: 0 } },
  { name: 'space',    file: 'space3.jpg',   opts: { chipset: 'stf', mode: 'stf-low' } },
  { name: 'photo',    file: 'photo.jpg',    opts: { chipset: 'ste', mode: 'ste-low' } },
  { name: 'grungy',   file: 'grungy.jpg',   opts: { chipset: 'stf', mode: 'stf-med' } },
  { name: 'fantasy1', file: 'fantasy1.jpg', opts: { chipset: 'stf', mode: 'stf-low' } },
  { name: 'fromthe',  file: 'fromthe.jpg',  opts: { chipset: 'ste', mode: 'ste-low' } },
]

// IBM PC (vga + ega + cga) — mix of VGA (256 / 16 color planar), EGA
// (16-of-64), CGA-320 (4-color), CGA-composite (NTSC artifacts), and
// CGA text-mode graphics (80×100 super-chunky). Each example targets a
// mode that fits its content: photos → VGA-13h, low-color art → CGA,
// classic monochrome → CGA-text. Brightness/contrast/gamma tuning
// carries over from the original asterix entry for the CGA-text cases.
const IBM_EXAMPLES: Example[] = [
  { name: 'makena',   file: 'makena.jpg',   opts: { chipset: 'vga', mode: 'vga-13h' } },
  { name: 'fantasy',  file: 'fantasy.jpg',  opts: { chipset: 'ega', mode: 'ega-320' } },
  { name: 'lovers',   file: 'lovers.jpg',   opts: { chipset: 'vga', mode: 'vga-13h' } },
  { name: 'logo',     file: 'logo.png',     opts: { chipset: 'ega', mode: 'ega-320', alphaThreshold: 0 } },
  { name: 'space',    file: 'space3.jpg',   opts: { chipset: 'vga', mode: 'vga-10h' } },
  { name: 'photo',    file: 'photo.jpg',    opts: { chipset: 'vga', mode: 'vga-13h' } },
  { name: 'grungy',   file: 'grungy.jpg',   opts: { chipset: 'cga', mode: 'cga-composite-hires' } },
  { name: 'fantasy1', file: 'fantasy1.jpg', opts: { chipset: 'cga', mode: 'cga-320' } },
  { name: 'fromthe',  file: 'fromthe.jpg',  opts: { chipset: 'ega', mode: 'ega-hi' } },
  { name: 'asterix',  file: 'asterix.jpg',  opts: { chipset: 'cga', mode: 'cga-text80x100', gamma: 1.2, brightness: -0.1, contrast: 1.6 } },
]

// c64 — own block-art sample pack (low-resolution sprite art / pixel
// game scenes represent VIC-II's natural output better than the Amiga
// photos). Per-image tuning mirrors png2c64's EXAMPLES list.
// ostromoukhov (used by png2c64's `alien`) was removed from png2amiga
// in v1.58, so we substitute floyd-steinberg.
const C64_EXAMPLES: Example[] = [
  { name: 'alien',    file: 'c64/alien.png',     opts: { chipset: 'c64', mode: 'c64-multicolor', gamma: 1, contrast: 1.5, dither: 'floyd-steinberg', ditherStrength: 1, matchRange: false } },
  { name: 'dog',      file: 'c64/dog.png',       opts: { chipset: 'c64', mode: 'c64-multicolor', gamma: 1.5, ditherStrength: 1 } },
  { name: 'dragon',   file: 'c64/dragon.png',    opts: { chipset: 'c64', mode: 'c64-multicolor', gamma: 1.5, ditherStrength: 1, matchRange: true } },
  { name: 'face',     file: 'c64/face.png',      opts: { chipset: 'c64', mode: 'c64-multicolor', gamma: 3, ditherStrength: 1, sharpen: -0.5, saturation: 0.5 } },
  { name: 'fantasy',  file: 'c64/fantasy.png',   opts: { chipset: 'c64', mode: 'c64-multicolor', gamma: 1, ditherStrength: 1, matchRange: true } },
  { name: 'game',     file: 'c64/game.png',      opts: { chipset: 'c64', mode: 'c64-multicolor', gamma: 2, ditherStrength: 1 } },
  { name: 'golden',   file: 'c64/golden3.jpeg',  opts: { chipset: 'c64', mode: 'c64-multicolor', gamma: 1.5, contrast: 1.5, ditherStrength: 1, matchRange: true } },
  { name: 'head',     file: 'c64/head.png',      opts: { chipset: 'c64', mode: 'c64-petscii',    gamma: 2, ditherStrength: 1, blackPoint: 0.09, whitePoint: 0.06, c64Metric: 'blur', c64PetsciiGraphicsOnly: true } },
  { name: 'monster',  file: 'c64/monster.jpeg',  opts: { chipset: 'c64', mode: 'c64-multicolor', gamma: 3, ditherStrength: 1 } },
  { name: 'ship',     file: 'c64/ship.jpeg',     opts: { chipset: 'c64', mode: 'c64-multicolor', gamma: 2, ditherStrength: 1, matchRange: true } },
]

// Map every chipset to its set. SNES + Genesis fall through to the
// Amiga set for now (no curated console images yet).
const EXAMPLES_BY_CHIPSET: Record<Chipset, Example[]> = {
  ocs:     AMIGA_EXAMPLES,
  aga:     AMIGA_EXAMPLES,
  stf:     ATARI_EXAMPLES,
  ste:     ATARI_EXAMPLES,
  vga:     IBM_EXAMPLES,
  ega:     IBM_EXAMPLES,
  cga:     IBM_EXAMPLES,
  c64:     C64_EXAMPLES,
  snes:    AMIGA_EXAMPLES,
  genesis: AMIGA_EXAMPLES,
}

export function examplesForChipset(chipset: Chipset): Example[] {
  return EXAMPLES_BY_CHIPSET[chipset]
}

// Legacy alias — the Amiga set is what main.ts / Converter.vue used to
// reach for via the unconditional `EXAMPLES[0]` initial loader; kept
// exported so `typeof EXAMPLES[number]` still types loadExample()'s
// parameter without churning that signature.
export const EXAMPLES: Example[] = AMIGA_EXAMPLES

// Hostname-driven chipset default. png2c64.app is an alias of
// png2amiga.app pointing at the same static bundle, so the UI inspects
// window.location.hostname at boot and lands on the c64 chipset there.
// Guarded for non-browser contexts (vitest jsdom defaults to localhost
// which falls through to 'ocs', matching the production amiga site).
export function detectDefaultChipset(): Chipset {
  // location may be missing under non-DOM test runners (vitest's default
  // node env). Cast through a permissive shape so the lookup is safe
  // and the production browser path still goes through unchanged.
  const g = globalThis as { location?: { hostname?: string } }
  const host = g.location?.hostname ?? ''
  if (host === 'png2c64.app' || host === 'www.png2c64.app') return 'c64'
  return 'ocs'
}

export function defaultOptions(): Options {
  const chipset = detectDefaultChipset()
  // c64 mode list doesn't include 'lores'; the chipset watcher would
  // otherwise rewrite mode on first paint. Seed the c64 default mode
  // directly so the initial encode runs against a valid pairing.
  const mode = chipset === 'c64' ? 'c64-multicolor' : 'lores'
  const opts: Options = {
    mode,
    chipset,
    depth: 5,
    interlace: false,
    copper: false,
    dither: 'floyd-steinberg',
    width: 0,
    height: 0,
    // sliced best-quality planner (multi-candidate slot search + joint
    // base-palette refinement). HAM6 + copper and HAM8 + copper only —
    // indexed copper modes ignore this flag (their planner is already
    // mature). +0.5..2 dB PSNR for ~4-5× the encode cost. Off by default.
    best: false,
    // Alpha
    alphaThreshold: 0,
    alphaDither: 'none',
    alphaDitherStrength: 1,
    // Crop
    cropX: 0,
    cropY: 0,
    cropW: 0,
    cropH: 0,
    cropAuto: false,
    // Copper override (0 = auto)
    copperChanges: 0,
    // Sliced 1-D Bayer alternation (spreads copper transitions across
    // rows; better visual smoothness on CRT, slightly worse S2/PSNR).
    slicedVerticalDither: false,
    // Symbol name for C header export (empty = auto from filename)
    symbolName: '',
    // Mask export
    maskInvert: false,
    // DOS modes: preserve source aspect in the fixed hardware buffer
    // (letterbox/pillarbox) instead of stretching to fill.
    nativePar: false,
    // Advanced
    lockColor0: true,
    reserves: [],
    // Dual playfield: encode image into PF2 (upper color regs 8-15 OCS /
    // 16-31 AGA), with PF1 (foreground) bitplanes zeroed. Forces depth=3
    // (OCS) or 4 (AGA). CAMG DBLPF flag set.
    dualPlayfield: false,
    // strips — Super Sliced palette: mid-line palette swaps
    // inside DPF's PF2. OCS lores only (Phase 1). Requires dpf + ocs.
    scap: false,
    ...CGA_TEXT_DEFAULTS,
    cgaCompositeNewCga: false,
    c64Palette: 'colodore',
    c64Metric:  'blur',
    c64PetsciiGraphicsOnly: false,
    tileBudget: 256,
    tileReserve: 0,
    matchRange: false,
    ...sliderDefaults(),
  }
  return opts
}

function sliderDefaults(): Record<SliderKey | DiffusionSliderKey, number> {
  const out = {} as Record<SliderKey | DiffusionSliderKey, number>
  for (const s of [...SLIDERS, ...DIFFUSION_SLIDERS]) out[s.key] = s.default
  return out
}

// --- Helper functions ---

// Extract base HAM type from compound mode (ham6, ham8, or null)
export function hamType(mode: string): HamType {
  if (mode.startsWith('ham8')) return 'ham8'
  if (mode.startsWith('ham6')) return 'ham6'
  return null
}

export function isHamMode(mode: string): boolean {
  return hamType(mode) !== null
}

export function isEhbMode(mode: string): boolean {
  return mode === 'ehb' || mode === 'ehb-lace'
}

export function isAtariMode(mode: string): boolean {
  return mode.startsWith('stf-') || mode.startsWith('ste-')
}

export function isVgaMode(mode: string): boolean { return mode.startsWith('vga-') }
export function isEgaMode(mode: string): boolean { return mode.startsWith('ega-') }
export function isCgaMode(mode: string): boolean { return mode.startsWith('cga-') }
export function isDosMode(mode: string): boolean {
  return isVgaMode(mode) || isEgaMode(mode) || isCgaMode(mode)
}
export function isSnesMode(mode: string): boolean {
  return mode.startsWith('snes-')
}
export function isGenesisMode(mode: string): boolean {
  return mode.startsWith('genesis-')
}
// ETC2 RGB8 — block-compressed texture format (4 bpp, 8-byte 4×4 blocks).
// Not an Amiga / retro mode. Output is .ktx2 with vkFormat
// VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK. None of the per-pixel Amiga knobs
// (palette quantization, dither at pixel level, chipset, depth, copper,
// sliced, strips, EHB, HAM, interlace) apply — gate them off in the UI.
// The --dither method IS honoured but only as the block-grid ED kernel
// (see src/etc2.cpp's encode_image + Options::block_ed.method).
export function isEtc2Mode(mode: string): boolean {
  return mode === 'etc2' || mode === 'etc2-rgb' || mode === 'etc2-rgb8'
}
export function isC64Mode(mode: string): boolean {
  return mode.startsWith('c64-')
}
// c64 charset modes accept arbitrary width/height (padded to cell size)
// and a configurable tile-budget. Distinguished from the bitmap c64
// modes (multicolor / hires / fli / afli / petscii) which stay locked
// to hardware screen dimensions.
export function isC64CharsetMode(mode: string): boolean {
  return mode === 'c64-charset-hires' ||
         mode === 'c64-charset-multicolor'
}

// Tile-based modes that accept arbitrary width/height padded to the
// True for any cga-text super-chunky mode (80×{200, 100, 50, 25}).
// All four use the same encoder + UI shape; only the cell height (and
// hence row count / output buffer size) differs.
export function isCgaText(mode: string): boolean {
  return mode === 'cga-text80x200' || mode === 'cga-text80x100'
      || mode === 'cga-text80x50'  || mode === 'cga-text80x25'
      || mode === 'cga-text40x200' || mode === 'cga-text40x100'
}
// per-platform tile size (8×8 for c64-charset / Genesis / SNES Mode 7).
// At freeform mode the Native PAR / fixed-buffer behavior is replaced
// by the user-typed dims; at default size they stay fixed-buffer.
export function isTileFreeformMode(mode: string): boolean {
  return isC64CharsetMode(mode) || isGenesisMode(mode) || isSnesMode(mode)
      || isCgaText(mode)
}
// SNES Mode 7 Direct quantises every pixel to the BBGGGRRR grid; the
// 2048-color gamut comes from per-tile palette-field bits. Yliluoma
// family (palette-aware ordered dithers) doesn't apply here — restrict
// the gallery + force a fallback when this mode is selected.
export function isSnesDirectMode(mode: string): boolean {
  return mode === 'snes-mode7-direct'
}
// Modes with non-square hardware pixels that benefit from --native-par
// (preserve source aspect ratio inside the fixed hardware buffer via
// letterbox / pillarbox). DOS + SNES both fit; auto-toggled on mode
// entry by the web UI.
export function isFixedBufferMode(mode: string): boolean {
  // Tile-freeform modes (c64-charset, Genesis, SNES Mode 7) drop out of
  // fixed-buffer when Resize is enabled; the UI uses isEffectiveFixedBuffer
  // (Vue side) to keep Native PAR available at default size.
  if (isTileFreeformMode(mode)) return false
  return isDosMode(mode) || isC64Mode(mode) || isAtariMode(mode)
}

// Hardware Pixel Aspect Ratio (display_pixel_width / display_pixel_height).
// Mirrors ModeParams::par in src/amiga.hpp. Used to CSS-stretch the preview
// canvas so it displays with the correct aspect (native-par on the web).
//   <1 tall pixels (e.g. EGA 640×200 = 0.417 — pixels are 2.4× taller than wide)
//    1 square
//   >1 wide pixels (e.g. CGA composite 160×200 = 1.667)
const MODE_PAR: Record<string, number> = {
  'vga-13h':    0.833,
  'vga-10h':    0.73,
  'vga-12h':    1,
  'ega-320':    0.833,
  'ega-640':    0.417,
  'ega-hi':     0.73,
  'cga-320':    0.833,
  'cga-640':    0.417,
  'cga-composite-hires': 0.417,
  'cga-text80x200': 0.417,
  'cga-text80x100': 0.417,
  'cga-text80x50':  0.417,
  'cga-text80x25':  0.417,
  'cga-text40x200': 0.833,
  'cga-text40x100': 0.833,
  // SNES Mode 7 — 256×224 → 4:3 ⇒ PAR ≈ 1.167 (slightly wide pixels).
  'snes-mode7-256':    1.167,
  'snes-mode7-direct': 1.167,
  // Sega Genesis: H32 256×224 → PAR 1.167 (matches SNES); H40 320×224
  // → PAR 0.933 (slightly tall pixels).
  'genesis-h32':    1.167,
  'genesis-h40':    0.933,
  'genesis-h32-sh': 1.167,
  'genesis-h40-sh': 0.933,
  // C64 hires / AFLI / PETSCII: encoder emits 320×200 native (1:1).
  // Display ratio = PAL VIC-II hardware pixel = 0.936:1.
  'c64-hires':           0.936,
  'c64-afli':            0.936,
  'c64-petscii':         0.936,
  'c64-charset-hires':   0.936,
  'c64-charset-multicolor': 1.872,
  // C64 multicolor / FLI: encoder emits 160×200 logical (each
  // logical pixel = 2 hardware pixels). Per-LOGICAL-pixel display
  // ratio = 2 × 0.936 = 1.872 (wide).
  'c64-multicolor':  1.872,
  'c64-fli':         1.872,
  // Atari ST/STE — color modes target a 4:3 CRT.
  //   low  320×200 → 4:3 ⇒ PAR ≈ 0.833 (slightly tall, like ega-320)
  //   med  640×200 → 4:3 ⇒ PAR ≈ 0.417 (2.4× tall, like ega-640)
  //   hi   640×400 monochrome monitor ⇒ PAR 1.0 (square)
  'stf-low':  0.833,  'ste-low':  0.833,
  'stf-med':  0.417,  'ste-med':  0.417,
  'stf-hi':   1,      'ste-hi':   1,
}

export function modePar(mode: string): number { return MODE_PAR[mode] ?? 1 }

const ERROR_DIFFUSION = new Set(['floyd-steinberg', 'sierra-lite', 'atkinson', 'jarvis', 'stucki', 'gilbert', 'riemersma', 'dbs'])

export function isErrorDiffusion(dither: string): boolean {
  return ERROR_DIFFUSION.has(dither)
}

export function isInterlaceMode(mode: string): boolean {
  return mode.endsWith('-lace')
}

export function isHiresMode(mode: string): boolean {
  return mode.includes('hires')
}

// DOS mode preview scaling overrides. The WASM encoder returns the raw
// hardware buffer (320×200, 640×200, 640×350, 640×480, 160×200); we
// nearest-neighbor scale on-canvas with whole factors. 640-wide buffers
// already fill most screen widths so don't double them; 320-wide ones do.
const DOS_PREVIEW_SCALE: Record<string, PreviewScale> = {
  'cga-320':         { sx: 2, sy: 2 },
  'ega-320':         { sx: 2, sy: 2 },
  'vga-13h':         { sx: 2, sy: 2 },
  'cga-composite-hires':     { sx: 1, sy: 2 },  // 640×200 → 640×400 → PAR ~640×480
  'vga-12h':         { sx: 1, sy: 1 },
  'vga-10h':         { sx: 1, sy: 1 },
  'ega-hi':          { sx: 1, sy: 1 },
  'ega-640':         { sx: 1, sy: 2 },
  'cga-640':         { sx: 1, sy: 2 },
  'cga-text80x200':  { sx: 1, sy: 1 },  // 1-scan cells: backing already 200 high
  'cga-text80x100':  { sx: 1, sy: 2 },
  'cga-text80x50':   { sx: 1, sy: 2 },
  'cga-text80x25':   { sx: 1, sy: 2 },
  'cga-text40x200':  { sx: 2, sy: 2 },  // 40-col: same 320×200 source as cga-320
  'cga-text40x100':  { sx: 2, sy: 2 },
  // C64 modes: backing-canvas scale before PAR-aware CSS stretch.
  //   hires / AFLI: encoder is 320×200 native; 2×2 → 640×400 backing.
  //   multicolor / FLI: encoder is 160×200 logical; 4×2 → 640×400
  //     backing (the 4× horizontal includes the 2:1 hardware-pixel
  //     doubling baked into multicolor display).
  // Both pairs land at the same physical canvas size.
  'c64-hires':           { sx: 2, sy: 2 },
  'c64-afli':            { sx: 2, sy: 2 },
  'c64-petscii':         { sx: 2, sy: 2 },
  'c64-charset-hires':       { sx: 2, sy: 2 },
  'c64-charset-multicolor':  { sx: 4, sy: 2 },
  'c64-multicolor':  { sx: 4, sy: 2 },
  'c64-fli':         { sx: 4, sy: 2 },
}

// Generic Amiga-mode preview scale by (hires?, interlace?). 1×1 for
// hires+lace (square 640×400), 1×2 for hires (tall fields), 2×1 for
// lace (extra rows already), 2×2 baseline.
const AMIGA_PREVIEW_SCALE: PreviewScale[] = [
  { sx: 2, sy: 2 },  // 0b00: progressive lores
  { sx: 1, sy: 2 },  // 0b01: hires, progressive
  { sx: 2, sy: 1 },  // 0b10: lores, interlace
  { sx: 1, sy: 1 },  // 0b11: hires, interlace
]

// Pixel display scale for preview (minimum pixel size on screen)
export function previewScale(mode: string): PreviewScale {
  const dos = DOS_PREVIEW_SCALE[mode]
  if (dos) return dos
  const hi = isHiresMode(mode) || mode.endsWith('-med') || mode.endsWith('-hi')
  const lace = isInterlaceMode(mode) || mode.endsWith('-hi')  // -hi is 640×400 square pixels
  return AMIGA_PREVIEW_SCALE[(lace ? 2 : 0) | (hi ? 1 : 0)] ?? { sx: 2, sy: 2 }
}

// Decompose a UI mode into C++ mode string + optional width override.
// e.g. 'ham6-hires-lace' -> { mode: 'ham6', width: 640, interlace: true }
export function decomposeMode(uiMode: string): DecomposedMode {
  const ham = hamType(uiMode)
  if (ham) {
    const hi = uiMode.includes('hires')
    const lace = uiMode.endsWith('-lace')
    return {
      mode: ham,
      width: hi ? 640 : 0,
      interlace: lace,
    }
  }
  return { mode: uiMode, width: 0, interlace: false }
}

// Maximum bitplane depth for a given mode/chipset.
// Modes whose depth is fixed by the target hardware (HAM data bits,
// EHB always-6, Atari mode-defined, DOS hardware-defined, C64 cell
// constraints) report 0 to mean "no user-adjustable depth slider".
const FIXED_DEPTH_PREDICATES = [
  isHamMode, isEhbMode, isAtariMode, isDosMode, isC64Mode,
] as const
export function maxDepth(mode: string, chipset: Chipset): number {
  for (const p of FIXED_DEPTH_PREDICATES) if (p(mode)) return 0
  if (isHiresMode(mode)) return chipset === 'aga' ? 8 : 4
  return chipset === 'aga' ? 8 : 5
}

// Modes whose default bitplane depth is fixed by hardware. Looked up
// before the family-based fallbacks below so e.g. cga-640 picks 1 not 4.
const FIXED_DEFAULT_DEPTH: Record<string, number> = {
  'stf-low': 4, 'ste-low': 4,
  'stf-med': 2, 'ste-med': 2,
  'stf-hi':  1, 'ste-hi':  1,
  'cga-640': 1,
  'cga-320': 2, 'cga-composite-hires': 1,
  'vga-13h': 8,
}

// Family-level fallbacks for defaultDepth. Tried in order after the
// per-mode FIXED_DEFAULT_DEPTH lookup misses. First matching predicate
// wins; the final entry is the lores baseline (always matches).
const FAMILY_DEFAULT_DEPTH: { match: (m: string) => boolean; depth: number }[] = [
  { match: m => m.startsWith('cga-text') || m.startsWith('ega-text'), depth: 4 },
  { match: m => isEgaMode(m) || isVgaMode(m), depth: 4 },
  { match: m => isHiresMode(m), depth: 4 },
  { match: () => true, depth: 5 },
]

// Default bitplane depth for a given mode
export function defaultDepth(mode: string): number {
  if (isEhbMode(mode)) return 6
  const ham = hamType(mode)
  if (ham === 'ham6') return 6
  if (ham === 'ham8') return 8
  const fixed = FIXED_DEFAULT_DEPTH[mode]
  if (fixed !== undefined) return fixed
  return FAMILY_DEFAULT_DEPTH.find(f => f.match(mode))?.depth ?? 5
}

// Number of displayable colors for a given mode/depth, formatted as a label.
// HAM modes return "N base + modify" since modify-bits don't count as a flat
// palette size; standard / EHB return the integer count as a string.
export function numColors(mode: string, depth: number): string {
  if (isEhbMode(mode)) return '64'
  if (isHamMode(mode)) {
    const dataBits = defaultDepth(mode) - 2
    return `${1 << dataBits} base + modify`
  }
  return String(1 << depth)
}

// Bitplane count for display
export function numBitplanes(mode: string, depth: number): number {
  if (isEhbMode(mode)) return 6
  if (isHamMode(mode)) return defaultDepth(mode)
  return depth
}

// Resolve effective chipset from mode and user selection
export function effectiveChipset(mode: string, chipset: Chipset): Chipset {
  if (hamType(mode) === 'ham8') return 'aga'
  return chipset
}
