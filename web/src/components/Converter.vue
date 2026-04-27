<script setup lang="ts">
import { ref, reactive, watch, nextTick, computed, onBeforeUnmount, useTemplateRef } from 'vue'
import type { ConvertResult } from '@wasm/png2amiga.js'
import InputNumber from 'primevue/inputnumber'
import Select from 'primevue/select'
import Slider from 'primevue/slider'
import ToggleSwitch from 'primevue/toggleswitch'
import Button from 'primevue/button'
import ProgressSpinner from 'primevue/progressspinner'
import ProgressBar from 'primevue/progressbar'
import Panel from 'primevue/panel'

import type { CrtRenderer } from '../lib/crt.js'
import {
  CHIPSETS, DITHER_METHODS, ALPHA_DITHER_METHODS,
  SLIDERS, DIFFUSION_SLIDERS, CGA_TEXT_METRICS, EXAMPLES,
  defaultOptions, isHamMode, hamType, isEhbMode, isAtariMode, isErrorDiffusion,
  isDosMode, isVgaMode, isEgaMode, modePar,
  maxDepth, defaultDepth, effectiveChipset, previewScale,
  modesForChipset,
} from '../lib/options.js'
import { track } from '../lib/analytics.js'
import { useImageUpload } from '../composables/useImageUpload.js'
import { useWasm } from '../composables/useWasm.js'

const { loading: wasmLoading, error: wasmError, convertRGBA, convertPNG, convertIFF, convertViewer, convertDegas, convertRaw, convertMask, convertMaskRaw, ditherDefaults } = useWasm()
const { imageBytes, imageName, imageUrl, imageWidth, imageHeight, dragOver, uploadTimestamp, onDrop, onDragOver, onDragLeave, openPicker } = useImageUpload()

const showUploadHint = ref(true)

// Load first example by default once WASM is ready
watch(wasmLoading, async (loading) => {
  if (loading || wasmError.value || imageBytes.value) return
  const example = EXAMPLES[0]
  if (!example) return
  try {
    const r = await fetch(`/examples/${example.file}`)
    const buf = await r.arrayBuffer()
    imageBytes.value = new Uint8Array(buf)
    imageName.value = example.file
    const type = example.file.endsWith('.jpg') || example.file.endsWith('.jpeg') ? 'image/jpeg' : 'image/png'
    const blob = new Blob([buf], { type })
    imageUrl.value = URL.createObjectURL(blob)
    // Decode dimensions so resize presets work for the default example.
    const img = new Image()
    img.addEventListener('load', () => {
      imageWidth.value = img.width
      imageHeight.value = img.height
    })
    img.src = imageUrl.value
  } catch (error) {
    console.warn('failed to load default example:', error)
  }
})

const options = reactive(defaultOptions())
const canvasRef = useTemplateRef<HTMLCanvasElement>('canvasRef')
const crtCanvasRef = useTemplateRef<HTMLCanvasElement>('crtCanvasRef')  // WebGL CRT-preview overlay canvas
const crtEnabled = ref(false)
const converting = ref(false)
const progress = ref(0)         // 0..100 — encoder progress for slow paths
const progressStage = ref('')

// Lazy-init the CRT renderer the first time --crt is toggled on; persist
// across re-renders. Only torn down on unmount.
let crtRenderer: CrtRenderer | null = null
let lastRgba: Uint8Array | null = null   // cached source RGBA so a CRT toggle
let lastSrc = { w: 0, h: 0 }             // change can re-render without re-encoding
let lastDst = { w: 0, h: 0 }
async function ensureCrtRenderer(): Promise<CrtRenderer | null> {
  if (crtRenderer || !crtCanvasRef.value) return crtRenderer
  const { createCrtRenderer } = await import('../lib/crt.js')
  try {
    crtRenderer = createCrtRenderer(crtCanvasRef.value)
  } catch (error) {
    errorMsg.value = `CRT: ${error instanceof Error ? error.message : String(error)}`
    crtEnabled.value = false
    return null
  }
  return crtRenderer
}
function renderCrt() {
  if (!lastRgba || !crtRenderer) return
  // Backing-store sizing is decoupled from the displayed (CSS) size.
  // We need ≥4 output rows per source row for the Gaussian beam to
  // resolve as scanlines instead of a dot grid (with only 2 rows per
  // source row, hardScan=-8 alternates ~1→~0 in adjacent rows and
  // the mask modulates each one independently → dot pattern, not
  // scanlines). For interlace we soften scanlines anyway so 2 rows
  // suffices. Width oversamples so the slot mask gets ≥6 output pixels
  // per RGB triad and reads as continuous stripes.
  const isInterlace = lastSrc.h >= 280
  const yScale = isInterlace ? 2 : 4
  const xScale = Math.max(2, Math.ceil(1024 / lastSrc.w))
  const dw = lastSrc.w * xScale
  const dh = lastSrc.h * yScale
  if (crtCanvasRef.value) {
    // Display size matches the regular preview's lastDst so the mode
    // aspect ratio (lores 2:1, hires 1:2 etc.) is preserved.
    crtCanvasRef.value.style.width  = `${lastDst.w}px`
    crtCanvasRef.value.style.height = `${lastDst.h}px`
  }
  crtRenderer.render(lastRgba, lastSrc.w, lastSrc.h, dw, dh)
}
const resultInfo = ref('')
const errorMsg = ref('')
const sizeOverride = ref(false)
const lastWidth = ref(320)
const lastHeight = ref(160)
const lastCopPerLine = ref(0)
const lastPlaneBytes = ref(0)
const lastCopperBytes = ref(0)
const lastChangesPerLine = ref(0)
const lastMaxMovesPerLine = ref(0)
const lastAga = ref(false)
const imageHasAlpha = ref(false)
const paletteData = ref<Uint8Array | null>(null)    // raw palette file bytes
const paletteColors = ref<string[]>([])              // parsed CSS color strings for preview
const pageLoadTime = Date.now()

// Loupe (4x zoom with drag-to-pan)
interface DragStart { x: number; y: number; ox: number; oy: number }

const loupeActive = ref(false)
const loupeX = ref(0)  // pan offset in CSS pixels (negative = scrolled right/down)
const loupeY = ref(0)
const loupeHeight = ref<string | null>(null)
const previewColRef = useTemplateRef<HTMLElement>('previewColRef')
let dragStart: DragStart | null = null    // { x, y, ox, oy } while dragging

function loupeToggle() {
  loupeActive.value = !loupeActive.value
  loupeX.value = 0
  loupeY.value = 0
  if (loupeActive.value && previewColRef.value) {
    const rect = previewColRef.value.getBoundingClientRect()
    loupeHeight.value = `${window.innerHeight - rect.top - 16}px`
  } else {
    loupeHeight.value = null
  }
}
function loupePointerDown(e: PointerEvent) {
  if (!loupeActive.value) return
  const target = e.target as HTMLElement | null
  if (target?.closest('.loupe-btn')) return
  dragStart = { x: e.clientX, y: e.clientY, ox: loupeX.value, oy: loupeY.value }
  ;(e.currentTarget as HTMLElement).setPointerCapture(e.pointerId)
}
function loupePointerMove(e: PointerEvent) {
  if (!dragStart) return
  // Use whichever canvas is currently visible. With v-show, the hidden
  // one's clientWidth/Height is 0 so picking the wrong one would clamp
  // the pan to zero and the zoomed view wouldn't scroll.
  const canvas = crtEnabled.value ? crtCanvasRef.value : canvasRef.value
  if (!canvas) return
  const maxX = canvas.clientWidth * 3
  const maxY = canvas.clientHeight * 3
  loupeX.value = Math.min(0, Math.max(-maxX, dragStart.ox + (e.clientX - dragStart.x)))
  loupeY.value = Math.min(0, Math.max(-maxY, dragStart.oy + (e.clientY - dragStart.y)))
}
function loupePointerUp() {
  dragStart = null
}
let firstConvertTracked = false
let exportCount = 0

// Flatten dither methods for grouped Select component.
// HAM8 (AGA only) has 6-bit MODIFY channels and 24-bit base palette,
// so the error-diffusion pre-dither is a no-op — hide the whole group.
// HAM6 supports F-S/Atkinson/etc. but Ostromoukhov falls back to F-S
// in HAM's inline pre-dither (variable coefficients can't be applied
// to the chipset-precision quantization), so hide it specifically.
const groupedDitherOptions = computed(() => {
  const ht = hamType(options.mode)
  // HAM: Ostromoukhov's variable-coefficient kernel degrades to plain
  // F-S in the pre-dither pass (both HAM6 and HAM8), so hide it to
  // avoid confusion.
  const hide_ostro = ht !== null
  // HAM modes: the C64-lineage non-square patterns (h2x4/v4x2/bayer4x2/
  // bayer2x4) target C64 multicolor pixel ratios, not Amiga square
  // pixels. They don't crash but produce mis-proportioned dither for
  // HAM — hide to avoid user confusion.
  const hide_nonsquare = ht !== null
  return DITHER_METHODS
    .filter(g => !(hide_nonsquare && g.group === 'Non-square'))
    .map(g => ({
      label: g.group,
      items: g.items
        .filter(d => !(hide_ostro && d.value === 'ostromoukhov'))
        .map(d => ({ value: d.value, label: d.label }))
    }))
})

