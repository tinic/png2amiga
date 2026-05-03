// Unit tests for pure helpers in options.ts. The data tables (MODES,
// CHIPSETS, DITHER_METHODS, EXAMPLES) are validated implicitly by
// modesForChipset / decomposeMode / defaultDepth round-trips.

import { describe, expect, it } from 'vitest'

import {
  CHIPSETS,
  decomposeMode,
  defaultDepth,
  defaultOptions,
  effectiveChipset,
  hamType,
  isAtariMode,
  isCgaMode,
  isDosMode,
  isEgaMode,
  isEhbMode,
  isErrorDiffusion,
  isHamMode,
  isHiresMode,
  isInterlaceMode,
  isVgaMode,
  maxDepth,
  modePar,
  modesForChipset,
  numBitplanes,
  numColors,
  previewScale,
} from './options.js'

describe('hamType', () => {
  it.each([
    ['ham6', 'ham6'],
    ['ham8', 'ham8'],
    ['ham6-hires-lace', 'ham6'],
    ['ham8-lace', 'ham8'],
    ['lores', null],
    ['ehb', null],
    ['stf-low', null],
  ] as const)('hamType(%s) === %s', (mode, expected) => {
    expect(hamType(mode)).toBe(expected)
  })
})

describe('mode predicates', () => {
  it('isHamMode covers ham4..ham8 prefixes', () => {
    expect(isHamMode('ham6')).toBe(true)
    expect(isHamMode('ham8-hires')).toBe(true)
    expect(isHamMode('lores')).toBe(false)
    expect(isHamMode('ehb')).toBe(false)
  })

  it('isEhbMode covers both progressive and interlace', () => {
    expect(isEhbMode('ehb')).toBe(true)
    expect(isEhbMode('ehb-lace')).toBe(true)
    expect(isEhbMode('ham6')).toBe(false)
  })

  it('isAtariMode covers stf-* and ste-*', () => {
    expect(isAtariMode('stf-low')).toBe(true)
    expect(isAtariMode('ste-hi')).toBe(true)
    expect(isAtariMode('lores')).toBe(false)
    expect(isAtariMode('vga-13h')).toBe(false)
  })

  it('isVgaMode/isEgaMode/isCgaMode are mutually exclusive', () => {
    const dos = ['vga-13h', 'ega-320', 'cga-320']
    for (const m of dos) {
      const hits = [isVgaMode(m), isEgaMode(m), isCgaMode(m)].filter(Boolean)
      expect(hits).toHaveLength(1)
    }
  })

  it('isDosMode is the union of vga/ega/cga', () => {
    expect(isDosMode('vga-13h')).toBe(true)
    expect(isDosMode('ega-320')).toBe(true)
    expect(isDosMode('cga-320')).toBe(true)
    expect(isDosMode('ham6')).toBe(false)
    expect(isDosMode('stf-low')).toBe(false)
  })

  it('isInterlaceMode catches the -lace suffix', () => {
    expect(isInterlaceMode('lores-lace')).toBe(true)
    expect(isInterlaceMode('ham6-hires-lace')).toBe(true)
    expect(isInterlaceMode('lores')).toBe(false)
    expect(isInterlaceMode('stf-hi')).toBe(false)  // -hi is not -lace
  })

  it('isHiresMode catches "hires" anywhere in the mode', () => {
    expect(isHiresMode('hires')).toBe(true)
    expect(isHiresMode('ham8-hires')).toBe(true)
    expect(isHiresMode('lores')).toBe(false)
  })
})

