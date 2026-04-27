// Type-side declarations.

export type Chipset = 'ocs' | 'aga' | 'stf' | 'ste' | 'vga' | 'ega' | 'cga' | 'snes' | 'genesis'

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
  | 'gamma' | 'ditherStrength' | 'brightness' | 'contrast' | 'saturation'
  | 'hueShift' | 'sharpen' | 'blackPoint' | 'whitePoint'
export type DiffusionSliderKey = 'errorClamp'

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
  hamBeam: number
  capBest: boolean
  alphaThreshold: number
  alphaDither: string
  alphaDitherStrength: number
  cropX: number
  cropY: number
  cropW: number
  cropH: number
  cropAuto: boolean
  copperChanges: number
  symbolName: string
  maskInvert: boolean
  nativePar: boolean
  reserveColor0: boolean
  paletteDiversity: number
  dualPlayfield: boolean
  scap: boolean
  cgaTextMetric: string
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
  { value: 'cga-320',          label: 'CGA 320x200 (4 colors)',        chipset: 'cga' },
  { value: 'cga-640',          label: 'CGA 640x200 (2 colors, mono)',  chipset: 'cga' },
  { value: 'cga-composite',    label: 'CGA Composite (160x200)',       chipset: 'cga' },
  { value: 'cga-text80x100',   label: 'CGA Text 80x100 (8x8 font)',    chipset: 'cga' },
  // SNES Mode 7.
  { value: 'snes-mode7-256',    label: 'Mode 7 (256-colour BGR555)',   chipset: 'snes' },
  { value: 'snes-mode7-direct', label: 'Mode 7 Direct (RGB443)',        chipset: 'snes' },
  // Sega Genesis / Mega Drive — 8x8 4bpp tiles + 4 palettes × 16 BGR333.
  { value: 'genesis-h32', label: 'H32 (256x224)', chipset: 'genesis' },
  { value: 'genesis-h40', label: 'H40 (320x224)', chipset: 'genesis' },
]

// Filter modes available for a given chipset
export function modesForChipset(chipset: Chipset): ModeOption[] {
  if (chipset === 'stf') return ALL_MODES.filter(m => m.chipset === 'stf')
  if (chipset === 'ste') return ALL_MODES.filter(m => m.chipset === 'ste')
  if (chipset === 'vga') return ALL_MODES.filter(m => m.chipset === 'vga')
  if (chipset === 'ega') return ALL_MODES.filter(m => m.chipset === 'ega')
  if (chipset === 'cga') return ALL_MODES.filter(m => m.chipset === 'cga')
  if (chipset === 'snes') return ALL_MODES.filter(m => m.chipset === 'snes')
  if (chipset === 'genesis') return ALL_MODES.filter(m => m.chipset === 'genesis')
  return ALL_MODES.filter(m =>
    (chipset === 'aga' || m.chipset === 'ocs') &&
    !['stf', 'ste', 'vga', 'ega', 'cga', 'snes', 'genesis'].includes(m.chipset))
}

export const MODES: ModeOption[] = ALL_MODES

export const CHIPSETS: ChipsetOption[] = [
  { value: 'ocs',  label: 'Amiga OCS (12-bit)' },
  { value: 'aga',  label: 'Amiga AGA (24-bit)' },
  { value: 'stf',  label: 'Atari STF (9-bit)' },
  { value: 'ste',  label: 'Atari STE (12-bit)' },
  { value: 'vga',  label: 'IBM PC VGA (18-bit DAC)' },
  { value: 'ega',  label: 'IBM PC EGA (6-bit IrgbIRGB)' },
  { value: 'cga',  label: 'IBM PC CGA (fixed palette)' },
  { value: 'snes', label: 'SNES Mode 7' },
  { value: 'genesis', label: 'Sega Genesis / Mega Drive' },
]