// Flat list of all dither values for prev/next cycling
const allDitherValues = computed(() =>
  groupedDitherOptions.value.flatMap(g => g.items.map(d => d.value))
)

function cycleDither(dir: number) {
  const vals = allDitherValues.value
  const idx = vals.indexOf(options.dither)
  const next = (idx + dir + vals.length) % vals.length
  const v = vals[next]
  if (v) options.dither = v
}

// Whether depth slider should be shown. Hidden for modes where depth is
// fixed by the target hardware: HAM (N-2 data bits), EHB (always 6),
// Atari (mode defines depth), and all DOS modes (EGA/VGA/CGA/text — 1/2/4/8).
const showDepthSlider = computed(() => {
  return !isHamMode(options.mode) && !isEhbMode(options.mode) &&
         !isAtariMode(options.mode) && !isDosMode(options.mode)
})

// Raw export tooltip with format layout (HTML for fixed-width font).
// SAFETY: PrimeVue's v-tooltip below uses escape:false so this string is
// inserted as raw HTML. Every interpolation MUST be numeric/bool and is
// coerced via Number() / Boolean() below so a future regression that
// smuggles a string with HTML special characters can't turn into XSS.
interface RawSection { off: number; size: number; label: string; detail: string }

function rawHeaderLines(): string[] {
  return [
    `Raw binary format (big-endian):`,
    ``,
    `Offset  Size     Content`,
    `------  -------  ----------------------------`,
  ]
}

function formatRawRow(s: RawSection): string[] {
  const offHex = s.off.toString(16).padStart(4, '0')
  const sizeStr = s.size.toLocaleString().padStart(7)
  return [`0x${offHex}  ${sizeStr}  ${s.label}`, `                 ${s.detail}`]
}

interface RawLayoutInputs { d: number; bpr: number; aga: boolean; colors: number; pb: number; copPerPass: number; cpl: number; h: number }

function rawSections({ d, bpr, aga, colors, pb, copPerPass, cpl, h }: RawLayoutInputs): RawSection[] {
  const palSize = colors * 2
  const sections: RawSection[] = []
  let off = 0
  sections.push({ off, size: pb, label: 'Bitplanes', detail: `(${d}bpl, ${bpr}B/row, interleaved)` }); off += pb
  sections.push({ off, size: palSize, label: `Palette${aga ? ' hi' : ''}`, detail: `(${colors} * u16, ${aga ? 'hi nibbles 0x0RGB' : '0x0RGB'})` }); off += palSize
  if (aga) {
    sections.push({ off, size: palSize, label: 'Palette lo', detail: `(${colors} * u16, lo nibbles 0x0RGB)` }); off += palSize
  }
  if (copPerPass > 0) {
    sections.push({ off, size: copPerPass, label: `Copper${aga ? ' hi' : ''}`, detail: `((u8:0+u8:reg+u16:col) * ${cpl}/line, ${h} lines)` }); off += copPerPass
    if (aga) {
      sections.push({ off, size: copPerPass, label: 'Copper lo', detail: `((u8:0+u8:reg+u16:col) * ${cpl}/line, ${h} lines)` })
    }
  }
  return sections
}

const rawTooltipHtml = computed(() => {
  const n = (v: unknown) => Number(v) || 0
  const w = n(lastWidth.value)
  const h = n(lastHeight.value)
  const d = n(options.depth || defaultDepth(options.mode))
  const dd = n(defaultDepth(options.mode))
  const bpr = Math.ceil(w / 16) * 2
  // Encoder-reported chipset, not the user option (HAM7/8 force AGA).
  const aga = lastAga.value
  const colors = 1 << (isHamMode(options.mode) ? dd - 2 : d)
  const pb = n(lastPlaneBytes.value)
  const cb = n(lastCopperBytes.value)
  // .raw uses fixed [h][cpl] grid with sentinels for unused/skipped slots.
  const cpl = n(lastChangesPerLine.value)
  const copPerPass = aga && cb ? cb / 2 : cb
  const sections = rawSections({ d, bpr, aga, colors, pb, copPerPass, cpl, h })
  const total = sections.reduce((sum, s) => sum + s.size, 0)
  const lines = [
    ...rawHeaderLines(),
    ...sections.flatMap(s => formatRawRow(s)),
    `------  -------  ----------------------------`,
    `Total:  ${total.toLocaleString().padStart(7)}  ${w}x${h}, ${d}bpl, ${aga ? 'AGA 24-bit' : 'OCS 12-bit'}`,
  ]
  return `<pre style="margin:0;font-size:0.7rem;line-height:1.3;white-space:pre">${lines.join('\n')}</pre>`
})

// Whether HAM controls should be shown
const showHamControls = computed(() => isHamMode(options.mode))

// Dual playfield: only valid for standard Amiga modes (no HAM, no EHB,
// no Atari/DOS) at the matching depth for the current chipset (3 for
// OCS = 8 PF2 colours, 4 for AGA = 16).
//
// OCS hires is excluded (any -lace variant too): OCS hires caps at 4
// bitplanes total, so DPF would split 2+2 giving only 4 colours per
// playfield, and the chipset doesn't officially support hires+DPF.
//
// OCS lores-lace + DPF IS allowed — BPLCON0's LACE bit (2) and DBLPF
// bit (10) are independent and can be set together. The hardware does
// 320×400 with two 8-colour playfields fine, even though the
// combination flickers on consumer monitors without scan-doubling.
// AGA hires + DPF (depth=4 → 4+4) is also fine.
const dpfAvailable = computed(() => {
  const m = options.mode
  if (isHamMode(m) || isEhbMode(m) || isAtariMode(m) || isDosMode(m)) return false
  const cs = effectiveChipset(m, options.chipset)
  if (cs === 'aga') return options.depth === 4
  return options.depth === 3 && !m.includes('hires')
})

// SCAP — mid-line palette swaps. Two flavours, both OCS lores only:
//   * DPF + lores (depth=3): 3-plane PF2, 8 base colours.
//   * EHB (mode=ehb): 32 base + 32 hardware-derived half-brites.
// SCAP is an extension to CAP (per-line palette evolution); enabling
// SCAP turns CAP on too, and turning CAP off cascades SCAP off.
const scapAvailable = computed(() => {
  const cs = effectiveChipset(options.mode, options.chipset)
  if (cs !== 'ocs') return false
  // DPF lores at depth=3
  if (options.dualPlayfield && options.mode === 'lores' &&
      options.depth === 3) return true
  // EHB (depth is fixed at 6 for EHB; mode string identifies it)
  if (options.mode === 'ehb') return true
  return false
})

// Available modes for current chipset
const availableModes = computed(() => modesForChipset(options.chipset))

// Current depth max for the slider
const depthMax = computed(() => maxDepth(options.mode, options.chipset))

// Copper changes slider range: static 0..16 across all modes. 0 = auto
// (safe default, backend picks worst-case K from the 14-MOVE budget); any
// value above the safe K is an explicit user override and may overshoot
// real hardware — we let people try it anyway.
const copperMax = computed(() => 32)

// Effective chipset label for status line
const statusChipset = computed(() => {
  return effectiveChipset(options.mode, options.chipset).toUpperCase()
})

// Palette mismatch warnings
const paletteMismatchDepth = computed(() => {
  if (!paletteColors.value.length) return ''
  const d = options.depth || defaultDepth(options.mode)
  const maxColors = 1 << d
  const n = paletteColors.value.length
  if (isEhbMode(options.mode)) {
    if (n !== 32) return `EHB needs 32 colors, palette has ${n}`
    return ''
  }
  if (n > maxColors) return `Palette has ${n} colors, depth ${d} supports ${maxColors}`
  const idealDepth = Math.ceil(Math.log2(n || 1))
  if (d > idealDepth) return `Palette has ${n} colors, depth ${idealDepth} would suffice (using ${d})`
  return ''
})

const paletteMismatchMode = computed(() => {
  if (!paletteColors.value.length) return ''
  if (isHamMode(options.mode)) return 'Custom palette not supported for HAM'
  return ''
})

function clampDepthForMode(mode: string): void {
  const max = maxDepth(mode, options.chipset)
  // Fixed depth modes (HAM, EHB) — use their default. Otherwise clamp.
  if (max === 0) options.depth = defaultDepth(mode)
  else if (options.depth > max) options.depth = max
}

function syncNativeParToMode(mode: string, oldMode: string): void {
  // DOS modes default to native PAR (letterbox/pillarbox into fixed buffer)
  // so the preview shows the right aspect. Reset when entering a DOS mode
  // from non-DOS; leave alone between DOS modes so the user toggle sticks.
  if (isDosMode(mode) && !isDosMode(oldMode)) options.nativePar = true
  if (!isDosMode(mode)) options.nativePar = false
}

// Update depth when mode changes — only clamp, don't reset
watch(() => options.mode, (mode, oldMode) => {
  clampDepthForMode(mode)
  // HAM6: Ostromoukhov falls back to F-S in pre-dither → switch to F-S
  if (hamType(mode) === 'ham6' && options.dither === 'ostromoukhov') options.dither = 'floyd-steinberg'
  syncNativeParToMode(mode, oldMode)
  // DPF and SCAP both require chipset-/depth-specific shapes.
  if (!dpfAvailable.value) options.dualPlayfield = false
  if (!scapAvailable.value) options.scap = false
  track('mode-change', { from: oldMode, to: mode })
})

