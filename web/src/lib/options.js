// All modes with chipset availability (ocs = available on both, aga = AGA only)
const ALL_MODES = [
  { value: 'lores',            label: 'Lores (320px)',                 chipset: 'ocs' },
  { value: 'lores-lace',       label: 'Lores Interlace (320px)',       chipset: 'ocs' },
  { value: 'hires',            label: 'Hires (640px)',                 chipset: 'ocs' },
  { value: 'hires-lace',       label: 'Hires Interlace (640px)',       chipset: 'ocs' },
  { value: 'ham6',             label: 'HAM6 Lores',                   chipset: 'ocs' },
  { value: 'ham6-lace',        label: 'HAM6 Lores Interlace',         chipset: 'ocs' },
  { value: 'ham6-hires',       label: 'HAM6 Hires',                   chipset: 'aga' },
  { value: 'ham6-hires-lace',  label: 'HAM6 Hires Interlace',         chipset: 'aga' },
  { value: 'ham8',             label: 'HAM8 Lores',                   chipset: 'aga' },
  { value: 'ham8-lace',        label: 'HAM8 Lores Interlace',         chipset: 'aga' },
  { value: 'ham8-hires',       label: 'HAM8 Hires',                   chipset: 'aga' },
  { value: 'ham8-hires-lace',  label: 'HAM8 Hires Interlace',         chipset: 'aga' },
  { value: 'ehb',              label: 'EHB (Extra Half-Brite)',        chipset: 'ocs' },
]

// Filter modes available for a given chipset
export function modesForChipset(chipset) {
  return ALL_MODES.filter(m => chipset === 'aga' || m.chipset === 'ocs')
}

export const MODES = ALL_MODES

export const CHIPSETS = [
  { value: 'ocs',  label: 'OCS (12-bit)' },
  { value: 'aga',  label: 'AGA (24-bit)' },
]

export const DITHER_METHODS = [
  { group: 'None', items: [
    { value: 'none', label: 'None' },
  ]},
  { group: 'Ordered (square)', items: [
    { value: 'bayer2x2', label: 'Bayer 2x2' },
    { value: 'bayer4x4', label: 'Bayer 4x4' },
    { value: 'bayer8x8', label: 'Bayer 8x8' },
  ]},
  { group: 'Ordered (pattern)', items: [
    { value: 'checker',       label: 'Checker' },
    { value: 'h2x4',          label: 'H 2x4' },
    { value: 'clustered-dot', label: 'Clustered Dot' },
  ]},
  { group: 'Lines', items: [
    { value: 'line2',         label: 'Line 2' },
    { value: 'line-checker',  label: 'Line Checker' },
    { value: 'line4',         label: 'Line 4' },
    { value: 'line8',         label: 'Line 8' },
  ]},
  { group: 'Error Diffusion', items: [
    { value: 'floyd-steinberg', label: 'Floyd-Steinberg' },
    { value: 'atkinson',        label: 'Atkinson' },
    { value: 'sierra-lite',     label: 'Sierra Lite' },
    { value: 'stucki',          label: 'Stucki' },
    { value: 'jarvis',          label: 'Jarvis' },
  ]},
]

export const ALPHA_DITHER_METHODS = [
  { value: 'none',         label: 'Hard Threshold' },
  { value: 'bayer4x4',    label: 'Bayer 4x4' },
  { value: 'bayer8x8',    label: 'Bayer 8x8' },
  { value: 'checker',     label: 'Checker' },
]

export const HAM_QUALITY = [
  { value: 'fast',    label: 'Fast (greedy)' },
  { value: 'optimal', label: 'Optimal (beam search)' },
]

export const SLIDERS = [
  { key: 'gamma',          label: 'Gamma',       min: 0.1, max: 3.0, step: 0.05, default: 1.0,
    tip: 'Power curve applied before color matching. >1 darkens midtones, <1 brightens them.' },
  { key: 'ditherStrength', label: 'Strength',    min: 0,   max: 3.0, step: 0.05, default: 0.8,
    tip: 'Dithering intensity. 0 = no dithering effect, 1 = standard, >1 = exaggerated.' },
  { key: 'brightness',     label: 'Brightness',  min: -1,  max: 1.0, step: 0.05, default: 0.0,
    tip: 'Additive lightness shift in perceptual OKLab space.' },
  { key: 'contrast',       label: 'Contrast',    min: 0,   max: 3.0, step: 0.05, default: 1.0,
    tip: 'Scale around perceptual mid-grey. 1.0 = no change, >1 increases contrast.' },
  { key: 'saturation',     label: 'Saturation',  min: 0,   max: 3.0, step: 0.05, default: 1.0,
    tip: 'Chroma scaling in OKLab space. 0 = greyscale, 1 = original, >1 = boosted color.' },
  { key: 'hueShift',       label: 'Hue',         min: -180, max: 180, step: 1.0,  default: 0.0,
    tip: 'Rotate all colors in OKLab. Shifts hues to better match the Amiga palette.' },
  { key: 'sharpen',        label: 'Sharpen',     min: -1,  max: 2.0, step: 0.05, default: 0.0,
    tip: 'Negative = blur (reduces noise), positive = sharpen (enhances edges).' },
  { key: 'blackPoint',     label: 'Black Pt',    min: 0,   max: 0.5, step: 0.01, default: 0.0,
    tip: 'Clip the darkest fraction of the image. Deepens blacks.' },
  { key: 'whitePoint',     label: 'White Pt',    min: 0,   max: 0.5, step: 0.01, default: 0.0,
    tip: 'Clip the brightest fraction of the image. Cleans up highlights.' },
]