describe('decomposeMode', () => {
  it('non-HAM modes pass through with default width', () => {
    expect(decomposeMode('lores')).toEqual({ mode: 'lores', width: 0, interlace: false })
    expect(decomposeMode('ehb')).toEqual({ mode: 'ehb', width: 0, interlace: false })
  })

  it('HAM6 progressive', () => {
    expect(decomposeMode('ham6')).toEqual({ mode: 'ham6', width: 0, interlace: false })
  })

  it('HAM6 hires forces 640px', () => {
    expect(decomposeMode('ham6-hires')).toEqual({ mode: 'ham6', width: 640, interlace: false })
  })

  it('HAM6 hires + lace flags both', () => {
    expect(decomposeMode('ham6-hires-lace')).toEqual({ mode: 'ham6', width: 640, interlace: true })
  })

  it('HAM8 lace without hires', () => {
    expect(decomposeMode('ham8-lace')).toEqual({ mode: 'ham8', width: 0, interlace: true })
  })
})

describe('maxDepth', () => {
  it('HAM/EHB/Atari/DOS modes return 0 (depth fixed by hardware)', () => {
    expect(maxDepth('ham6', 'ocs')).toBe(0)
    expect(maxDepth('ehb', 'ocs')).toBe(0)
    expect(maxDepth('stf-low', 'stf')).toBe(0)
    expect(maxDepth('vga-13h', 'vga')).toBe(0)
  })

  it('lores: 5 on OCS, 8 on AGA', () => {
    expect(maxDepth('lores', 'ocs')).toBe(5)
    expect(maxDepth('lores', 'aga')).toBe(8)
  })

  it('hires: 4 on OCS, 8 on AGA', () => {
    expect(maxDepth('hires', 'ocs')).toBe(4)
    expect(maxDepth('hires', 'aga')).toBe(8)
  })
})

describe('defaultDepth', () => {
  it.each([
    ['ham6',     6],
    ['ham8',     8],
    ['ehb',      6],
    ['stf-low',  4],
    ['stf-med',  2],
    ['stf-hi',   1],
    ['ste-low',  4],
    ['cga-640',  1],
    ['cga-320',  2],
    ['vga-13h',  8],
    ['ega-320',  4],
    ['hires',    4],
    ['lores',    5],
  ] as const)('defaultDepth(%s) === %d', (mode, expected) => {
    expect(defaultDepth(mode)).toBe(expected)
  })
})

describe('numColors', () => {
  it('EHB always returns "64"', () => {
    expect(numColors('ehb', 6)).toBe('64')
  })

  it('HAM6 returns "16 base + modify"', () => {
    expect(numColors('ham6', 6)).toBe('16 base + modify')
  })

  it('HAM8 returns "64 base + modify"', () => {
    expect(numColors('ham8', 8)).toBe('64 base + modify')
  })

  it('non-HAM/EHB returns 2^depth as a string', () => {
    expect(numColors('lores', 5)).toBe('32')
    expect(numColors('lores', 1)).toBe('2')
    expect(numColors('hires', 4)).toBe('16')
  })
})

describe('numBitplanes', () => {
  it('EHB always 6', () => {
    expect(numBitplanes('ehb', 3)).toBe(6)
  })

  it('HAM mirrors defaultDepth', () => {
    expect(numBitplanes('ham6', 3)).toBe(6)
    expect(numBitplanes('ham8', 3)).toBe(8)
  })

  it('standard modes pass through depth', () => {
    expect(numBitplanes('lores', 5)).toBe(5)
    expect(numBitplanes('hires', 3)).toBe(3)
  })
})

describe('effectiveChipset', () => {
  it('HAM8 forces AGA', () => {
    expect(effectiveChipset('ham8', 'ocs')).toBe('aga')
    expect(effectiveChipset('ham8-hires-lace', 'ocs')).toBe('aga')
  })

  it('HAM6 keeps user choice', () => {
    expect(effectiveChipset('ham6', 'ocs')).toBe('ocs')
    expect(effectiveChipset('ham6', 'aga')).toBe('aga')
  })

  it('non-HAM keeps user choice', () => {
    expect(effectiveChipset('lores', 'ocs')).toBe('ocs')
    expect(effectiveChipset('hires', 'aga')).toBe('aga')
  })
})