// DPF + copper now compose (copper branch in api.cpp expands to PF2 +
// shifts CAP register targets into the upper palette bank). Just track
// toggles. SCAP and copper still don't combine — SCAP supplies its own
// per-line copper stream.
watch(() => options.dualPlayfield, (on) => {
  if (!scapAvailable.value) options.scap = false
  track('dpf-toggle', { enabled: on })
})
watch(() => options.scap, (on) => {
  if (on) {
    // SCAP is an extension to CAP — make sure CAP is on too.
    options.copper = true
  }
  track('scap-toggle', { enabled: on })
})

// Turning Copper off pulls SCAP off too — SCAP layers mid-line moves
// on top of CAP and is meaningless without it. Disabling SCAP alone
// only removes those mid-line moves; CAP stays on.
watch(() => options.copper, (on) => {
  if (!on && options.scap) options.scap = false
})

// Depth changes can invalidate DPF (requires depth=3 OCS / 4 AGA) and
// SCAP (depth=3 OCS lores only). Mode/dpf watchers above don't fire
// when only the depth slider moves, so reset here too.
watch(() => options.depth, () => {
  if (!dpfAvailable.value) options.dualPlayfield = false
  if (!scapAvailable.value) options.scap = false
})

function maybeResetModeForChipset(): void {
  const modes = modesForChipset(options.chipset)
  if (modes.some(m => m.value === options.mode)) return
  const fallback = modes[0]
  if (!fallback) return
  options.mode = fallback.value
  options.depth = defaultDepth(options.mode)
}

// When chipset changes, reset mode if current mode isn't available
watch(() => options.chipset, (chipset, oldChipset) => {
  track('chipset-change', { from: oldChipset, to: chipset })
  maybeResetModeForChipset()
  if (isAtariMode(options.mode) || isDosMode(options.mode)) options.copper = false
  const max = maxDepth(options.mode, options.chipset)
  if (max > 0 && options.depth > max) options.depth = max
  // SCAP is OCS-only and DPF requires the chipset-specific depth — both
  // get invalidated when the chipset flips. The mode/dpf/depth watchers
  // above don't fire here, so reset directly.
  if (!dpfAvailable.value) options.dualPlayfield = false
  if (!scapAvailable.value) options.scap = false
})

// When size override is toggled, populate from last result or reset to 0
watch(sizeOverride, (on) => {
  if (on) {
    options.width = lastWidth.value
    options.height = lastHeight.value
  } else {
    options.width = 0
    options.height = 0
  }
})

// When a new image is loaded while resize override is active, drop the
// stale dimensions so the new image gets its natural defaults computed
// by the encoder. The post-convert hook below repopulates the inputs
// with the new auto-sized values once the conversion completes.
watch(imageBytes, () => {
  if (sizeOverride.value) {
    options.width = 0
    options.height = 0
  }
})

// Resize presets: scale source dimensions and write into width/height.
// Width is rounded to a 16-pixel boundary (Amiga bitplane requirement).
// Only callable when sizeOverride is already on (UI hides the buttons otherwise).
function setSizePreset(scale: number) {
  if (!imageWidth.value || !imageHeight.value) return
  options.width = Math.max(16, Math.round(imageWidth.value * scale / 16) * 16)
  options.height = Math.max(2, Math.round(imageHeight.value * scale))
}

// Aspect-ratio lock: when enabled, committing width or height keeps the
// other field proportional to the source image's aspect ratio. Sync only
// fires on blur or Enter — not per-keystroke — so the user sees stable
// intermediate values while typing.
const aspectLocked = ref(true)

function onWidthCommit() {
  if (!aspectLocked.value) return
  if (!imageWidth.value || !imageHeight.value || !options.width) return
  const aspect = imageHeight.value / imageWidth.value
  options.height = Math.max(2, Math.round(options.width * aspect))
}

function onHeightCommit() {
  if (!aspectLocked.value) return
  if (!imageWidth.value || !imageHeight.value || !options.height) return
  const aspect = imageWidth.value / imageHeight.value
  options.width = Math.max(16, Math.round(options.height * aspect / 16) * 16)
}

// Build the options object to pass to WASM (matches wasm_bindings.cpp field names)
function buildWasmOptions() {
  const opts = { ...options }
  if (opts.alphaDither === 'none') opts.alphaDither = ''
  if (!opts.paletteData) delete opts.paletteData
  // Pass compound mode string as-is — C++ decompose_mode_options handles it
  return opts
}

// Track dither changes
watch(() => options.dither, (to, from) => { track('dither-change', { from, to }) })
watch(() => options.copper, (enabled) => { track('copper-toggle', { enabled }) })

// CRT toggle: lazy-init the WebGL renderer on first use, re-render the
// cached preview without forcing a fresh encode pass.
watch(crtEnabled, async (on) => {
  track('crt-toggle', { enabled: on })
  if (on) {
    const r = await ensureCrtRenderer()
    if (r) renderCrt()
  }
})
onBeforeUnmount(() => {
  if (crtRenderer) { crtRenderer.dispose(); crtRenderer = null }
})

// Refresh errorClamp from the C++ per-mode tuning table whenever the mode
// (or any flag that feeds Context — copper/dpf/scap/depth/chipset) changes.
// Ignores whatever the user had set previously: the table value wins so a
// mode switch always lands on the empirically-tuned default for that mode.
async function refreshErrorClamp() {
  if (wasmLoading.value) return
  try {
    const d = await ditherDefaults({
      mode: options.mode,
      chipset: options.chipset,
      depth: options.depth,
      copper: options.copper,
      dualPlayfield: options.dualPlayfield,
      scap: options.scap,
    })
    if (typeof d.errorClamp === 'number') options.errorClamp = d.errorClamp
  } catch { /* WASM not ready yet — initial defaults stand */ }
}
watch(
  () => [options.mode, options.copper, options.dualPlayfield, options.scap,
         options.depth, options.chipset],
  () => { void refreshErrorClamp() })
// Also refresh once after WASM finishes loading so the very first preview
// uses the table value (defaultOptions() seeds with a generic 0.35).
watch(wasmLoading, (loading) => { if (!loading) void refreshErrorClamp() })

// Track slider tweaks (debounced)
let tweakTimer: ReturnType<typeof setTimeout> | null = null
for (const s of [...SLIDERS, ...DIFFUSION_SLIDERS]) {
  watch(() => options[s.key], (val) => {
    if (tweakTimer) clearTimeout(tweakTimer)
    tweakTimer = setTimeout(() => { track('setting-tweak', { key: s.key, value: val }); }, 500)
  })
}
watch(() => options.cgaTextMetric, (val) => {
  // The blur and pca metrics need a continuous source — pre-dithering
  // destroys the precision they need. Force dither=none whenever a
  // perceptual metric is selected.
  if (val !== 'mse' && options.dither !== 'none') options.dither = 'none'
  if (tweakTimer) clearTimeout(tweakTimer)
  tweakTimer = setTimeout(() => { track('setting-tweak', { key: 'cgaTextMetric', value: val }); }, 500)
})

// Session duration on page unload
onBeforeUnmount(() => {
  track('session-duration', { seconds: Math.round((Date.now() - pageLoadTime) / 1000) })
})
// Component runs only on the main thread; window is always present.
window.addEventListener('beforeunload', () => {
  track('session-duration', { seconds: Math.round((Date.now() - pageLoadTime) / 1000) })
})

let debounceTimer: ReturnType<typeof setTimeout> | null = null
let spinnerTimer: ReturnType<typeof setTimeout> | null = null

function formatBytes(b: number): string {
  return b >= 1024 ? `${(b / 1024).toFixed(1)}K` : `${b}B`
}

function formatSizeStats(result: ConvertResult): string {
  // diskBytes / chipBytes are the single source of truth (computed once
  // in api::make_result so they can't go stale across mode additions).
  if (!result.planeBytes || result.planeBytes <= 0) return ''
  return `, disk: ${formatBytes(result.diskBytes ?? 0)}, chip: ${formatBytes(result.chipBytes ?? 0)}`
}

function pushIf(parts: string[], cond: unknown, fragment: string): void {
  if (cond) parts.push(fragment)
}

function formatResultInfo(result: ConvertResult) {
  const colorCount = result.totalColors || result.colors || 0
  const parts = [`${result.width}x${result.height}, ${statusChipset.value}`,
                 `${result.depth || '?'}bpl, ${colorCount} colors`]
  pushIf(parts, result.copperChanges, `${(result.copperChanges ?? 0).toFixed(1)} avg CAP/line`)
  const sizeStats = formatSizeStats(result)
  pushIf(parts, sizeStats, sizeStats.slice(2))  // strip leading ", "
  pushIf(parts, result.quantError != null, `error: ${(result.quantError ?? 0).toFixed(2)}`)
  pushIf(parts, result.psnr != null && Number.isFinite(result.psnr), `PSNR: ${(result.psnr ?? 0).toFixed(1)} dB`)
  return parts.join(', ')
}