export const DIFFUSION_SLIDERS = [
  { key: 'errorClamp',    label: 'Error Clamp', min: 0,   max: 2.0, step: 0.05, default: 0.12,
    tip: 'Max error accumulation per channel. Lower = fewer stray pixels, higher = more detail.' },
]

export const EXAMPLES = [
  { name: 'electrichues', file: 'electrichues02.jpg', opts: { mode: 'lores', depth: 5, dither: 'checker', ditherStrength: 0.5 } },
  { name: 'fantasy',      file: 'fantasy.png',        opts: { mode: 'ham6', dither: 'none' } },
  { name: 'lovers',       file: 'lovers.jpg',         opts: { mode: 'lores', depth: 5, dither: 'checker', ditherStrength: 0.5 } },
  { name: 'logo',         file: 'logo.png',           opts: { mode: 'lores', depth: 5, dither: 'checker', alphaThreshold: 0 } },
  { name: 'space',        file: 'space3.png',          opts: { mode: 'lores', depth: 5, dither: 'checker', ditherStrength: 0.5 } },
]

export function defaultOptions() {
  const opts = {
    mode: 'lores',
    chipset: 'ocs',
    depth: 5,
    interlace: false,
    copper: false,
    dither: 'checker',
    width: 0,
    height: 0,
    // HAM
    hamQuality: 'optimal',
    hamBeam: 16,
    // Alpha
    alphaThreshold: 0,
    alphaDither: 'none',
    alphaDitherStrength: 1.0,
    // Crop
    cropX: 0,
    cropY: 0,
    cropW: 0,
    cropH: 0,
    cropAuto: false,
    // Symbol name for C header export (empty = auto from filename)
    symbolName: '',
  }
  for (const s of [...SLIDERS, ...DIFFUSION_SLIDERS]) opts[s.key] = s.default
  return opts
}

// --- Helper functions ---

// Extract base HAM type from compound mode (ham6, ham8, or null)
export function hamType(mode) {
  if (mode.startsWith('ham8')) return 'ham8'
  if (mode.startsWith('ham6')) return 'ham6'
  return null
}

export function isHamMode(mode) {
  return hamType(mode) !== null
}

export function isEhbMode(mode) {
  return mode === 'ehb'
}

const ERROR_DIFFUSION = new Set(['floyd-steinberg', 'atkinson', 'sierra-lite', 'stucki', 'jarvis'])

export function isErrorDiffusion(dither) {
  return ERROR_DIFFUSION.has(dither)
}

export function isInterlaceMode(mode) {
  return mode.endsWith('-lace')
}

export function isHiresMode(mode) {
  return mode.includes('hires')
}

// Pixel display scale for preview (minimum pixel size on screen)
export function previewScale(mode) {
  const hi = isHiresMode(mode)
  const lace = isInterlaceMode(mode)
  if (hi && lace) return { sx: 1, sy: 1 }
  if (hi)         return { sx: 1, sy: 2 }
  if (lace)       return { sx: 2, sy: 1 }
  return { sx: 2, sy: 2 }
}

// Decompose a UI mode into C++ mode string + optional width override.
// e.g. 'ham6-hires-lace' -> { mode: 'ham6', width: 640, interlace: true }
export function decomposeMode(uiMode) {
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
export function maxDepth(mode, chipset) {
  if (isHamMode(mode) || isEhbMode(mode)) return 0 // depth is fixed for these modes
  if (isHiresMode(mode)) return chipset === 'aga' ? 8 : 4
  return chipset === 'aga' ? 8 : 5
}

// Default bitplane depth for a given mode
export function defaultDepth(mode) {
  if (isEhbMode(mode)) return 6
  const ham = hamType(mode)
  if (ham === 'ham6') return 6
  if (ham === 'ham8') return 8
  if (isHiresMode(mode)) return 4
  return 5
}

// Number of displayable colors for a given mode/depth
export function numColors(mode, depth) {
  if (isEhbMode(mode)) return 64
  if (isHamMode(mode)) {
    const dataBits = defaultDepth(mode) - 2
    return `${1 << dataBits} base + modify`
  }
  return 1 << depth
}

// Bitplane count for display
export function numBitplanes(mode, depth) {
  if (isEhbMode(mode)) return 6
  if (isHamMode(mode)) return defaultDepth(mode)
  return depth
}

// Resolve effective chipset from mode and user selection
export function effectiveChipset(mode, chipset) {
  if (hamType(mode) === 'ham8') return 'aga'
  return chipset
}