describe('isErrorDiffusion', () => {
  it.each([
    ['floyd-steinberg', true],
    ['atkinson',         true],
    ['sierra-lite',      true],
    ['stucki',           true],
    ['jarvis',           true],
    ['fs-ostro',         true],
    ['ostromoukhov',     true],  // back-compat alias
    ['none',             false],
    ['bayer8x8',         false],
    ['checker',          false],
    ['blue-noise',       false],
  ] as const)('isErrorDiffusion(%s) === %s', (d, expected) => {
    expect(isErrorDiffusion(d)).toBe(expected)
  })
})

describe('modePar', () => {
  it('returns 1 for unknown modes (square pixels)', () => {
    expect(modePar('lores')).toBe(1)
    expect(modePar('ham6')).toBe(1)
    expect(modePar('zzz-not-a-mode')).toBe(1)
  })

  it('DOS modes have known PAR overrides', () => {
    expect(modePar('vga-13h')).toBeCloseTo(0.833)
    expect(modePar('ega-640')).toBeCloseTo(0.417)
    expect(modePar('cga-composite')).toBeCloseTo(1.667)
  })
})

describe('previewScale', () => {
  it('lores progressive defaults to 2x2', () => {
    expect(previewScale('lores')).toEqual({ sx: 2, sy: 2 })
  })

  it('lores interlace stretches X by 2', () => {
    expect(previewScale('lores-lace')).toEqual({ sx: 2, sy: 1 })
  })

  it('hires progressive stretches Y by 2', () => {
    expect(previewScale('hires')).toEqual({ sx: 1, sy: 2 })
  })

  it('hires interlace renders 1:1 (square pixels)', () => {
    expect(previewScale('hires-lace')).toEqual({ sx: 1, sy: 1 })
  })

  it('CGA composite stretches 4x2 (160-wide → wide pixels)', () => {
    expect(previewScale('cga-composite')).toEqual({ sx: 4, sy: 2 })
  })

  it('VGA 13h doubles both axes', () => {
    expect(previewScale('vga-13h')).toEqual({ sx: 2, sy: 2 })
  })
})

describe('modesForChipset', () => {
  it('OCS shows OCS-only Amiga modes', () => {
    const modes = modesForChipset('ocs').map(m => m.value)
    expect(modes).toContain('lores')
    expect(modes).toContain('hires')
    expect(modes).toContain('ham6')
    expect(modes).not.toContain('ham8')      // AGA-only
    expect(modes).not.toContain('vga-13h')   // wrong family
    expect(modes).not.toContain('stf-low')   // wrong family
  })

  it('AGA shows OCS + AGA Amiga modes', () => {
    const modes = modesForChipset('aga').map(m => m.value)
    expect(modes).toContain('lores')
    expect(modes).toContain('ham8')
    expect(modes).toContain('ham8-hires')
    expect(modes).not.toContain('vga-13h')
  })

  it('non-Amiga chipsets isolate their own family', () => {
    expect(modesForChipset('vga').every(m => m.chipset === 'vga')).toBe(true)
    expect(modesForChipset('cga').every(m => m.chipset === 'cga')).toBe(true)
    expect(modesForChipset('stf').every(m => m.chipset === 'stf')).toBe(true)
  })
})

describe('CHIPSETS table', () => {
  it('every chipset value is unique', () => {
    const values = CHIPSETS.map(c => c.value)
    expect(new Set(values).size).toBe(values.length)
  })
})

describe('defaultOptions', () => {
  it('returns a fresh object each call (no shared mutable state)', () => {
    const a = defaultOptions()
    const b = defaultOptions()
    expect(a).not.toBe(b)
    a.gamma = 99
    expect(b.gamma).not.toBe(99)
  })

  it('seeds slider keys from the SLIDERS table', () => {
    const opts = defaultOptions()
    expect(opts.gamma).toBe(1)
    expect(opts.brightness).toBe(0)
    expect(opts.contrast).toBe(1)
    expect(opts.saturation).toBe(1)
    expect(opts.errorClamp).toBe(0.35)
  })

  it('seeds the CGA text metric default', () => {
    expect(defaultOptions().cgaTextMetric).toBe('blur')
  })
})