function paintPreviewCanvas(result: ConvertResult): { dw: number; dh: number } | null {
  const canvas = canvasRef.value
  if (!canvas || !result.rgba) return null
  const { sx, sy } = previewScale(options.mode)
  const dw = result.width * sx
  const dh = result.height * sy
  canvas.width = dw
  canvas.height = dh
  // Native-PAR preview (DOS modes only): keep the canvas backing at
  // integer-NN upscaled resolution for sharp pixels, but CSS-stretch
  // the displayed HEIGHT so each pixel renders with the hardware PAR.
  //   Target CSS aspect = buffer_w * par / buffer_h (the real CRT frame)
  // With width pinned to dw, height becomes dw * buffer_h / (buffer_w * par).
  // PAR < 1 (tall pixels → EGA 640×200 etc.) stretches height UP;
  // PAR > 1 (wide pixels → CGA composite) compresses height DOWN.
  if (isDosMode(options.mode) && options.nativePar) {
    const par = modePar(options.mode)
    const cssH = Math.round(dw * result.height / (result.width * par))
    canvas.style.width = `${dw}px`
    canvas.style.height = `${cssH}px`
  } else {
    canvas.style.width = `${dw}px`
    canvas.style.height = `${dh}px`
  }
  const ctx = canvas.getContext('2d')
  if (!ctx) return null
  ctx.imageSmoothingEnabled = false
  const tmp = document.createElement('canvas')
  tmp.width = result.width
  tmp.height = result.height
  const tmpCtx = tmp.getContext('2d')
  if (!tmpCtx) return null
  const imgData = new ImageData(
    new Uint8ClampedArray(result.rgba),
    result.width, result.height
  )
  tmpCtx.putImageData(imgData, 0, 0)
  ctx.drawImage(tmp, 0, 0, dw, dh)
  return { dw, dh }
}

function maybeSeedSizes(result: ConvertResult): void {
  // If size-override is on but width/height are 0 (fresh image just loaded),
  // seed the inputs with the natural defaults; triggers one idempotent
  // re-convert via the deep options watcher.
  if (!sizeOverride.value || (options.width && options.height)) return
  options.width = result.width
  options.height = result.height
}

function updateLastResultRefs(result: ConvertResult): void {
  lastWidth.value = result.width
  lastHeight.value = result.height
  lastCopPerLine.value = result.copperChanges || 0
  lastPlaneBytes.value = result.planeBytes || 0
  lastCopperBytes.value = result.copperBytes || 0
  lastChangesPerLine.value = result.changesPerLine || 0
  lastMaxMovesPerLine.value = result.maxMovesPerLine || 0
  lastAga.value = Boolean(result.aga)
  imageHasAlpha.value = Boolean(result.hasTransparency)
  maybeSeedSizes(result)
}

function trackConvertSuccess(_result: ConvertResult, convertMs: number): void {
  track('convert', {
    mode: options.mode, chipset: options.chipset, dither: options.dither,
    depth: options.depth, copper: options.copper,
    ditherStrength: options.ditherStrength, gamma: options.gamma,
    brightness: options.brightness, contrast: options.contrast,
    saturation: options.saturation, convertMs: Math.round(convertMs),
  })
  if (!firstConvertTracked) {
    firstConvertTracked = true
    track('first-convert-time', { seconds: Math.round((Date.now() - pageLoadTime) / 1000) })
  }
}

async function paintAndCacheResult(result: ConvertResult): Promise<boolean> {
  const painted = paintPreviewCanvas(result)
  if (!painted || !result.rgba) return false
  const { dw, dh } = painted
  // Cache source for CRT re-render on toggle without re-encoding.
  lastRgba = new Uint8Array(result.rgba)
  lastSrc = { w: result.width, h: result.height }
  lastDst = { w: dw, h: dh }
  if (crtEnabled.value) {
    const r = await ensureCrtRenderer()
    if (r) renderCrt()
  }
  return true
}

async function runConvert(srcBytes: Uint8Array): Promise<void> {
  errorMsg.value = ''
  const convertStart = performance.now()
  // Spinner only if conversion is slower than 100 ms.
  if (spinnerTimer) clearTimeout(spinnerTimer)
  spinnerTimer = setTimeout(() => { converting.value = true }, 100)
  progress.value = 0
  progressStage.value = ''
  try {
    const onProgress = (p: number, stage: string) => {
      progress.value = Math.round(p * 100)
      progressStage.value = stage || ''
    }
    const result = await convertRGBA(srcBytes, buildWasmOptions(), onProgress)
    clearTimeout(spinnerTimer)
    progress.value = 0
    progressStage.value = ''
    if (result.error) {
      errorMsg.value = result.error
      track('error', { type: 'convert', message: result.error, mode: options.mode })
      return
    }
    if (!await paintAndCacheResult(result)) return
    updateLastResultRefs(result)
    resultInfo.value = formatResultInfo(result)
    trackConvertSuccess(result, performance.now() - convertStart)
  } catch (error) {
    clearTimeout(spinnerTimer)
    const message = errorMessage(error)
    errorMsg.value = message
    track('error', { type: 'convert-exception', message })
  } finally {
    converting.value = false
  }
}

function doConvert() {
  const bytes = imageBytes.value
  if (!bytes || wasmLoading.value) return
  if (debounceTimer) clearTimeout(debounceTimer)
  debounceTimer = setTimeout(() => { void runConvert(bytes) }, 150)
}

watch([imageBytes, () => ({ ...options })], doConvert, { deep: true })

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error)
}

// payload widening: Uint8Array<ArrayBufferLike> isn't accepted by BlobPart in
// TS 5.7+ due to typed-array buffer variance, but Blob() at runtime accepts
// any ArrayBufferView. Cast at the boundary. Also accepts a pre-built Blob
// (e.g. from fetch().blob()).
type BlobPayload = Uint8Array | ArrayBuffer | string | Blob
function downloadBlob(payload: BlobPayload, filename: string, mime: string) {
  const blob = payload instanceof Blob ? payload : new Blob([payload as BlobPart], { type: mime })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = filename
  a.click()
  URL.revokeObjectURL(url)
}

function baseStem(): string {
  return (imageName.value || 'image').replace(/\.[^.]+$/, '')
}

