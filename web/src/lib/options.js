export const MODES = [
  { value: 'lores', label: 'Lores (320px)' },
  { value: 'hires', label: 'Hires (640px)' },
  { value: 'ham4', label: 'HAM4 (4 planes)' },
  { value: 'ham5', label: 'HAM5 (5 planes)' },
  { value: 'ham6', label: 'HAM6 (6 planes, OCS)' },
  { value: 'ham7', label: 'HAM7 (7 planes)' },
  { value: 'ham8', label: 'HAM8 (8 planes, AGA)' },
  { value: 'ehb', label: 'EHB (Extra Half-Brite)' },
]

export const CHIPSETS = [
  { value: 'ocs', label: 'OCS (12-bit)' },
  { value: 'aga', label: 'AGA (24-bit)' },
]

export const DITHER_METHODS = [
  { group: 'None', items: [{ value: 'none', label: 'None' }] },
  { group: 'Ordered (square)', items: [
    { value: 'bayer2x2', label: 'Bayer 2x2' },
    { value: 'bayer4x4', label: 'Bayer 4x4' },
    { value: 'bayer8x8', label: 'Bayer 8x8' },
  ]},
  { group: 'Ordered (pattern)', items: [
    { value: 'checker', label: 'Checker' },
    { value: 'h2x4', label: 'H 2x4' },
    { value: 'clustered-dot', label: 'Clustered Dot' },
  ]},
  { group: 'Lines', items: [
    { value: 'line2', label: 'Line 2' },
    { value: 'line-checker', label: 'Line Checker' },
    { value: 'line4', label: 'Line 4' },
    { value: 'line8', label: 'Line 8' },
  ]},
  { group: 'Error Diffusion', items: [
    { value: 'floyd-steinberg', label: 'Floyd-Steinberg' },
    { value: 'atkinson', label: 'Atkinson' },
    { value: 'sierra-lite', label: 'Sierra Lite' },
    { value: 'stucki', label: 'Stucki' },
    { value: 'jarvis', label: 'Jarvis' },
  ]},
]

export const ALPHA_DITHER_METHODS = [
  { value: '', label: 'Hard Threshold' },
  { value: 'bayer4x4', label: 'Bayer 4x4' },
  { value: 'bayer8x8', label: 'Bayer 8x8' },
  { value: 'checker', label: 'Checker' },
  { value: 'floyd-steinberg', label: 'Floyd-Steinberg' },
]

export const SLIDERS = [
  { key: 'gamma',          label: 'Gamma',       min: 0.1, max: 5.0, step: 0.05, default: 1.0,
    tip: 'Power curve applied before color matching. >1 darkens midtones, <1 brightens them.' },
  { key: 'ditherStrength', label: 'Strength',    min: 0,   max: 3.0, step: 0.05, default: 1.0,
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
  { key: 'blackPoint',     label: 'Black Pt',    min: 0,   max: 0.4, step: 0.01, default: 0.0,
    tip: 'Clip the darkest fraction of the image. Deepens blacks.' },
  { key: 'whitePoint',     label: 'White Pt',    min: 0,   max: 0.4, step: 0.01, default: 0.0,
    tip: 'Clip the brightest fraction of the image. Cleans up highlights.' },
]

export const DIFFUSION_SLIDERS = [
  { key: 'errorClamp',     label: 'Error Clamp', min: 0,   max: 2.0, step: 0.05, default: 0.12,
    tip: 'Max error accumulation per channel. Lower = fewer stray pixels, higher = more detail.' },
]

export const EXAMPLES = [
  { name: 'test', file: 'test.png' },
]

export function defaultOptions() {
  const opts = {
    mode: 'lores',
    chipset: 'ocs',
    depth: 5,
    interlace: false,
    dither: 'floyd-steinberg',
    matchRange: false,
    width: 0,
    height: 0,
    // HAM
    hamQuality: 'optimal',
    hamBeam: 48,
    // Flags
    copper: false,
    // Alpha
    alphaThreshold: 0.5,
    alphaDither: '',
    alphaDitherStrength: 1.0,
    // Crop
    cropX: 0,
    cropY: 0,
    cropW: 0,
    cropH: 0,
    cropAuto: false,
  }
  for (const s of [...SLIDERS, ...DIFFUSION_SLIDERS]) opts[s.key] = s.default
  return opts
}

export function isHamMode(mode) {
  return mode === 'ham4' || mode === 'ham5' || mode === 'ham6' || mode === 'ham7' || mode === 'ham8'
}

export function isEhbMode(mode) {
  return mode === 'ehb'
}

const ERROR_DIFFUSION = new Set(['floyd-steinberg', 'atkinson', 'sierra-lite', 'stucki', 'jarvis'])

export function isErrorDiffusion(dither) {
  return ERROR_DIFFUSION.has(dither)
}

// Maximum depth for a given mode/chipset
export function maxDepth(mode, chipset) {
  if (isHamMode(mode) || isEhbMode(mode)) return 0 // depth is fixed
  if (mode === 'hires') return chipset === 'aga' ? 8 : 4
  return chipset === 'aga' ? 8 : 6
}

// Default depth for mode
export function defaultDepth(mode) {
  switch (mode) {
    case 'lores': return 5
    case 'hires': return 4
    case 'ham4': return 4
    case 'ham5': return 5
    case 'ham6': return 6
    case 'ham7': return 7
    case 'ham8': return 8
    case 'ehb': return 6
    default: return 5
  }
}

// Number of colors for display
export function numColors(mode, depth) {
  if (isEhbMode(mode)) return 64
  if (isHamMode(mode)) {
    const dataBits = depth - 2
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
