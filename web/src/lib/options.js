// All modes with chipset availability (ocs = available on both, aga = AGA only)
const ALL_MODES = [
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
]

// Filter modes available for a given chipset
export function modesForChipset(chipset) {
  if (chipset === 'stf') return ALL_MODES.filter(m => m.chipset === 'stf')
  if (chipset === 'ste') return ALL_MODES.filter(m => m.chipset === 'ste')
  if (chipset === 'vga') return ALL_MODES.filter(m => m.chipset === 'vga')
  if (chipset === 'ega') return ALL_MODES.filter(m => m.chipset === 'ega')
  if (chipset === 'cga') return ALL_MODES.filter(m => m.chipset === 'cga')
  return ALL_MODES.filter(m =>
    (chipset === 'aga' || m.chipset === 'ocs') &&
    !['stf', 'ste', 'vga', 'ega', 'cga'].includes(m.chipset))
}

export const MODES = ALL_MODES

export const CHIPSETS = [
  { value: 'ocs',  label: 'Amiga OCS (12-bit)' },
  { value: 'aga',  label: 'Amiga AGA (24-bit)' },
  { value: 'stf',  label: 'Atari STF (9-bit)' },
  { value: 'ste',  label: 'Atari STE (12-bit)' },
  { value: 'vga',  label: 'IBM PC VGA (18-bit DAC)' },
  { value: 'ega',  label: 'IBM PC EGA (6-bit IrgbIRGB)' },
  { value: 'cga',  label: 'IBM PC CGA (fixed palette)' },
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
    { value: 'clustered-dot', label: 'Clustered Dot' },
  ]},
  { group: 'Non-square', items: [
    { value: 'h2x4',          label: 'H 2x4 (wide pixels)' },
    { value: 'v4x2',          label: 'V 4x2 (tall pixels)' },
    { value: 'bayer4x2',      label: 'Bayer 4x2 (hires)' },
    { value: 'bayer2x4',      label: 'Bayer 2x4 (interlace)' },
  ]},
  { group: 'Halftone', items: [
    { value: 'halftone8x8',   label: 'Halftone 8x8 (45°)' },
    { value: 'diagonal8x8',   label: 'Diagonal 8x8 (newspaper)' },
    { value: 'spiral5x5',     label: 'Spiral 5x5' },
  ]},
  { group: 'Non-rectangular', items: [
    { value: 'hex8x8',        label: 'Hexagonal 8x8' },
    { value: 'hex5x5',        label: 'Hexagonal 5x5' },
    { value: 'blue-noise',    label: 'Blue Noise' },
    { value: 'ign',           label: 'IGN (Gradient Noise)' },
    { value: 'white-noise',  label: 'White Noise' },
    { value: 'r2',           label: 'R2 Sequence' },
    { value: 'crosshatch',   label: 'Crosshatch' },
    { value: 'radial',       label: 'Radial' },
    { value: 'value-noise',  label: 'Value Noise' },
  ]},
  { group: 'Lines', items: [
    { value: 'line2',         label: 'Line 2' },
    { value: 'line-checker',  label: 'Line Checker' },
    { value: 'line4',         label: 'Line 4' },
    { value: 'line8',         label: 'Line 8' },
    { value: 'vline2',        label: 'VLine 2' },
    { value: 'vline-checker', label: 'VLine Checker' },
    { value: 'vline4',        label: 'VLine 4' },
    { value: 'vline8',        label: 'VLine 8' },
  ]},
  { group: 'Error Diffusion', items: [
    { value: 'ostromoukhov',    label: 'Ostromoukhov' },
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
  { key: 'errorClamp',    label: 'Error Clamp', min: 0,   max: 1.0, step: 0.025, default: 0.35,
    tip: 'Max error accumulation per channel (squared internally). Lower = cleaner, higher = more dithering noise.' },
]

// CGA-text-mode-only options. Shown only when mode === 'cga-text80x100'.
export const CGA_TEXT_METRICS = [
  { value: 'blur', label: 'Pappas-Neuhoff' },
  { value: 'mse',  label: 'Per-pixel MSE' },
]
export const CGA_TEXT_DEFAULTS = {
  cgaTextMetric: 'blur',
}

export const EXAMPLES = [
  { name: 'electrichues', file: 'electrichues02.jpg', opts: { mode: 'lores', depth: 5, dither: 'ostromoukhov', ditherStrength: 0.5 } },
  { name: 'fantasy',      file: 'fantasy.png',        opts: { mode: 'ham6', dither: 'ostromoukhov', copper: true } },
  { name: 'lovers',       file: 'lovers.jpg',         opts: { mode: 'lores', depth: 5, dither: 'ostromoukhov', ditherStrength: 0.5 } },
  { name: 'logo',         file: 'logo.png',           opts: { mode: 'lores', depth: 5, dither: 'ostromoukhov', alphaThreshold: 0 } },
  { name: 'space',        file: 'space3.png',          opts: { mode: 'lores', depth: 5, dither: 'ostromoukhov', ditherStrength: 0.5 } },
  { name: 'photo',        file: 'photo.jpg',           opts: { mode: 'ham6', dither: 'ostromoukhov', copper: true } },
  { name: 'grungy',       file: 'grungy.png',          opts: { mode: 'lores', depth: 3, dither: 'sierra-lite', ditherStrength: 1.2, brightness: -0.05, contrast: 0.9, gamma: 1.0, copper: true } },
  { name: 'fantasy1',     file: 'fantasy1.png',        opts: { mode: 'lores', depth: 3, dither: 'ostromoukhov', copper: true } },
]

export function defaultOptions() {
  const opts = {
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
  }
  for (const s of [...SLIDERS, ...DIFFUSION_SLIDERS]) opts[s.key] = s.default
  Object.assign(opts, CGA_TEXT_DEFAULTS)
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
  return mode === 'ehb' || mode === 'ehb-lace'
}

export function isAtariMode(mode) {
  return mode.startsWith('stf-') || mode.startsWith('ste-')
}

export function isVgaMode(mode) { return mode.startsWith('vga-') }
export function isEgaMode(mode) { return mode.startsWith('ega-') }
export function isCgaMode(mode) { return mode.startsWith('cga-') }
export function isDosMode(mode) {
  return isVgaMode(mode) || isEgaMode(mode) || isCgaMode(mode)
}

// Hardware Pixel Aspect Ratio (display_pixel_width / display_pixel_height).
// Mirrors ModeParams::par in src/amiga.hpp. Used to CSS-stretch the preview
// canvas so it displays with the correct aspect (native-par on the web).
//   <1 tall pixels (e.g. EGA 640×200 = 0.417 — pixels are 2.4× taller than wide)
//    1 square
//   >1 wide pixels (e.g. CGA composite 160×200 = 1.667)
const MODE_PAR = {
  'vga-13h':    0.833,
  'vga-10h':    0.730,
  'vga-12h':    1.0,
  'ega-320':    0.833,
  'ega-640':    0.417,
  'ega-hi':     0.730,
  'cga-320':    0.833,
  'cga-640':    0.417,
  'cga-composite': 1.667,
  'cga-text80x100': 0.417,
}

export function modePar(mode) { return MODE_PAR[mode] ?? 1.0 }

const ERROR_DIFFUSION = new Set(['floyd-steinberg', 'atkinson', 'sierra-lite', 'stucki', 'jarvis', 'ostromoukhov'])

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
  // DOS modes have non-integer PAR (e.g. VGA 13h ~ 0.83 tall). The WASM
  // encoder returns the raw hardware buffer (e.g. 640x350, 640x480, 640x200);
  // we nearest-neighbor scale it on-canvas using whole factors so the
  // preview is readable. 640-wide buffers already fill most screen widths,
  // so don't double them; 320-wide DOS buffers double horizontally.
  if (mode === 'cga-320' || mode === 'ega-320' || mode === 'vga-13h')
    return { sx: 2, sy: 2 }
  if (mode === 'cga-composite')               return { sx: 4, sy: 2 } // 160x200 → stretch
  if (mode === 'vga-12h')                     return { sx: 1, sy: 1 }
  // Text modes: renderer returns cols*8 × rows*cell_h; pixels already
  // account for the scanline count so just pick a display sy that
  // approximates 4:3 on a modern display.
  if (mode === 'vga-10h' || mode === 'ega-hi') return { sx: 1, sy: 1 }
  if (mode === 'ega-640' || mode === 'cga-640' ||
      mode === 'cga-text80x100') return { sx: 1, sy: 2 }
  const hi = isHiresMode(mode) || mode.endsWith('-med') || mode.endsWith('-hi')
  const lace = isInterlaceMode(mode) || mode.endsWith('-hi')  // -hi is 640x400 square pixels
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
  if (isHamMode(mode) || isEhbMode(mode) || isAtariMode(mode)) return 0
  if (isDosMode(mode)) return 0  // DOS modes have fixed depth per hardware
  if (isHiresMode(mode)) return chipset === 'aga' ? 8 : 4
  return chipset === 'aga' ? 8 : 5
}

// Default bitplane depth for a given mode
export function defaultDepth(mode) {
  if (isEhbMode(mode)) return 6
  const ham = hamType(mode)
  if (ham === 'ham6') return 6
  if (ham === 'ham8') return 8
  if (mode === 'stf-low' || mode === 'ste-low') return 4
  if (mode === 'stf-med' || mode === 'ste-med') return 2
  if (mode === 'stf-hi' || mode === 'ste-hi') return 1
  // DOS modes: depth fixed by hardware.
  if (mode === 'cga-640') return 1
  if (mode === 'cga-320' || mode === 'cga-composite') return 2
  if (mode === 'vga-13h') return 8
  if (mode.startsWith('cga-text') || mode.startsWith('ega-text')) return 4
  if (isEgaMode(mode) || isVgaMode(mode)) return 4
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
