// Audit: no Amiga-only control may be reachable from a non-Amiga mode.
// Mirrors the gates in Converter.vue so a regression there is caught here.
import { describe, expect, it } from 'vitest'

import {
  MODES,
  isAmigaMode,
  isHamMode,
  isEhbMode,
  isAtariMode,
  supportsCustomPalette,
} from './options.js'

const showDepthSlider = (m: string) => isAmigaMode(m) && !isHamMode(m) && !isEhbMode(m)
const slicedAvailable = (m: string) => isAmigaMode(m)
const dpfAvailable = (m: string) => isAmigaMode(m) && !isHamMode(m) && !isEhbMode(m) && !isAtariMode(m)

describe('non-Amiga mode sweep', () => {
  const nonAmiga = MODES.filter(m => !isAmigaMode(m.value)).map(m => m.value)

  it('covers every shipped mode', () => {
    expect(MODES.length).toBeGreaterThan(40)
    expect(nonAmiga.length).toBeGreaterThan(20)
  })

  it('hides copper-only controls (sliced / dual playfield / depth) everywhere non-Amiga', () => {
    const leaks = nonAmiga.filter(m => slicedAvailable(m) || dpfAvailable(m) || showDepthSlider(m))
    expect(leaks).toEqual([])
  })

  it('offers Custom Palette only where the encoder honours it', () => {
    // Verified against the CLI per family: C64 / CGA-text / SNES / Genesis
    // ignore an uploaded palette; Thomson / TED / GBA-direct reject it.
    const shouldNotOffer = nonAmiga.filter(m =>
      /^c64-|^snes-|^genesis-|^thomson-|^ted-|^cga-text/.test(m) || m === 'gba-mode3' || m === 'gba-mode5')
    expect(shouldNotOffer.filter(m => supportsCustomPalette(m))).toEqual([])
    // ...and still offers it where it works (DOS bitmap, GBA mode4, Atari).
    for (const m of ['vga-13h', 'vga-12h', 'ega-320', 'cga-320', 'gba-mode4', 'stf-low']) {
      expect(supportsCustomPalette(m), m).toBe(true)
    }
  })
})