export const DITHER_METHODS: DitherGroup[] = [
  { group: 'None', items: [
    { value: 'none', label: 'None' },
  ]},
  // Order within "Error Diffusion" reflects the mean PSNR ranking from
  // a 10-image × 6-mode sweep (lores/ham6/ham8/ehb plus copper variants):
  // ostromoukhov 37.116 dB > sierra-lite 37.100 > atkinson 37.089 >
  // jarvis 37.062 > floyd-steinberg 37.039 > stucki 36.767 > gilbert
  // 35.767 > riemersma 35.081. Top-5 sit within 0.08 dB so the order is
  // a guideline, not a verdict; ostromoukhov is the global default.
  // Structure-aware variants sit at the bottom because they intentionally
  // sacrifice PSNR for perceptual quality. Palette-aware methods
  // (yliluoma family / knoll / opt-* / tri-tone) follow.
  { group: 'Error Diffusion', items: [
    { value: 'ostromoukhov',    label: 'Ostromoukhov' },
    { value: 'sierra-lite',     label: 'Sierra Lite' },
    { value: 'atkinson',        label: 'Atkinson' },
    { value: 'jarvis',          label: 'Jarvis' },
    { value: 'floyd-steinberg', label: 'Floyd–\nSteinberg' },
    { value: 'stucki',          label: 'Stucki' },
    { value: 'gilbert',         label: 'Gilbert' },
    { value: 'riemersma',       label: 'Riemersma' },
    { value: 'opt-checker',     label: 'Optimal\nChecker' },
    { value: 'opt-line',        label: 'Optimal\nLine' },
    { value: 'opt-line-checker',label: 'Optimal\nLine-Chk' },
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
  { key: 'gamma',          label: 'Gamma',       min: 0.1, max: 3, step: 0.05, default: 1,
    tip: 'Power curve applied before color matching. >1 darkens midtones, <1 brightens them.' },
  { key: 'ditherStrength', label: 'Strength',    min: 0,   max: 3, step: 0.05, default: 0.8,
    tip: 'Dithering intensity. 0 = no dithering effect, 1 = standard, >1 = exaggerated.' },
  { key: 'brightness',     label: 'Brightness',  min: -1,  max: 1, step: 0.05, default: 0,
    tip: 'Additive lightness shift in perceptual OKLab space.' },
  { key: 'contrast',       label: 'Contrast',    min: 0,   max: 3, step: 0.05, default: 1,
    tip: 'Scale around perceptual mid-grey. 1.0 = no change, >1 increases contrast.' },
  { key: 'saturation',     label: 'Saturation',  min: 0,   max: 3, step: 0.05, default: 1,
    tip: 'Chroma scaling in OKLab space. 0 = greyscale, 1 = original, >1 = boosted color.' },
  { key: 'hueShift',       label: 'Hue',         min: -180, max: 180, step: 1,  default: 0,
    tip: 'Rotate all colors in OKLab. Shifts hues to better match the Amiga palette.' },
  { key: 'sharpen',        label: 'Sharpen',     min: -1,  max: 2, step: 0.05, default: 0,
    tip: 'Negative = blur (reduces noise), positive = sharpen (enhances edges).' },
  { key: 'blackPoint',     label: 'Black Pt',    min: 0,   max: 0.5, step: 0.01, default: 0,
    tip: 'Clip the darkest fraction of the image. Deepens blacks.' },
  { key: 'whitePoint',     label: 'White Pt',    min: 0,   max: 0.5, step: 0.01, default: 0,
    tip: 'Clip the brightest fraction of the image. Cleans up highlights.' },
]

export const DIFFUSION_SLIDERS: Slider[] = [
  { key: 'errorClamp',    label: 'Error Clamp', min: 0,   max: 1, step: 0.025, default: 0.35,
    tip: 'Max error accumulation per channel (squared internally). Lower = cleaner, higher = more dithering noise.' },
]

// CGA-text-mode-only options. Shown only when mode === 'cga-text80x100'.
export const CGA_TEXT_METRICS: NamedItem[] = [
  { value: 'blur', label: 'Pappas-Neuhoff' },
  { value: 'mse',  label: 'Per-pixel MSE' },
]
export const CGA_TEXT_DEFAULTS = {
  cgaTextMetric: 'blur',
}

export const EXAMPLES: Example[] = [
  { name: 'electrichues', file: 'electrichues02.jpg', opts: { mode: 'lores', depth: 5, dither: 'ostromoukhov', ditherStrength: 0.5 } },
  { name: 'fantasy',      file: 'fantasy.png',        opts: { mode: 'ham6', dither: 'ostromoukhov', copper: true } },
  { name: 'lovers',       file: 'lovers.jpg',         opts: { mode: 'lores', depth: 5, dither: 'ostromoukhov', ditherStrength: 0.5 } },
  { name: 'logo',         file: 'logo.png',           opts: { mode: 'lores', depth: 5, dither: 'ostromoukhov', alphaThreshold: 0 } },
  { name: 'space',        file: 'space3.png',          opts: { mode: 'lores', depth: 5, dither: 'ostromoukhov', ditherStrength: 0.5 } },
  { name: 'photo',        file: 'photo.jpg',           opts: { mode: 'ham6', dither: 'ostromoukhov', copper: true } },
  { name: 'grungy',       file: 'grungy.png',          opts: { mode: 'lores', depth: 3, dither: 'sierra-lite', ditherStrength: 0.9, brightness: -0.05, contrast: 0.9, gamma: 1, copper: true } },
  { name: 'fantasy1',     file: 'fantasy1.png',        opts: { mode: 'lores', depth: 3, dither: 'ostromoukhov', copper: true } },
  { name: 'fromthe',      file: 'fromthe.png',         opts: { mode: 'lores', depth: 3, dither: 'checker', dualPlayfield: true, scap: true } },
  { name: 'asterix',      file: 'asterix.png',         opts: { mode: 'cga-text80x100', chipset: 'cga', gamma: 1.2, brightness: -0.1, contrast: 1.6 } },
]

export function defaultOptions(): Options {
  const opts: Options = {
    mode: 'lores',
    chipset: 'ocs',
    depth: 5,
    interlace: false,
    copper: false,
    dither: 'ostromoukhov',
    width: 0,
    height: 0,
    // HAM
    hamBeam: 16,
    // CAP best-quality planner (multi-candidate slot search + joint
    // base-palette refinement). HAM6 + copper and HAM8 + copper only —
    // indexed copper modes ignore this flag (their planner is already
    // mature). +0.5..2 dB PSNR for ~4-5× the encode cost. Off by default.
    capBest: false,
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
    // Symbol name for C header export (empty = auto from filename)
    symbolName: '',
    // Mask export
    maskInvert: false,
    // DOS modes: preserve source aspect in the fixed hardware buffer
    // (letterbox/pillarbox) instead of stretching to fill.
    nativePar: false,
    // Advanced
    reserveColor0: true,
    paletteDiversity: 0,
    // Dual playfield: encode image into PF2 (upper color regs 8-15 OCS /
    // 16-31 AGA), with PF1 (foreground) bitplanes zeroed. Forces depth=3
    // (OCS) or 4 (AGA). CAMG DBLPF flag set.
    dualPlayfield: false,
    // SCAP — Super Copper-Augmented Palette: mid-line palette swaps
    // inside DPF's PF2. OCS lores only (Phase 1). Requires dpf + ocs.
    scap: false,
    ...CGA_TEXT_DEFAULTS,
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
// SNES Mode 7 Direct quantises every pixel directly to the RGB443 grid
// (no palette table). Only the hard-coded FS-style serpentine error
// diffusion in the encoder is meaningful — every other dither method
// silently collapses to it. Restrict the gallery + force a fallback
// when this mode is selected.
export function isSnesDirectMode(mode: string): boolean {
  return mode === 'snes-mode7-direct'
}
// Modes with non-square hardware pixels that benefit from --native-par
// (preserve source aspect ratio inside the fixed hardware buffer via
// letterbox / pillarbox). DOS + SNES both fit; auto-toggled on mode
// entry by the web UI.
export function isFixedBufferMode(mode: string): boolean {
  return isDosMode(mode) || isSnesMode(mode) || isGenesisMode(mode)
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
  'cga-composite': 1.667,
  'cga-text80x100': 0.417,
  // SNES Mode 7 — 256×224 → 4:3 ⇒ PAR ≈ 1.167 (slightly wide pixels).
  'snes-mode7-256':    1.167,
  'snes-mode7-direct': 1.167,
  // Sega Genesis: H32 256×224 → PAR 1.167 (matches SNES); H40 320×224
  // → PAR 0.933 (slightly tall pixels).
  'genesis-h32': 1.167,
  'genesis-h40': 0.933,
}

export function modePar(mode: string): number { return MODE_PAR[mode] ?? 1 }

const ERROR_DIFFUSION = new Set(['ostromoukhov', 'sierra-lite', 'atkinson', 'jarvis', 'floyd-steinberg', 'stucki', 'gilbert', 'riemersma'])

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
  'cga-composite':   { sx: 4, sy: 2 },  // 160×200 → stretch wide
  'vga-12h':         { sx: 1, sy: 1 },
  'vga-10h':         { sx: 1, sy: 1 },
  'ega-hi':          { sx: 1, sy: 1 },
  'ega-640':         { sx: 1, sy: 2 },
  'cga-640':         { sx: 1, sy: 2 },
  'cga-text80x100':  { sx: 1, sy: 2 },
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

// Maximum bitplane depth for a given mode/chipset
export function maxDepth(mode: string, chipset: Chipset): number {
  if (isHamMode(mode) || isEhbMode(mode) || isAtariMode(mode)) return 0
  if (isDosMode(mode)) return 0  // DOS modes have fixed depth per hardware
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
  'cga-320': 2, 'cga-composite': 2,
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