async function downloadPNG() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertPNG(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    downloadBlob(result.data, baseStem() + '-amiga.png', 'image/png')
    exportCount++
    track('export', { format: 'png', mode: options.mode, exportCount, secsSinceUpload: uploadTimestamp.value ? Math.round((Date.now() - uploadTimestamp.value) / 1000) : undefined })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

async function downloadDegas() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertDegas(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    let ext = '.pi1'
    if (options.mode.endsWith('-hi')) ext = '.pi3'
    else if (options.mode.endsWith('-med')) ext = '.pi2'
    downloadBlob(result.data, baseStem() + ext, 'application/octet-stream')
    exportCount++
    track('export', { format: ext, mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

async function downloadIFF() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertIFF(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    downloadBlob(result.data, baseStem() + '.iff', 'application/octet-stream')
    exportCount++
    track('export', { format: 'iff', mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

async function downloadViewer() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const stem = options.symbolName ||
      (imageName.value || 'image').replace(/\.[^.]+$/, '').replaceAll(/\W/g, '_')
    const opts = buildWasmOptions()
    opts.symbolName = stem
    const result = await convertViewer(imageBytes.value, opts)
    if (result.error) { errorMsg.value = result.error; return }
    const payload: BlobPayload | undefined = result.header ?? result.data
    if (!payload) return
    downloadBlob(payload, stem + '.cpp', 'text/plain')
    exportCount++
    track('export', { format: 'cpp', mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

async function downloadRaw() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertRaw(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    downloadBlob(result.data, baseStem() + '.raw', 'application/octet-stream')
    exportCount++
    track('export', { format: 'raw', mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

async function downloadMaskPNG() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const opts = buildWasmOptions()
    opts.maskInvert = options.maskInvert
    const result = await convertMask(imageBytes.value, opts)
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    downloadBlob(result.data, baseStem() + '-mask.png', 'image/png')
    exportCount++
    track('export', { format: 'mask-png', mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

async function downloadMaskRaw() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const opts = buildWasmOptions()
    opts.maskInvert = options.maskInvert
    const result = await convertMaskRaw(imageBytes.value, opts)
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    downloadBlob(result.data, baseStem() + '-mask.raw', 'application/octet-stream')
    exportCount++
    track('export', { format: 'mask-raw', mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

async function postCompile(format: string, source: string): Promise<Blob | null> {
  const resp = await fetch(`/api/compile?format=${format}`, {
    method: 'POST',
    headers: { 'Content-Type': 'text/plain' },
    body: source,
  })
  if (!resp.ok) {
    errorMsg.value = await resp.text()
    return null
  }
  return resp.blob()
}

function viewerSourceFromResult(result: ConvertResult): string {
  return result.header || (result.data ? new TextDecoder().decode(result.data) : '')
}

async function compileAndDownload(format: string) {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const stem = options.symbolName ||
      (imageName.value || 'image').replace(/\.[^.]+$/, '').replaceAll(/\W/g, '_')
    const opts = buildWasmOptions()
    opts.symbolName = stem
    const result = await convertViewer(imageBytes.value, opts)
    if (result.error) { errorMsg.value = result.error; return }
    const blob = await postCompile(format, viewerSourceFromResult(result))
    if (!blob) return
    downloadBlob(blob, baseStem() + '.' + format, 'application/octet-stream')
    exportCount++
    track('export', { format, mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

function resetOptions() {
  Object.assign(options, defaultOptions())
  clearPalette()
  track('reset')
}

const GPL_RGB = /^\s*(\d+)\s+(\d+)\s+(\d+)/
function parseGimpPalette(text: string): string[] {
  const colors: string[] = []
  for (const line of text.split('\n')) {
    const m = GPL_RGB.exec(line)
    if (m) colors.push(`rgb(${m[1]},${m[2]},${m[3]})`)
  }
  return colors
}

function readBe32(bytes: Uint8Array, pos: number): number {
  return ((bytes[pos] ?? 0) << 24) | ((bytes[pos+1] ?? 0) << 16) | ((bytes[pos+2] ?? 0) << 8) | (bytes[pos+3] ?? 0)
}

function readCmapColors(bytes: Uint8Array, pos: number, size: number): string[] {
  const colors: string[] = []
  for (let i = 0; i + 2 < size && pos + i + 2 <= bytes.length; i += 3) {
    colors.push(`rgb(${bytes[pos+i] ?? 0},${bytes[pos+i+1] ?? 0},${bytes[pos+i+2] ?? 0})`)
  }
  return colors
}

function parseIffCmap(bytes: Uint8Array): string[] {
  let pos = 12
  while (pos + 8 <= bytes.length) {
    const id = String.fromCodePoint(...bytes.slice(pos, pos + 4))
    const size = readBe32(bytes, pos + 4)
    pos += 8
    if (id === 'CMAP') return readCmapColors(bytes, pos, size)
    pos += size + (size & 1)  // chunks are word-aligned
  }
  return []
}

const HEX_LINE = /^#?([0-9a-fA-F]{6})$/
function parseHexPalette(text: string): string[] {
  const colors: string[] = []
  for (const line of text.split('\n')) {
    const m = HEX_LINE.exec(line.trim())
    if (m) colors.push(`#${m[1]}`)
  }
  return colors
}

function parseBinaryPalette(bytes: Uint8Array): string[] {
  // Binary .pal: 2 bytes per color, big-endian 0x0RGB.
  const colors: string[] = []
  for (let i = 0; i + 1 < bytes.length; i += 2) {
    const w = ((bytes[i] ?? 0) << 8) | (bytes[i + 1] ?? 0)
    const r = ((w >> 8) & 0xF) * 17
    const g = ((w >> 4) & 0xF) * 17
    const b = (w & 0xF) * 17
    colors.push(`rgb(${r},${g},${b})`)
  }
  return colors
}

function parsePaletteBytes(bytes: Uint8Array): string[] {
  const text = new TextDecoder().decode(bytes)
  let colors: string[]
  if (text.startsWith('GIMP Palette')) colors = parseGimpPalette(text)
  else if (bytes.length >= 12 && String.fromCodePoint(...bytes.slice(0, 4)) === 'FORM') colors = parseIffCmap(bytes)
  else colors = parseHexPalette(text)
  // Fall back to binary .pal if no text-format matches found anything.
  if (colors.length === 0) colors = parseBinaryPalette(bytes)
  return colors
}

function loadPalette() {
  const input = document.createElement('input')
  input.type = 'file'
  input.accept = '.gpl,.hex,.txt,.pal,.iff,.ilbm,.lbm'
  async function onFile() {
    const file = input.files?.[0]
    if (!file) return
    const buf = await file.arrayBuffer()
    const bytes = new Uint8Array(buf)
    paletteData.value = bytes
    options.copper = false  // copper not compatible with custom palette
    paletteColors.value = parsePaletteBytes(bytes)
    // Pass raw file bytes to WASM for auto-format detection
    options.paletteData = bytes
  }
  input.addEventListener('change', () => { void onFile() })
  input.click()
}

function clearPalette() {
  paletteData.value = null
  paletteColors.value = []
  options.paletteData = null
}

function dismissHint() {
  showUploadHint.value = false
}

async function loadExample(example: typeof EXAMPLES[number]) {
  dismissHint()
  track('example', { name: example.name })
  // Reset to defaults, then apply example-specific settings.
  // nextTick between the two so watchers actually observe the
  // intermediate "all defaults" state — without it Vue batches both
  // Object.assigns into a single flush and watchers compare end vs
  // initial. For SCAP that means scap stays true→true (no fire,
  // copper never auto-enables) while the copper watcher DOES fire
  // (true→false) and cascades scap off. Net effect: clicking the
  // SCAP example a second time toggles copper off.
  Object.assign(options, defaultOptions())
  await nextTick()
  if (example.opts) Object.assign(options, example.opts)
  const resp = await fetch(`/examples/${example.file}`)
  const buf = await resp.arrayBuffer()
  imageBytes.value = new Uint8Array(buf)
  imageName.value = example.file
  const type = example.file.endsWith('.jpg') || example.file.endsWith('.jpeg') ? 'image/jpeg' : 'image/png'
  const blob = new Blob([buf], { type })
  imageUrl.value = URL.createObjectURL(blob)
}
</script>

<template>
  <div>
    <!-- Loading -->
    <div v-if="wasmLoading" class="flex align-items-center justify-content-center py-8 gap-3">
      <ProgressSpinner style="width: 2rem; height: 2rem" />
      <span class="text-color-secondary">Loading converter...</span>
    </div>
    <div v-else-if="wasmError" class="text-center py-8 text-red-400">{{ wasmError }}</div>

    <!-- Main layout -->
    <div v-else class="grid">
      <!-- Controls sidebar -->
      <div class="col-12 md:col-4 lg:col-3">
        <div class="flex flex-column gap-3">

          <!-- Upload / Image -->
          <Panel header="Image">
            <div
              class="drop-zone border-2 border-round-lg cursor-pointer overflow-hidden"
              :class="{
                'border-primary': dragOver,
                'border-dashed p-4 text-center': !imageUrl,
                'border-transparent': imageUrl && !dragOver,
              }"
              @drop="onDrop($event); dismissHint()"
              @dragover="onDragOver"
              @dragleave="onDragLeave"
              @click="openPicker(); dismissHint()"
            >
              <template v-if="imageUrl">
                <div class="relative">
                  <img :src="imageUrl" class="original-preview w-full border-round" />
                  <div v-if="showUploadHint" class="upload-hint"
                    @click.stop="dismissHint(); openPicker()"
                    @drop.prevent="onDrop($event); dismissHint()"
                    @dragover.prevent="onDragOver($event)"
                    @dragleave="onDragLeave"
                  >
                    <i class="pi pi-images mb-2" style="font-size: 1.5rem"></i>
                    <div class="font-semibold text-sm">Drop or click to load your own image</div>
                    <div class="text-xs mt-1" style="opacity: 0.7">Or pick an example below</div>
                    <div class="text-xs mt-2" style="opacity: 0.5">CLI tool with more features on <a href="https://github.com/tinic/png2amiga" target="_blank" style="color: inherit;">GitHub</a></div>
                  </div>
                </div>
                <div class="text-xs text-color-secondary mt-2 px-1 flex justify-content-between overflow-hidden">
                  <span class="overflow-hidden text-overflow-ellipsis" style="min-width: 0; display: block;">{{ imageName }}<template v-if="imageWidth"> ({{ imageWidth }}&times;{{ imageHeight }}, {{ (imageWidth / imageHeight).toFixed(2) }}:1)</template></span>
                  <span class="white-space-nowrap ml-2 cursor-pointer flex-shrink-0" @click.stop="openPicker(); dismissHint()">Change</span>
                </div>
              </template>
              <template v-else>
                <i class="pi pi-image text-4xl text-color-secondary mb-2"></i>
                <div class="font-semibold text-sm">Drop image here</div>
                <div class="text-xs text-color-secondary">or click to browse</div>
              </template>
            </div>
            <div class="mt-3">
              <label class="block text-xs text-color-secondary font-semibold mb-2">Examples</label>
              <div class="flex flex-wrap gap-1">
                <div
                  v-for="ex in EXAMPLES" :key="ex.name"
                  class="example-thumb cursor-pointer border-round overflow-hidden"
                  :class="{ 'ring-1 ring-primary': imageName === ex.file }"
                  @click="loadExample(ex)"
                  :title="ex.name"
                >
                  <img :src="`/examples/${ex.file}`" :alt="ex.name" />
                </div>
              </div>
            </div>
          </Panel>

          <!-- Output Settings -->
          <Panel header="Output">
            <div class="flex flex-column gap-2">
              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="OCS: Original Chip Set (12-bit). AGA: Advanced Graphics Architecture (24-bit).">Chipset</label>
                <div class="col-8">
                  <Select v-model="options.chipset" :options="CHIPSETS" optionValue="value" optionLabel="label" class="w-full" />
                </div>
              </div>

              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Amiga graphics mode. Lores: 320px, Hires: 640px, HAM: Hold-And-Modify, EHB: Extra Half-Brite.">Mode<i v-if="paletteMismatchMode" class="pi pi-exclamation-triangle ml-1" style="color:#f59e0b;font-size:0.7rem" :title="paletteMismatchMode" /></label>
                <div class="col-8">
                  <Select v-model="options.mode" :options="availableModes" optionValue="value" optionLabel="label" class="w-full" />
                </div>
              </div>

              <div v-if="showDepthSlider" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Number of bitplanes (1-8). More planes = more colors but more chip RAM.">Depth<i v-if="paletteMismatchDepth" class="pi pi-exclamation-triangle ml-1" style="color:#f59e0b;font-size:0.7rem" :title="paletteMismatchDepth" /></label>
                <div class="col-5">
                  <Slider v-model="options.depth" :min="1" :max="depthMax || 6" :step="1" class="w-full" />
                </div>
                <div class="col-3">
                  <InputNumber v-model="options.depth" :min="1" :max="depthMax || 6" :step="1" class="w-full input-sm" />
                </div>
              </div>

              <!-- CGA text mode only: per-cell error metric. When blur is
                   selected the dither selector below is hidden, since blur
                   needs the continuous source. -->
              <div v-if="options.mode === 'cga-text80x100'" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold"
                  title="Per-cell error metric for the glyph + (fg, bg) brute-force search. Pappas-Neuhoff (perceptual blur) needs the continuous source and hides the dither selector. MSE pairs with pixel-level dither below.">
                  Metric
                </label>
                <div class="col-8">
                  <Select v-model="options.cgaTextMetric" :options="CGA_TEXT_METRICS"
                    optionLabel="label" optionValue="value" class="w-full" />
                </div>
              </div>

              <div v-if="!(options.mode === 'cga-text80x100' && options.cgaTextMetric !== 'mse')"
                class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Dithering algorithm. Ordered methods use fixed patterns; error diffusion propagates quantization error to neighbors.">Dither</label>
                <div class="col-8 flex gap-1 align-items-center">
                  <Select
                    v-model="options.dither"
                    :options="groupedDitherOptions"
                    optionValue="value"
                    optionLabel="label"
                    optionGroupLabel="label"
                    optionGroupChildren="items"
                    class="flex-1"
                    style="min-width:0"
                  />
                  <div class="flex flex-column" style="gap:1px">
                    <Button icon="pi pi-chevron-up" severity="secondary" text size="small"
                      @click="cycleDither(-1)" style="min-width:0;width:1.2rem;height:0.85rem;padding:0" />
                    <Button icon="pi pi-chevron-down" severity="secondary" text size="small"
                      @click="cycleDither(1)" style="min-width:0;width:1.2rem;height:0.85rem;padding:0" />
                  </div>
                </div>
              </div>

              <!-- CAP — Copper-Augmented Palette (Amiga only; not Atari/DOS) -->
              <div v-if="!isAtariMode(options.mode) && !isDosMode(options.mode) && !paletteData" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="CAP — Copper-Augmented Palette: per-scanline palette swaps via the Copper coprocessor, picked greedily by OKLab error reduction. Each row gets its own per-line variant of the base palette. Composes with --dpf (palette evolves across the upper PF2 register bank).">CAP</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.copper" />
                  <span style="color: #888; font-size: 0.625rem;">Copper-Augmented Palette</span>
                </div>
              </div>
              <!-- CAP best quality (HAM modes only — flag is a no-op elsewhere) -->
              <div v-if="options.copper && showHamControls" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Slower (~4-5x) CAP planner: multi-candidate slot search + joint base-palette refinement. +0.5..2 dB PSNR on HAM6/HAM8 + CAP. Off by default.">CAP best</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.capBest" />
                  <span style="color: #888; font-size: 0.625rem;">~4-5× slower</span>
                </div>
              </div>

              <!-- Dual playfield (standard Amiga lores/hires + matching depth). -->
              <div v-if="dpfAvailable" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Dual playfield: encode the image into PF2 (upper color registers 8-15 OCS / 16-31 AGA), with PF1 (foreground) bitplanes left zeroed. Requires depth = 3 (OCS) or 4 (AGA). CAMG DBLPF flag set.">Dual playfield</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.dualPlayfield" />
                  <span style="color: #888; font-size: 0.625rem;">PF2 only, upper color regs</span>
                </div>
              </div>

              <!-- SCAP — mid-line palette swaps (OCS lores only, DPF or EHB) -->
              <div v-if="scapAvailable" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="SCAP — Super CAP: mid-line palette swaps inside the displayed area, on top of CAP's per-line evolution. 19 MOVEs per scanline at 16-lores-px stride; slot HPOS table calibrated against real OCS hardware. Two flavours: DPF (3-plane PF2, 8 base colours) and EHB (32 base + 32 hardware-derived half-brites).">SCAP</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.scap" />
                  <span style="color: #888; font-size: 0.625rem;">mid-line palette swaps (19/line)</span>
                </div>
              </div>

              <!-- Native PAR (DOS modes only): preserve source aspect inside the fixed
                   hardware buffer by letterboxing/pillarboxing with black padding. -->
              <div v-if="isDosMode(options.mode)" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Preserve source aspect ratio on DOS hardware by letterboxing (reduce height) or pillarboxing (reduce width) inside the fixed frame. Off = stretch to fill the full buffer.">Native PAR</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.nativePar" />
                </div>
              </div>

              <!-- Resize override (not for Atari or DOS — fixed hardware buffer) -->
              <div v-if="!isAtariMode(options.mode) && !isDosMode(options.mode)" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold">Resize</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="sizeOverride" />
                  <template v-if="sizeOverride">
                    <Button size="small" severity="secondary" text class="size-preset"
                            :disabled="!imageBytes" @click="setSizePreset(1)"
                            v-tooltip.top="'Same as source'">100%</Button>
                    <Button size="small" severity="secondary" text class="size-preset"
                            :disabled="!imageBytes" @click="setSizePreset(0.5)"
                            v-tooltip.top="'Half of source'">50%</Button>
                    <Button size="small" severity="secondary" text class="size-preset"
                            :disabled="!imageBytes" @click="setSizePreset(0.25)"
                            v-tooltip.top="'Quarter of source'">25%</Button>
                  </template>
                </div>
              </div>
              <template v-if="sizeOverride && !isAtariMode(options.mode)">
                <div class="resize-fields" :class="{ unlinked: !aspectLocked }">
                  <label class="resize-label text-xs text-color-secondary font-semibold" title="Output width in pixels. Must be multiple of 16.">Width</label>
                  <div class="resize-input">
                    <InputNumber v-model="options.width" :min="16" :step="16"
                                 @blur="() => onWidthCommit()"
                                 @keyup.enter="() => onWidthCommit()"
                                 class="w-full input-sm" />
                  </div>
                  <div class="resize-bracket top"></div>
                  <label class="resize-label text-xs text-color-secondary font-semibold" title="Output height in pixels.">Height</label>
                  <div class="resize-input">
                    <InputNumber v-model="options.height" :min="2" :step="2"
                                 @blur="() => onHeightCommit()"
                                 @keyup.enter="() => onHeightCommit()"
                                 class="w-full input-sm" />
                  </div>
                  <div class="resize-bracket bot"></div>
                  <Button class="aspect-lock"
                          severity="secondary" text
                          :icon="aspectLocked ? 'pi pi-lock' : 'pi pi-lock-open'"
                          v-tooltip.top="aspectLocked ? 'Aspect ratio locked — click to unlock' : 'Aspect ratio unlocked — click to lock'"
                          @click="aspectLocked = !aspectLocked" />
                </div>
              </template>
            </div>
          </Panel>

          <!-- Alpha (only shown when source has transparency) -->
          <Panel v-if="imageHasAlpha" header="Alpha">
            <div class="flex flex-column gap-2">
              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Shift the alpha cutoff. 0 = standard 50% threshold. Negative = more opaque pixels pass, positive = fewer.">Threshold</label>
                <div class="col-5">
                  <Slider v-model="options.alphaThreshold" :min="-0.5" :max="0.5" :step="0.05" class="w-full" />
                </div>
                <div class="col-3">
                  <InputNumber v-model="options.alphaThreshold" :min="-0.5" :max="0.5" :step="0.05"
                    :minFractionDigits="2" :maxFractionDigits="2" class="w-full input-sm" />
                </div>
              </div>

              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Alpha dithering method. Hard threshold = binary cutoff. Others dither alpha to 1-bit using the selected pattern.">Dither</label>
                <div class="col-8">
                  <Select v-model="options.alphaDither" :options="ALPHA_DITHER_METHODS" optionValue="value" optionLabel="label" class="w-full" />
                </div>
              </div>

              <div v-if="options.alphaDither !== 'none'" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Alpha dither strength. Controls how aggressively alpha is dithered.">Strength</label>
                <div class="col-5">
                  <Slider v-model="options.alphaDitherStrength" :min="0" :max="3.0" :step="0.05" class="w-full" />
                </div>
                <div class="col-3">
                  <InputNumber v-model="options.alphaDitherStrength" :min="0" :max="3.0" :step="0.05"
                    :minFractionDigits="2" :maxFractionDigits="2" class="w-full input-sm" />
                </div>
              </div>
            </div>
          </Panel>

          <!-- Adjustments -->
          <Panel header="Adjustments">
            <div class="flex flex-column gap-2">
              <div v-for="s in SLIDERS" :key="s.key" class="grid align-items-center">
                <label class="col-3 text-xs text-color-secondary font-semibold white-space-nowrap" :title="s.tip">{{ s.label }}</label>
                <div class="col-6">
                  <Slider v-model="options[s.key]" :min="s.min" :max="s.max" :step="s.step" class="w-full" />
                </div>
                <div class="col-3">
                  <InputNumber v-model="options[s.key]" :min="s.min" :max="s.max" :step="s.step"
                    :minFractionDigits="2" :maxFractionDigits="2" class="w-full input-sm" />
                </div>
              </div>

              <template v-if="isErrorDiffusion(options.dither)">
                <div v-for="s in DIFFUSION_SLIDERS" :key="s.key" class="grid align-items-center">
                  <label class="col-3 text-xs text-color-secondary font-semibold white-space-nowrap" :title="s.tip">{{ s.label }}</label>
                  <div class="col-6">
                    <Slider v-model="options[s.key]" :min="s.min" :max="s.max" :step="s.step" class="w-full" />
                  </div>
                  <div class="col-3">
                    <InputNumber v-model="options[s.key]" :min="s.min" :max="s.max" :step="s.step"
                      :minFractionDigits="2" :maxFractionDigits="2" class="w-full input-sm" />
                  </div>
                </div>
              </template>

            </div>
          </Panel>

          <!-- Export Actions -->
          <div class="flex flex-column gap-2">
            <!-- Atari export buttons -->
            <div v-if="isAtariMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button :label="options.mode.endsWith('-hi') ? 'pi3' : options.mode.endsWith('-med') ? 'pi2' : 'pi1'" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadDegas"
                title="Download as Degas Elite file for Atari ST/STE." />
            </div>
            <!-- IBM PC DOS export buttons: PNG preview + raw + DJGPP cpp viewer. -->
            <div v-if="isDosMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="raw" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadRaw"
                :title="isEgaMode(options.mode)
                  ? 'Download raw EGA planar data: 4 bitplanes + 16-byte IrgbIRGB palette (DMA-ready for 0xA0000).'
                  : isVgaMode(options.mode)
                  ? 'Download raw VGA planar data: 4 bitplanes + 16×3-byte 6-bit DAC palette (feed to 0x3C9).'
                  : 'Download raw CGA banked planar data (cga-320: 4-color 2bpp; cga-640: 2-color 1bpp).'" />
              <Button label="cpp" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadViewer"
                title="Download a DJGPP-compilable .cpp viewer (sets video mode, loads palette, blits image, waits for key, restores text mode). Build: i586-pc-msdosdjgpp-g++ -O2 -o out.exe out.cpp" />
            </div>
            <!-- Amiga export buttons -->
            <div v-if="!isAtariMode(options.mode) && !isDosMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="iff" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadIFF"
                :title="options.copper
                  ? 'Download as IFF ILBM with standard PCHG chunk for per-line palette (readable by Recoil, ViewTek, OS3.5+ ilbm.datatype).'
                  : 'Download as IFF ILBM (Deluxe Paint, Personal Paint, WinUAE compatible).'" />
              <Button label="adf" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="compileAndDownload('adf')"
                title="Download bootable Amiga floppy disk image (ADF)." />
            </div>
            <div v-if="!isAtariMode(options.mode) && !isDosMode(options.mode)" class="flex gap-2">
              <Button label="exe" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="compileAndDownload('exe')"
                title="Download compiled AmigaOS executable. Click left mouse button to exit." />
              <Button label="cpp" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadViewer"
                title="Download standalone AmigaOS viewer source (.cpp) — compile with m68k-amiga-elf-gcc." />
              <Button label="raw" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadRaw"
                v-tooltip.top="{ value: rawTooltipHtml, escape: false }" />
            </div>
            <Button label="Reset" icon="pi pi-refresh" severity="secondary" outlined class="w-full" @click="resetOptions"
              title="Reset all parameters to defaults." />

            <!-- Advanced section -->
            <Panel header="Advanced" toggleable collapsed class="mt-2">
              <!-- Custom palette -->
              <div v-if="!isHamMode(options.mode)">
                <label class="block text-xs text-color-secondary font-semibold mb-1">Custom Palette</label>
                <div class="flex gap-2 align-items-center">
                  <Button label="Load" icon="pi pi-upload" size="small" severity="secondary" @click="loadPalette" :disabled="converting" />
                  <Button v-if="paletteData" label="Clear" icon="pi pi-times" size="small" severity="secondary" text @click="clearPalette" />
                  <span v-if="paletteData" class="text-xs text-color-secondary">{{ paletteColors.length }} colors</span>
                </div>
                <div v-if="paletteColors.length" class="flex flex-wrap gap-1 mt-1">
                  <div v-for="(c, i) in paletteColors" :key="i"
                    :style="{ background: c, width: '12px', height: '12px', borderRadius: '2px' }"
                    :title="`#${i}: ${c}`" />
                </div>
                <div v-if="isEhbMode(options.mode) && paletteColors.length && paletteColors.length !== 32"
                  class="text-xs text-red-400 mt-1">EHB requires exactly 32 colors</div>
              </div>

              <!-- Crop -->
              <div class="pt-3 mt-3 border-top-1 surface-border">
                <label class="block text-xs text-color-secondary font-semibold mb-1">Crop</label>
                <div class="flex gap-2 align-items-center mb-1">
                  <label class="text-xs" style="width:1.5rem">X</label>
                  <InputNumber v-model="options.cropX" :min="0" :max="9999" class="flex-1 input-sm" :disabled="options.cropAuto" />
                  <label class="text-xs" style="width:1.5rem">Y</label>
                  <InputNumber v-model="options.cropY" :min="0" :max="9999" class="flex-1 input-sm" :disabled="options.cropAuto" />
                </div>
                <div class="flex gap-2 align-items-center mb-1">
                  <label class="text-xs" style="width:1.5rem">W</label>
                  <InputNumber v-model="options.cropW" :min="0" :max="9999" class="flex-1 input-sm" :disabled="options.cropAuto" placeholder="0 = full" />
                  <label class="text-xs" style="width:1.5rem">H</label>
                  <InputNumber v-model="options.cropH" :min="0" :max="9999" class="flex-1 input-sm" :disabled="options.cropAuto" placeholder="0 = full" />
                </div>
                <div class="flex align-items-center gap-2">
                  <input type="checkbox" v-model="options.cropAuto" id="cropAuto" />
                  <label for="cropAuto" class="text-xs text-color-secondary">Auto-crop to mode aspect ratio</label>
                </div>
              </div>

              <!-- Reserve color 0 -->
              <div class="pt-3 mt-3 border-top-1 surface-border">
                <div class="flex align-items-center gap-2">
                  <input type="checkbox" v-model="options.reserveColor0" id="reserveColor0" />
                  <label for="reserveColor0" class="text-xs text-color-secondary" title="Reserve palette index 0 for black (Amiga border/background color). Disable to use all palette slots for image colors.">Reserve color 0 for black</label>
                </div>
              </div>

              <!-- HAM beam width (DP search) — advanced: quality plateaus
                   quickly past ~8, default 16 is fine for almost any image. -->
              <template v-if="showHamControls">
                <div class="pt-3 mt-3 border-top-1 surface-border">
                  <label class="block text-xs text-color-secondary font-semibold mb-1" title="Beam width for DP search. Higher = marginally better quality, slower. Range 1-256 (default 16). In practice quality plateaus past ~8.">HAM Beam Width</label>
                  <div class="flex gap-2 align-items-center">
                    <Slider v-model="options.hamBeam" :min="1" :max="256" :step="1" class="flex-1" />
                    <InputNumber v-model="options.hamBeam" :min="1" :max="256" :step="1" class="input-sm" style="width: 4rem" />
                  </div>
                </div>
              </template>

              <!-- Palette diversity (experimental) -->
              <div class="pt-3 mt-3 border-top-1 surface-border">
                <label class="block text-xs text-color-secondary font-semibold mb-3" title="Experimental. Removes near-duplicate palette entries and re-seeds them from poorly-served image regions. 0 = off, 1 = conservative, 9 = aggressive. Results plateau around 5.">
                  Palette Diversity
                  <span class="text-color-secondary font-normal">({{ options.paletteDiversity }})</span>
                </label>
                <Slider v-model="options.paletteDiversity" :min="0" :max="9" :step="1" class="w-full" />
              </div>

              <!-- CAP changes override -->
              <div v-if="options.copper" class="pt-3 mt-3 border-top-1 surface-border">
                <label class="block text-xs text-color-secondary font-semibold mb-1" title="Copper-Augmented Palette swaps per scanline. 0 = auto: backend picks the worst-case K that fits the 14-MOVE budget, plus a K+3 retry path. Higher values bypass the budget check and may overshoot real hardware.">CAP Changes/Line</label>
                <div class="flex gap-2 align-items-center">
                  <InputNumber v-model="options.copperChanges" :min="0" :max="copperMax" class="flex-1 input-sm" placeholder="0 = auto" />
                  <span class="text-xs text-color-secondary">max: {{ copperMax }}</span>
                </div>
              </div>

              <!-- Mask export (only when source has transparency) -->
              <div v-if="imageHasAlpha" class="pt-3 mt-3 border-top-1 surface-border">
                <label class="block text-xs text-color-secondary font-semibold mb-1">Mask Export</label>
                <div class="flex align-items-center gap-2 mb-2">
                  <input type="checkbox" v-model="options.maskInvert" id="maskInvert" />
                  <label for="maskInvert" class="text-xs text-color-secondary" title="Invert mask polarity. Default: white=opaque, black=transparent. Inverted: white=transparent, black=opaque.">Invert (1=transparent)</label>
                </div>
                <div class="flex gap-2">
                  <Button label="mask png" icon="pi pi-download" size="small" class="flex-1" :disabled="!imageBytes || converting" @click="downloadMaskPNG"
                    title="Download 1-bit transparency mask as PNG (black & white)." />
                  <Button label="mask raw" icon="pi pi-download" size="small" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadMaskRaw"
                    title="Download raw 1-bitplane mask data (word-aligned, no header)." />
                </div>
              </div>
            </Panel>
          </div>

        </div>
      </div>

      <!-- Preview (sticky right side) -->
      <div ref="previewColRef" class="col-12 md:col-8 lg:col-9 preview-col" :class="{ 'loupe-expanded': loupeActive }" :style="loupeHeight ? { height: loupeHeight } : {}">
        <div v-if="!imageBytes" class="surface-card border-round-lg flex align-items-center justify-content-center" style="min-height: 500px;">
          <div class="text-center text-color-secondary">
            <i class="pi pi-upload text-5xl mb-3 block"></i>
            <div>Upload an image to get started</div>
          </div>
        </div>

        <div v-else class="flex flex-column gap-2">
          <div class="preview-container surface-card border-round-lg overflow-auto relative"
               @pointerdown="loupePointerDown" @pointermove="loupePointerMove" @pointerup="loupePointerUp"
               :class="{ 'loupe-active': loupeActive }"
          >
            <div class="canvas-wrap relative" :style="loupeActive ? { transform: `scale(4) translate(${loupeX/4}px, ${loupeY/4}px)`, transformOrigin: '0 0' } : {}">
              <canvas ref="canvasRef" class="preview-canvas" v-show="!crtEnabled" />
              <canvas ref="crtCanvasRef" class="preview-canvas" v-show="crtEnabled" />
              <div v-if="converting" class="overlay flex flex-column align-items-center justify-content-center" style="gap: 0.5rem">
                <ProgressSpinner v-if="!progress" style="width: 2rem; height: 2rem" />
                <div v-else style="width: 70%; max-width: 22rem; text-align: center;">
                  <ProgressBar :value="progress" :show-value="true" style="height: 1.5rem" />
                  <div class="text-xs text-color-secondary mt-1" v-if="progressStage">{{ progressStage }}</div>
                </div>
              </div>
            </div>
            <button class="loupe-btn" :class="{ active: loupeActive }" @click.stop="loupeToggle" title="Toggle 4x zoom">
              <i class="pi pi-search"></i>
            </button>
            <button class="loupe-btn crt-btn" :class="{ active: crtEnabled }" @click.stop="crtEnabled = !crtEnabled" title="CRT preview — Commodore 1084S RGB monitor simulation (slot mask, scanlines, bloom)">
              <i class="pi pi-desktop"></i>
            </button>
          </div>
          <div class="flex justify-content-between align-items-center px-1">
            <span class="text-xs text-color-secondary">{{ resultInfo }}</span>
            <div class="flex align-items-center gap-2">
              <span v-if="errorMsg" class="text-xs text-red-400">{{ errorMsg }}</span>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style>
/* PrimeVue tooltip: allow wide content for raw format table */
.p-tooltip {
  max-width: none !important;
}
/* Global: PrimeVue teleports overlays to body, outside scoped styles */
.p-select-overlay .p-select-option,
.p-select-overlay .p-select-option-group {
  font-size: 0.75rem;
}
.p-select-overlay .p-select-option-group {
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  opacity: 0.5;
  padding-top: 0.75rem;
}
</style>

<style scoped>
:deep(.p-select),
:deep(.p-select-label) {
  font-size: 0.75rem;
}

:deep(.input-sm) {
  max-width: 100%;
}
:deep(.input-sm .p-inputnumber-input) {
  font-size: 0.75rem;
  padding: 0.25rem 0.3rem;
  width: 100%;
  min-width: 0;
}

/* Compact resize-preset buttons (100% / 50% / 25%) */
:deep(.size-preset) {
  font-size: 0.65rem;
  padding: 0.15rem 0.35rem !important;
  min-width: 0 !important;
}

/* Resize fields: 3-column grid with chain-lock button spanning both rows.
 * Bracket lines connect the lock button to both the Width and Height
 * inputs so users can tell at a glance which fields the lock affects.
 *
 *   Width  [320] ┐
 *                 ├─ 🔗
 *   Height [213] ┘
 */
.resize-fields {
  display: grid;
  grid-template-columns: 33.33% 1fr 0.5rem auto;
  grid-template-rows: auto auto;
  column-gap: 0.5rem;
  row-gap: 0.6rem;
  align-items: center;
}
.resize-fields .resize-label {
  grid-column: 1;
}
.resize-fields .resize-input {
  grid-column: 2;
}
/* Bracket arms: a thin column of border between input and lock button */
.resize-fields .resize-bracket {
  grid-column: 3;
  width: 0.5rem;
  height: 100%;
  border-color: var(--p-content-border-color, #4b5563);
  border-style: solid;
  border-width: 0;
}
.resize-fields .resize-bracket.top {
  grid-row: 1;
  border-top-width: 1px;
  border-right-width: 1px;
  border-top-right-radius: 4px;
}
.resize-fields .resize-bracket.bot {
  grid-row: 2;
  border-bottom-width: 1px;
  border-right-width: 1px;
  border-bottom-right-radius: 4px;
}
.resize-fields .aspect-lock {
  grid-column: 4;
  grid-row: 1 / span 2;
  align-self: center;
}
:deep(.aspect-lock) {
  padding: 0.4rem 0.5rem !important;
  min-width: 0 !important;
}
:deep(.aspect-lock .p-button-icon) {
  font-size: 0.85rem;
}
/* Dim the brackets when the lock is unlinked (the icon shape itself
 * already conveys the state, so no color override needed) */
.resize-fields.unlinked .resize-bracket {
  border-color: var(--p-content-border-color, #4b5563);
  opacity: 0.25;
}

.preview-col {
  position: sticky;
  top: 1rem;
  align-self: start;
}
.preview-col.loupe-expanded {
  position: sticky;
  top: 1rem;
  display: flex;
  flex-direction: column;
}
.preview-col.loupe-expanded > div {
  flex: 1;
  display: flex;
  flex-direction: column;
  min-height: 0;
}
.preview-col.loupe-expanded .preview-container {
  flex: 1;
  min-height: 0;
  overflow: hidden;
}

.drop-zone {
  transition: border-color 0.15s, background 0.15s;
  max-width: 100%;
}
.drop-zone:hover {
  border-color: var(--p-primary-color) !important;
}

:deep(.p-panel-content) {
  overflow: hidden;
}

.original-preview {
  display: block;
  max-height: 200px;
  object-fit: contain;
}

.upload-hint {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.7);
  color: #fff;
  border-radius: inherit;
  cursor: pointer;
  text-align: center;
  padding: 1rem;
}

.example-thumb {
  width: 48px;
  height: 36px;
  transition: opacity 0.15s;
}
.example-thumb:hover {
  opacity: 0.8;
}
.example-thumb img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: block;
}

.preview-container {
  display: inline-block;
  background: #000;
  padding: 1rem;
}
.preview-container.loupe-active {
  cursor: grab;
  overflow: hidden;
}
.preview-container.loupe-active:active {
  cursor: grabbing;
}

.canvas-wrap {
  display: inline-block;
}

.loupe-btn {
  position: absolute;
  top: 0.4rem;
  right: 0.4rem;
  width: 1.75rem;
  height: 1.75rem;
  border: none;
  border-radius: 4px;
  background: rgba(0, 0, 0, 0.5);
  color: rgba(255, 255, 255, 0.6);
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 0.85rem;
  transition: background 0.15s, color 0.15s;
}
.loupe-btn:hover {
  background: rgba(0, 0, 0, 0.7);
  color: #fff;
}
.loupe-btn.active {
  background: var(--p-primary-color);
  color: #fff;
}
.loupe-btn.crt-btn {
  /* Sit immediately to the left of the loupe button. */
  right: 2.4rem;
}

.preview-canvas {
  display: block;
  image-rendering: pixelated;
  image-rendering: crisp-edges;
  background-image:
    linear-gradient(45deg, #808080 25%, transparent 25%),
    linear-gradient(-45deg, #808080 25%, transparent 25%),
    linear-gradient(45deg, transparent 75%, #808080 75%),
    linear-gradient(-45deg, transparent 75%, #808080 75%);
  background-size: 16px 16px;
  background-position: 0 0, 0 8px, 8px -8px, -8px 0;
  background-color: #c0c0c0;
}

.overlay {
  position: absolute;
  inset: 0;
  background: rgba(0, 0, 0, 0.5);
}
</style>
