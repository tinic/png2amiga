<script setup lang="ts">
import { ref, reactive, watch, nextTick, computed, onBeforeUnmount, useTemplateRef } from 'vue'
import type { ConvertResult, WasmOptions } from '@wasm/png2amiga.js'
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
  CHIPSETS, DITHER_METHODS, ALPHA_DITHER_METHODS, isNonSquareDither,
  SLIDERS, CGA_TEXT_METRICS, CGA_TEXT_KERNELS, C64_PALETTES, C64_METRICS, c64PaletteRgb, EXAMPLES, examplesForChipset,
  defaultOptions, isHamMode, hamType, isEhbMode, isAtariMode,
  isDosMode, isVgaMode, isEgaMode, isSnesMode, isSnesDirectMode, isGenesisMode, isGbaMode, isGbaDirectMode, isC64Mode, isC64CharsetMode, isThomsonMode, isTedMode, isCgaMode, isCgaText, isTileFreeformMode, isFixedBufferMode, isAmigaMode, supportsCustomPalette, isInterlaceMode, modePar,
  maxDepth, defaultDepth, effectiveChipset, previewScale,
  modesForChipset,
} from '../lib/options.js'
import { track } from '../lib/analytics.js'
import { useImageUpload } from '../composables/useImageUpload.js'
import { useWasm } from '../composables/useWasm.js'

import DitherGallery from './DitherGallery.vue'

const { loading: wasmLoading, error: wasmError, abort: abortWasm, convertRGBA, convertPNG, convertIFF, convertHeader, convertViewer, convertDegas, convertRaw, convertPRG, convertKoa, convertHir, convertMask, convertMaskRaw } = useWasm()

function onStopEncode(): void {
  abortWasm()
  // Bump committedGen ABOVE every in-flight runConvert's myGen so
  // their catch/finally on the next microtask see myGen <= committedGen
  // and refuse to overwrite errorMsg / canvas / resultInfo with stale
  // data after the user already moved on. Bump convertGen too to keep
  // start-time identifiers monotonic.
  convertGen++
  committedGen = convertGen
  // Tear down EVERY in-flight encode handle. What DOES matter: clear
  // both timers so the next options change (e.g. user toggling --best
  // off) starts fresh — without this, a stale debounce timer that was
  // queued mid-stop could re-fire runConvert.
  if (debounceTimer) { clearTimeout(debounceTimer); debounceTimer = null }
  if (spinnerTimer)  { clearTimeout(spinnerTimer);  spinnerTimer = null }
  converting.value = false
  progress.value = 0
  progressStage.value = ''
  errorMsg.value = ''
  // Paint the preview canvas black so the user has a clear "aborted"
  // signal rather than the stale half-finished output. Re-encode runs
  // on the next option change.
  const canvas = canvasRef.value
  if (canvas) {
    const ctx = canvas.getContext('2d')
    if (ctx) {
      ctx.fillStyle = '#000'
      ctx.fillRect(0, 0, canvas.width || 320, canvas.height || 200)
    }
  }
  resultInfo.value = 'stopped'
}
const { imageBytes, imageName, imageUrl, imageWidth, imageHeight, dragOver, uploadTimestamp, onDrop, onDragOver, onDragLeave, openPicker } = useImageUpload()

const showUploadHint = ref(true)

// Load first example by default once WASM is ready. Use the per-chipset
// list (driven by hostname via detectDefaultChipset / defaultOptions) so
// png2c64.app boots onto a c64 image, png2amiga.app onto an Amiga one.
// Default-tuning suffices for the initial frame — user-clicked examples
// pull in per-image opts via loadExample().
watch(wasmLoading, async (loading) => {
  if (loading || wasmError.value || imageBytes.value) return
  const example = examplesForChipset(options.chipset)[0]
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
const charsetCanvasRef = useTemplateRef<HTMLCanvasElement>('charsetCanvasRef')
const genesisTilesCanvasRef = useTemplateRef<HTMLCanvasElement>('genesisTilesCanvasRef')
const snesTilesCanvasRef = useTemplateRef<HTMLCanvasElement>('snesTilesCanvasRef')
// Sliced / strips / copper-HAM modes evolve their palette per scanline.
// We render the per-row base palette as a vertical strip beside the
// preview: backing is N×H pixels (one column per slot, one row per
// scanline) and CSS-scaled to align with the preview canvas vertically.
const scanlinePaletteCanvasRef = useTemplateRef<HTMLCanvasElement>('scanlinePaletteCanvasRef')
const hasScanlinePalette = ref(false)
const crtEnabled = ref(false)
const converting = ref(false)
const progress = ref(0)         // 0..100 — encoder progress for slow paths
const progressStage = ref('')

// Lazy-init the CRT renderer the first time --crt is toggled on; persist
// across re-renders. Only torn down on unmount.
let crtRenderer: CrtRenderer | null = null
let lastRgba: Uint8Array | null = null   // cached source RGBA so a CRT toggle
let lastIndices: Uint8Array | null = null // cached per-pixel palette indices for
                                          // hover-isolate (non-HAM modes only;
                                          // null otherwise → fall back to RGB
                                          // matching).
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
  // Backing-store sizing combines two constraints:
  //   1. Match physical screen pixels (cssSize × devicePixelRatio) so
  //      the shader's mask — keyed off gl_FragCoord at a 6-pixel period
  //      per RGB triad — lines up with real phosphor pitch on the
  //      output device. Without DPR scaling, a Windows 1× monitor would
  //      get a 4× oversampled backing store that the browser then
  //      downsamples 2× for display — the mask aliases into giant RGB
  //      fringes that aren't a real CRT artifact. Retina happened to
  //      look right by coincidence (cssW × 2 ≈ srcW × 4 for lores).
  //   2. Floor at ≥4 output rows per source row so hardScan=-8's
  //      Gaussian renders scanlines as continuous curves instead of a
  //      dot grid (the mask + scanline beat at 2 rows/src looks like
  //      scattered dots). Interlace softens scanlines, so 2 suffices.
  //      Width has a 1×-source floor to guarantee the 3-tap horizontal
  //      filter can address every source pixel (don't OVERshoot it
  //      though — backing > CSS×DPR forces a browser downsample which
  //      aliases the mask back into giant fringes).
  const DPR = window.devicePixelRatio || 1
  const isInterlace = isInterlaceMode(options.mode)
  const minDw = lastSrc.w
  const minDh = lastSrc.h * (isInterlace ? 2 : 4)
  const dw = Math.max(minDw, Math.round(lastDst.w * DPR))
  const dh = Math.max(minDh, Math.round(lastDst.h * DPR))
  if (crtCanvasRef.value) {
    // Display size matches the regular preview's lastDst so the mode
    // aspect ratio (lores 2:1, hires 1:2 etc.) is preserved.
    crtCanvasRef.value.style.width  = `${lastDst.w}px`
    crtCanvasRef.value.style.height = `${lastDst.h}px`
  }
  // C64 modes simulate composite output to a PAL TV (chroma blur,
  // delay-line averaging, chromatic aberration). Amiga modes assume an
  // 1084S RGB monitor — leave PAL mode off.
  crtRenderer.setPalMode(isC64Mode(options.mode))
  // Interlace mode — drives visual tuning (softer scanlines, equalized
  // brightness) and the 60 Hz field-flicker animation. Keyed off the
  // selected mode (-lace suffix) rather than the source-height heuristic
  // we used to use, because short interlaced content (e.g. a 320×238
  // logo) won't trip a height threshold. The renderer manages its own
  // RAF loop while interlace is on; turn off for non-interlace modes
  // so the preview stays static and doesn't burn GPU cycles.
  crtRenderer.setInterlaceMode(isInterlaceMode(options.mode))
  // 1084S dot pitch is 0.42mm. CSS spec is 96 dpi = 3.78 px/mm, so one
  // triad in device pixels = 0.42 × 3.78 × DPR ≈ 1.6 (1×) / 3.2 (Retina) /
  // 4.8 (DPR=3 phones). The renderer floors at 3 (Nyquist for the cosine
  // mask), so 1× / low-DPI displays cap at "the finest phosphor pitch we
  // can render," and high-DPR displays approach the real 0.42mm pitch as
  // pixels get smaller. This is the actual constraint — phosphors finer
  // than your display can't be reproduced.
  const maskPeriod = 0.42 * (96 / 25.4) * DPR
  crtRenderer.setMaskPeriod(maskPeriod)
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
  track('loupe-toggle', { enabled: loupeActive.value })
  loupeX.value = 0
  loupeY.value = 0
  if (loupeActive.value && previewColRef.value) {
    const rect = previewColRef.value.getBoundingClientRect()
    loupeHeight.value = `${window.innerHeight - rect.top - 16}px`
  } else {
    loupeHeight.value = null
  }
}

// Palette swatch viewer toggle. Renders the rendered image's final
// palette as a grid of 8×8 swatches (≤ 64 per row). Available for any
// mode whose result includes paletteBytes.
const paletteViewActive = ref(false)
const paletteCanvasRef = useTemplateRef<HTMLCanvasElement>('paletteCanvasRef')
function paletteToggle() {
  paletteViewActive.value = !paletteViewActive.value
  track('palette-view-toggle', { enabled: paletteViewActive.value })
}
// CSS pixels per swatch (50% of the previous 32 px → 16 px on screen,
// canvas backing 8 px = 1 swatch native).
const kPaletteSwatchPx = 8
const kPaletteCssScale = 2
const kPalettePerRow = 32
// Draw the EHB halfbrite carve-out: faint outline, no color fill.
function drawPaletteCarveCell(ctx: CanvasRenderingContext2D,
                              cx: number, cy: number) {
  ctx.strokeStyle = 'rgba(255,255,255,0.18)'
  ctx.lineWidth = 1
  ctx.strokeRect(cx + 0.5, cy + 0.5,
                 kPaletteSwatchPx - 1, kPaletteSwatchPx - 1)
}

// Draw the red X with a black outline marking a reserved slot.
function drawPaletteReserveX(ctx: CanvasRenderingContext2D,
                             cx: number, cy: number) {
  const x0 = cx + 1, y0 = cy + 1
  const x1 = cx + kPaletteSwatchPx - 1
  const y1 = cy + kPaletteSwatchPx - 1
  ctx.lineCap = 'round'
  ctx.strokeStyle = '#000'
  ctx.lineWidth = 2.5
  ctx.beginPath()
  ctx.moveTo(x0, y0); ctx.lineTo(x1, y1)
  ctx.moveTo(x1, y0); ctx.lineTo(x0, y1)
  ctx.stroke()
  ctx.strokeStyle = '#e22'
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.moveTo(x0, y0); ctx.lineTo(x1, y1)
  ctx.moveTo(x1, y0); ctx.lineTo(x0, y1)
  ctx.stroke()
}

function drawPalette(bytes: Uint8Array) {
  const canvas = paletteCanvasRef.value
  if (!canvas) return
  const n = bytes.length / 3
  const cols = Math.min(kPalettePerRow, n)
  const rows = Math.ceil(n / kPalettePerRow)
  const w = cols * kPaletteSwatchPx
  const h = rows * kPaletteSwatchPx
  canvas.width = w
  canvas.height = h
  canvas.style.width = `${w * kPaletteCssScale}px`
  canvas.style.height = `${h * kPaletteCssScale}px`
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  ctx.imageSmoothingEnabled = false
  ctx.clearRect(0, 0, w, h)
  const reserved = reservedIndexSet.value
  const ehbMode = isEhbMode(options.mode)
  for (let i = 0; i < n; ++i) {
    const cx = (i % kPalettePerRow) * kPaletteSwatchPx
    const cy = Math.floor(i / kPalettePerRow) * kPaletteSwatchPx
    // EHB halfbrite cells (32..63) cascade reservation: when
    // base[i-32] is reserved, the halfbrite cell renders cleared
    // too (the encoder won't dither into a reserved base or its
    // hardware-derived halfbrite).
    if (ehbMode && i >= 32 && reserved.has(i - 32)) {
      drawPaletteCarveCell(ctx, cx, cy)
    } else if (reserved.has(i)) {
      drawPaletteReserveX(ctx, cx, cy)
    } else {
      ctx.fillStyle = `rgb(${bytes[i*3]}, ${bytes[i*3+1]}, ${bytes[i*3+2]})`
      ctx.fillRect(cx, cy, kPaletteSwatchPx, kPaletteSwatchPx)
    }
  }
}

// Render the per-scanline palette as a vertical strip alongside the
// preview canvas. Backing is `N × H` pixels — one column per slot, one
// row per source scanline — with the same H-CSS dimension as the
// preview so they align vertically. CSS width scales per-slot to a
// visible swatch (kPalettePerLineSlotCssPx) regardless of slot count.
// At ≥ 32 slots we cap total width to kPalettePerLineMaxCssPx and let
// the per-slot width shrink so the strip stays on-screen for d=6..8
// (64/128/256 colors per row).
const kPalettePerLineSlotCssPx = 8
const kPalettePerLineMaxCssPx = 256
function fillRgbaFromRgb(out: Uint8ClampedArray, src: Uint8Array, pixels: number) {
  for (let i = 0; i < pixels; ++i) {
    out[i*4 + 0] = src[i*3 + 0] ?? 0
    out[i*4 + 1] = src[i*3 + 1] ?? 0
    out[i*4 + 2] = src[i*3 + 2] ?? 0
    out[i*4 + 3] = 255
  }
}
function paintScanlinePaletteStrip(result: ConvertResult, cssH: number) {
  const canvas = scanlinePaletteCanvasRef.value
  const bytes = result.scanlinePaletteBytes
  const n = result.scanlinePaletteSize ?? 0
  const rows = result.height
  const valid = canvas && bytes && n > 0 && rows > 0 && bytes.length >= rows * n * 3
  if (!valid) {
    hasScanlinePalette.value = false
    return
  }
  hasScanlinePalette.value = true
  canvas.width = n
  canvas.height = rows
  // Lock CSS height to the preview's so the strip aligns row-for-row
  // with the rendered image. Width is min(N×slot, maxWidth) so d=6..8
  // (64..256 colors) stays visible.
  const cssW = Math.min(n * kPalettePerLineSlotCssPx, kPalettePerLineMaxCssPx)
  canvas.style.width = `${cssW}px`
  canvas.style.height = `${cssH}px`
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  ctx.imageSmoothingEnabled = false
  const img = ctx.createImageData(n, rows)
  fillRgbaFromRgb(img.data, bytes, rows * n)
  ctx.putImageData(img, 0, 0)
}

// Custom floating tooltip that tracks the cursor while it's over a
// palette swatch. Native `title=` would only fire after a long pause,
// and only on the swatch the cursor entered first — bad UX for a 32-
// or 256-cell grid where you want to flick across colors and read
// values fly-by. The tooltip shows index, #RRGGBB, and rgb(...).
const paletteTooltip = reactive({
  visible: false,
  x: 0,
  y: 0,
  text: '',
  swatch: '#000',
})
interface PaletteHit { idx: number; r: number; g: number; b: number }
// Pixel offset within the canvas (or null if the canvas isn't
// visible). Split out to keep paletteHitAt under the eslint
// complexity gate.
function paletteCanvasOffset(clientX: number, clientY: number):
    { x: number; y: number; w: number; h: number } | null {
  const canvas = paletteCanvasRef.value
  if (!canvas) return null
  const rect = canvas.getBoundingClientRect()
  const x = clientX - rect.left
  const y = clientY - rect.top
  if (x < 0 || y < 0 || x >= rect.width || y >= rect.height) return null
  return { x, y, w: rect.width, h: rect.height }
}
// Returns the palette swatch under (clientX, clientY) including its
// RGB values, or null if the pointer is outside the canvas / past
// the populated swatches.
function paletteHitAt(clientX: number, clientY: number): PaletteHit | null {
  const off = paletteCanvasOffset(clientX, clientY)
  const bytes = lastPaletteBytes.value
  if (!off || !bytes) return null
  const cssPerSwatch = kPaletteSwatchPx * kPaletteCssScale
  const col = Math.floor(off.x / cssPerSwatch)
  const row = Math.floor(off.y / cssPerSwatch)
  const idx = row * kPalettePerRow + col
  if (idx < 0 || idx * 3 + 2 >= bytes.length) return null
  return {
    idx,
    r: bytes[idx * 3] ?? 0,
    g: bytes[idx * 3 + 1] ?? 0,
    b: bytes[idx * 3 + 2] ?? 0,
  }
}
// When a palette swatch is hovered, the preview canvas is repainted
// with all pixels NOT matching the swatch hidden (alpha=0). When the
// encoder emits a per-pixel index map (lastIndices, non-HAM modes)
// the filter uses index equality — distinguishes slots that resolve
// to the same RGB (e.g. EHB's slot 0 black-base vs slot 32 black-
// halfbrite). For HAM and other modes without indices we fall back
// to RGB-equality. Cleared on mouseleave.
const hoveredPaletteHit = ref<PaletteHit | null>(null)
function paletteHover(ev: MouseEvent) {
  const hit = paletteHitAt(ev.clientX, ev.clientY)
  if (!hit) {
    paletteTooltip.visible = false
    if (hoveredPaletteHit.value !== null) {
      hoveredPaletteHit.value = null
      repaintMaskedPreview()
    }
    return
  }
  const hex = '#' + [hit.r, hit.g, hit.b]
    .map(v => v.toString(16).padStart(2, '0')).join('')
  paletteTooltip.text =
    `idx ${hit.idx} · ${hex} · rgb(${hit.r}, ${hit.g}, ${hit.b})`
  paletteTooltip.swatch = `rgb(${hit.r}, ${hit.g}, ${hit.b})`
  paletteTooltip.x = ev.clientX + 12
  paletteTooltip.y = ev.clientY + 12
  paletteTooltip.visible = true
  if (hoveredPaletteHit.value?.idx !== hit.idx) {
    hoveredPaletteHit.value = hit
    repaintMaskedPreview()
  }
}
function paletteHoverLeave() {
  paletteTooltip.visible = false
  if (hoveredPaletteHit.value !== null) {
    hoveredPaletteHit.value = null
    repaintMaskedPreview()
  }
}

// Build a copy of lastRgba where pixels NOT matching `target` are
// turned transparent (alpha=0). When lastIndices is available we use
// per-pixel INDEX equality (so two slots with the same RGB still
// distinguish — EHB slot 0 vs 32 are both black but different idx).
// When indices are absent (HAM / sliced / strips / tile modes) we
// fall back to RGB equality. Returns rgba unchanged when target=null.
function maskByIndex(rgba: Uint8Array, indices: Uint8Array,
                     targetIdx: number, n: number): Uint8ClampedArray {
  const out = new Uint8ClampedArray(n * 4)
  for (let i = 0; i < n; ++i) {
    out[i * 4 + 0] = rgba[i * 4 + 0] ?? 0
    out[i * 4 + 1] = rgba[i * 4 + 1] ?? 0
    out[i * 4 + 2] = rgba[i * 4 + 2] ?? 0
    out[i * 4 + 3] = (indices[i] === targetIdx) ? 255 : 0
  }
  return out
}
function maskByRgb(rgba: Uint8Array,
                   tr: number, tg: number, tb: number,
                   n: number): Uint8ClampedArray {
  const out = new Uint8ClampedArray(n * 4)
  for (let i = 0; i < n; ++i) {
    const r = rgba[i * 4 + 0] ?? 0
    const g = rgba[i * 4 + 1] ?? 0
    const b = rgba[i * 4 + 2] ?? 0
    out[i * 4 + 0] = r
    out[i * 4 + 1] = g
    out[i * 4 + 2] = b
    out[i * 4 + 3] = (r === tr && g === tg && b === tb) ? 255 : 0
  }
  return out
}
function buildMaskedRgba(target: PaletteHit | null,
                         w: number, h: number): Uint8ClampedArray {
  const out = new Uint8ClampedArray(w * h * 4)
  if (!lastRgba) return out
  if (!target) { out.set(lastRgba); return out }
  const n = w * h
  const idx = lastIndices
  if (idx?.length === n) return maskByIndex(lastRgba, idx, target.idx, n)
  return maskByRgb(lastRgba, target.r, target.g, target.b, n)
}

// Repaint the preview canvas honoring the current hover mask.
// Page-CSS checkerboard shows through alpha=0 pixels.
function repaintMaskedPreview() {
  const canvas = canvasRef.value
  if (!canvas || !lastRgba || lastSrc.w === 0) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  const w = lastSrc.w, h = lastSrc.h
  const out = buildMaskedRgba(hoveredPaletteHit.value, w, h)
  const tmp = document.createElement('canvas')
  tmp.width = w; tmp.height = h
  const tmpCtx = tmp.getContext('2d')
  if (!tmpCtx) return
  tmpCtx.putImageData(new ImageData(new Uint8ClampedArray(out), w, h), 0, 0)
  ctx.imageSmoothingEnabled = false
  ctx.clearRect(0, 0, canvas.width, canvas.height)
  ctx.drawImage(tmp, 0, 0, canvas.width, canvas.height)
}
const lastPaletteBytes = ref<Uint8Array | null>(null)
watch(paletteViewActive, (on) => {
  if (!on) return
  const cached = lastPaletteBytes.value
  if (cached) void nextTick(() => { drawPalette(cached) })
})

// Reserve-palette panel. The 16×N grid renders one swatch per RESERVABLE
// palette slot. EHB carves out the halfbrite section (slots 32..63
// are hardware-derived from the base 32 — the encoder operates on the
// base palette only), so for EHB we only show the base. Other modes
// use the full emitted palette.
const reservablePaletteSize = computed(() => {
  const total = Math.floor((lastPaletteBytes.value?.length ?? 0) / 3)
  return isEhbMode(options.mode) ? Math.min(total, 32) : total
})
// Reserves are unsupported in modes where the encoder rejects the
// option (HAM dynamic palette, DPF split, multi-palette tile modes,
// fixed hardware-palette modes). Hide the panel in those cases —
// surfacing it would just cause the convert call to error.
// Modes whose palette is fixed in hardware, dynamic, auto-quantized, or
// split into multiple lines — none expose a single reservable CLUT.
// (HAM dynamic, Genesis/SNES multi-line, C64/Thomson/TED fixed-or-auto,
// CGA hardware-fixed, GBA direct = no palette.) DPF / DPF+sliced are NOT
// here: the CLI lets reserves through (PF2 base palette) and copper.cpp
// rejects OCS-snapping onto reserved colors.
function modeHasNoReservableClut(m: string): boolean {
  const fixedOrDynamic = [isHamMode, isGenesisMode, isSnesMode, isC64Mode,
    isThomsonMode, isTedMode, isCgaMode, isCgaText, isGbaDirectMode]
  return fixedOrDynamic.some(p => p(m))
}
const reservesSupported = computed(() => !modeHasNoReservableClut(options.mode))
const numReserveRows = computed(() => {
  if (!reservesSupported.value) return 0
  return Math.max(0, Math.ceil(reservablePaletteSize.value / 16))
})

// Flat grid items so we can drive the whole 16×N grid with a single
// v-for. The earlier nested template + nested v-for shape produced
// an off-by-one click target when Vue's keyed reconciliation reused
// nodes between rows. One flat list with stable keys eliminates it.
type ReserveGridItem =
  | { kind: 'corner'; key: string }
  | { kind: 'col-label'; key: string; text: string }
  | { kind: 'row-label'; key: string; text: string }
  | { kind: 'swatch';   key: string; idx: number; readonly: boolean }
const reserveGridItems = computed<ReserveGridItem[]>(() => {
  const items: ReserveGridItem[] = []
  items.push({ kind: 'corner', key: 'corner' })
  for (let c = 0; c < 16; c++) {
    items.push({ kind: 'col-label', key: `hc${c}`, text: HEX_DIGITS.charAt(c) })
  }
  const rows = numReserveRows.value
  const cap = reservablePaletteSize.value
  for (let r = 0; r < rows; r++) {
    items.push({ kind: 'row-label', key: `rl${r}`, text: HEX_DIGITS.charAt(r) })
    for (let c = 0; c < 16; c++) {
      const idx = r * 16 + c
      // Slot 0 is implicitly locked to black when "Reserve color 0 for
      // black" is on, and the encoder rejects redundant reserves on it.
      // Render the swatch read-only in that case so the user can see
      // the color but the click is inert.
      const readonly = (idx === 0 && options.lockColor0)
      items.push({
        kind: 'swatch',
        key: `sw${idx}`,
        idx: idx < cap ? idx : -1,
        readonly,
      })
    }
  }
  return items
})
const reservedIndexSet = computed(() =>
    new Set(options.reserves.map(r => r.index)))
function isReserved(idx: number): boolean {
  return reservedIndexSet.value.has(idx)
}
function toggleReserve(idx: number) {
  if (isReserved(idx)) clearReserve(idx)
  else setReserve(idx)
}
// Add a reserve at idx (no-op if already reserved). Used by the
// drag-paint set mode + keyboard toggle.
function setReserve(idx: number) {
  const bytes = lastPaletteBytes.value
  if (!bytes || idx * 3 + 2 >= bytes.length) return
  if (options.reserves.some(r => r.index === idx)) return
  options.reserves.push({
    index: idx,
    r: bytes[idx * 3 + 0] ?? 0,
    g: bytes[idx * 3 + 1] ?? 0,
    b: bytes[idx * 3 + 2] ?? 0,
  })
  if (paletteViewActive.value) {
    void nextTick(() => { drawPalette(bytes) })
  }
}
// Remove the reserve at idx (no-op if not reserved). Used by the
// drag-paint clear mode + keyboard toggle.
function clearReserve(idx: number) {
  const cur = options.reserves.findIndex(r => r.index === idx)
  if (cur === -1) return
  options.reserves.splice(cur, 1)
  const bytes = lastPaletteBytes.value
  if (paletteViewActive.value && bytes) {
    void nextTick(() => { drawPalette(bytes) })
  }
}

// Drag-to-paint reserve toggling. Mousedown picks a paint mode
// based on the starting cell's current state (set if it was
// unreserved, clear if it was reserved); mouseenter on subsequent
// cells applies that mode. Window-level mouseup ends the drag.
const reserveDragMode = ref<'set' | 'clear' | null>(null)
function reserveCellDown(idx: number, ev: MouseEvent) {
  // Only left button, and avoid text selection during drag.
  if (ev.button !== 0) return
  ev.preventDefault()
  if (isReserved(idx)) {
    reserveDragMode.value = 'clear'
    clearReserve(idx)
  } else {
    reserveDragMode.value = 'set'
    setReserve(idx)
  }
}
function reserveCellEnter(idx: number) {
  if (reserveDragMode.value === 'set') setReserve(idx)
  else if (reserveDragMode.value === 'clear') clearReserve(idx)
}
function endReserveDrag() { reserveDragMode.value = null }
addEventListener('mouseup', endReserveDrag)
onBeforeUnmount(() => {
  removeEventListener('mouseup', endReserveDrag)
})
// CSS background fill for one reserve-grid cell — reads the palette
// byte triple. Returns transparent for indices beyond the palette.
function reserveCellBg(idx: number): string {
  const bytes = lastPaletteBytes.value
  if (!bytes || idx * 3 + 2 >= bytes.length) return 'transparent'
  return `rgb(${bytes[idx*3]},${bytes[idx*3+1]},${bytes[idx*3+2]})`
}
const HEX_DIGITS = '0123456789ABCDEF'

// Clear stale reserves when the palette structure changes — a reserve
// at idx 63 from EHB doesn't make sense after switching to lores d=4
// (16 slots). Mode / depth / chipset changes are the obvious palette-
// size triggers.
watch(() => [options.mode, options.depth, options.chipset], () => {
  if (options.reserves.length > 0) options.reserves = []
})
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
// Set of palette-aware methods that don't fit HAM's encoder-bit
// pipeline — kept in one set so the filter expression stays simple
// (and below the eslint complexity limit).
const YLIL_FAMILY = new Set([
  'yliluoma', 'yliluoma2', 'opt-checker', 'knoll', 'tri-tone',
  'yliluoma1', 'opt-line', 'opt-line-checker',
  'opt-vline', 'opt-vline-checker',
])
// SNES Mode 7 Direct quantises every pixel directly to the RGB443 grid
// — there is no palette table, so the yliluoma family (palette-aware
// pattern dithers) has nothing to mix and is hidden. All other ordered
// + ED methods route through dither::diffuse_raw_buffer.

const groupedDitherOptions = computed(() => {
  const ht = hamType(options.mode)
  const hide_nonsquare = ht !== null
  // SNES Mode 7 Direct has no palette table at all → yliluoma family
  // has nothing to mix. HAM does support the family (ham.cpp's per-
  // pixel reachable set), so only Mode 7 Direct hides it now.
  // GBA direct modes (mode3/mode5) are gated exactly like SNES Mode 7
  // Direct: 16bpp BGR555 grid, no palette table → yliluoma has nothing
  // to mix and DBS has no indices to sweep.
  const hide_yliluoma = isSnesDirectMode(options.mode) || isGbaDirectMode(options.mode)
  // DBS sweeps palette indices and so doesn't apply in HAM (no fixed
  // palette) or the direct grid modes (BGR555 / RGB443 grid quantisation).
  const hide_dbs = ht !== null || isSnesDirectMode(options.mode) || isGbaDirectMode(options.mode)
  return DITHER_METHODS
    .map(g => ({
      label: g.group,
      items: g.items
        .filter(d => !(hide_nonsquare && isNonSquareDither(d.value)))
        .filter(d => !(hide_yliluoma && YLIL_FAMILY.has(d.value)))
        .filter(d => !(hide_dbs && d.value === 'dbs'))
        .map(d => ({ value: d.value, label: d.label }))
    }))
    .filter(g => g.items.length > 0)
})

// Whether depth slider should be shown. Hidden for modes where depth is
// fixed by the target hardware: HAM (N-2 data bits), EHB (always 6),
// Atari (mode defines depth), DOS modes (EGA/VGA/CGA/text — 1/2/4/8),
// and SNES Mode 7 (always 8bpp chunky).
const showDepthSlider = computed(() => {
  // Hidden for any mode where depth is fixed by the hardware buffer:
  // HAM (N-2 data bits), EHB (always 6), Atari (mode defines depth),
  // DOS (EGA/VGA/CGA/text — 1/2/4/8), SNES Mode 7 (8bpp chunky),
  // Sega Genesis (4bpp tiles), and c64 charset (1bpp/2bpp fixed by
  // mode). Tile-freeform modes left isFixedBufferMode in the recent
  // refactor but their depth is still hardware-fixed.
  // Positive gate: depth means bitplanes, which is an Amiga concept. The old
  // negation chain relied on every non-Amiga family being classified as
  // fixed-buffer or tile-freeform — true today, but it silently opens up for
  // any family added later that isn't.
  return isAmigaMode(options.mode) && !isHamMode(options.mode) &&
         !isEhbMode(options.mode)
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

// "Effective" fixed-buffer state — tile-freeform modes (c64-charset,
// Genesis, SNES) count as fixed-buffer when Resize is off (encoder
// runs at the mode default), so Native PAR + PAR-aware preview
// scaling kick in. With Resize on, those modes are freeform and
// Native PAR is meaningless.
const isEffectiveFixedBuffer = computed(() =>
  isFixedBufferMode(options.mode) ||
  (isTileFreeformMode(options.mode) && !sizeOverride.value)
)

// Dual playfield: only valid for standard Amiga modes (no HAM, no EHB,
// no Atari/DOS) at the matching depth for the current chipset (3 for
// OCS = 8 PF2 colors, 4 for AGA = 16).
//
// OCS hires is excluded (any -lace variant too): OCS hires caps at 4
// bitplanes total, so DPF would split 2+2 giving only 4 colors per
// playfield, and the chipset doesn't officially support hires+DPF.
//
// OCS lores-lace + DPF IS allowed — BPLCON0's LACE bit (2) and DBLPF
// bit (10) are independent and can be set together. The hardware does
// 320×400 with two 8-color playfields fine, even though the
// combination flickers on consumer monitors without scan-doubling.
// AGA hires + DPF (depth=4 → 4+4) is also fine.
// Sliced palette — Amiga copper only, and meaningless with a user-supplied
// palette (the per-line variants are derived from the encoder's own).
const slicedAvailable = computed(() => isAmigaMode(options.mode) && !paletteData.value)

const dpfAvailable = computed(() => {
  const m = options.mode
  // Amiga-only, same reason as the Sliced toggle: isFixedBufferMode() lets
  // the tile-freeform modes (c64-charset, Genesis, SNES) through.
  if (!isAmigaMode(m)) return false
  if (isHamMode(m) || isEhbMode(m) || isAtariMode(m)) return false
  const cs = effectiveChipset(m, options.chipset)
  if (cs === 'aga') return options.depth === 4
  return options.depth === 3 && !m.includes('hires')
})

// strips — mid-line palette swaps. Two flavours, both OCS lores only:
//   * DPF + lores (depth=3): 3-plane PF2, 8 base colors.
//   * EHB (mode=ehb): 32 base + 32 hardware-derived half-brites.
// strips is an extension to sliced (per-line palette evolution); enabling
// strips turns sliced on too, and turning sliced off cascades strips off.
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

// --best multi-restart sweep eligibility. Mirrors the C++ eligibility
// gates in src/api.cpp: lores/hires plain, EHB plain (no copper/scap/dpf),
// HAM6/HAM8 (any depth that maps to those modes), any sliced/strips
// mode (copper or scap on), and Thomson forme-couleur (pair-weight grid
// sweep).
const PLAIN_INDEXED_MODES = new Set([
  'lores', 'lores-lace', 'hires', 'hires-lace',
])
const FORMECOULEUR_MODES = new Set([
  'thomson-to7-320x16', 'thomson-to8-320x16',
])
const bestEligible = computed(() => {
  const m = options.mode
  if (options.copper || options.scap) return true
  const t = hamType(m)
  if (t === 'ham6' || t === 'ham8') return true
  if (options.dualPlayfield) return false
  if (isEhbMode(m)) return true
  if (FORMECOULEUR_MODES.has(m)) return true
  return PLAIN_INDEXED_MODES.has(m)
})

// Available modes for current chipset
const availableModes = computed(() => modesForChipset(options.chipset))

// Per-chipset example thumbnails. c64 ships with its own block-art
// sample pack; every other chipset falls back to the DEFAULT_EXAMPLES
// Amiga set (per options.ts). Switching chipset only swaps the example
// strip — the loaded image is preserved.
const availableExamples = computed(() => examplesForChipset(options.chipset))

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
  // Fixed-buffer + tile-freeform modes (DOS + SNES + Genesis + C64)
  // default to native PAR so the preview shows the right aspect.
  const isFixed = (m: string): boolean =>
    isFixedBufferMode(m) || isTileFreeformMode(m)
  const fixedNew = isFixed(mode)
  const fixedOld = isFixed(oldMode)
  if (fixedNew && !fixedOld) options.nativePar = true
  if (!fixedNew) options.nativePar = false
}

// Methods that don't dither in HAM — auto-fallback on mode change so
// the dither dropdown never shows a "selected but inactive" pick. The
// yliluoma family (opt-* / yliluoma / knoll / tri-tone) IS supported on
// HAM now (ham.cpp builds a per-pixel reachable set and feeds it to the
// pickers, mirroring ham_convert's "Checks (lines-mixed, optimal)"),
// so only DBS — which sweeps palette indices — stays incompatible.
const HAM_INCOMPATIBLE_DITHERS = new Set(['dbs'])

function maybeFallbackHamDither(mode: string): void {
  if (hamType(mode) !== null && HAM_INCOMPATIBLE_DITHERS.has(options.dither)) {
    // Atkinson wins HAM6 7/10 in our sweep and ties HAM8 4/10.
    options.dither = 'atkinson'
  }
}

function maybeFallbackSnesDirectDither(mode: string): void {
  // Mode 7 Direct has no palette table, so the yliluoma family (palette-
  // aware pattern dithers) is meaningless. Snap any yliluoma selection
  // to F-S; everything else routes through dither::diffuse_raw_buffer.
  if (isSnesDirectMode(mode) && YLIL_FAMILY.has(options.dither)) {
    options.dither = 'floyd-steinberg'
  }
}

function maybeFallbackGbaDirectDither(mode: string): void {
  // GBA direct modes (mode3/mode5) are 16bpp BGR555 with no palette
  // table — same gating as SNES Mode 7 Direct. Snap any yliluoma
  // selection back to F-S.
  if (isGbaDirectMode(mode) && YLIL_FAMILY.has(options.dither)) {
    options.dither = 'floyd-steinberg'
  }
}

// Genesis prefers opt-checker by default: its 2×2-phase threshold aligns
// with 8-pixel tile boundaries so tile dedup survives (~40% on photos).
// Error-diffusion methods (FS, atkinson, etc.) destroy dedup → blow the
// 1280-tile VRAM budget. Auto-snap to opt-checker when entering Genesis
// from a non-Genesis mode; switching between H32 and H40 keeps the
// user's current choice.
function maybeSelectGenesisDither(mode: string, oldMode?: string): void {
  if (!isGenesisMode(mode)) return
  if (oldMode && isGenesisMode(oldMode)) return
  options.dither = 'opt-checker'
}

// Update depth when mode changes — only clamp, don't reset
watch(() => options.mode, (mode, oldMode) => {
  clampDepthForMode(mode)
  maybeFallbackHamDither(mode)
  maybeFallbackSnesDirectDither(mode)
  maybeFallbackGbaDirectDither(mode)
  maybeSelectGenesisDither(mode, oldMode)
  syncNativeParToMode(mode, oldMode)
  // Resize toggle is only meaningful for non-fixed-buffer modes (Amiga
  // free-resolution + tile-freeform). Switching INTO a fixed-buffer
  // mode (Atari, c64 bitmap, DOS at default size) while sizeOverride
  // was on left the width/height inputs visible — clear the toggle so
  // the layout matches the mode.
  if (sizeOverride.value && isFixedBufferMode(mode)) {
    sizeOverride.value = false
  }
  // Sliced / DPF / strips all require chipset-/depth-specific shapes. Clear
  // any that the new mode can't express — otherwise the flag stays set in the
  // options dict and api.cpp rejects the convert outright ("--sliced/--copper
  // ... not supported"), which surfaces as a failed encode rather than a
  // hidden toggle.
  if (!slicedAvailable.value) options.copper = false
  if (!dpfAvailable.value) options.dualPlayfield = false
  if (!scapAvailable.value) options.scap = false
  track('mode-change', { from: oldMode, to: mode })
})

// DPF + copper now compose (copper branch in api.cpp expands to PF2 +
// shifts sliced register targets into the upper palette bank). Just track
// toggles. strips and copper still don't combine — strips supplies its own
// per-line copper stream.
watch(() => options.dualPlayfield, (on) => {
  if (!scapAvailable.value) options.scap = false
  track('dpf-toggle', { enabled: on })
})
watch(() => options.scap, (on) => {
  if (on) {
    // strips is an extension to sliced — make sure sliced is on too.
    options.copper = true
  }
  track('scap-toggle', { enabled: on })
})

// Turning Copper off pulls strips off too — strips layers mid-line moves
// on top of the sliced palette and is meaningless without it. Disabling strips alone
// only removes those mid-line moves; sliced stays on.
watch(() => options.copper, (on) => {
  if (!on && options.scap) options.scap = false
})

// Clear --best whenever the surrounding mode switches it ineligible
// (e.g. switching to a fixed-buffer mode or enabling DPF on lores).
watch(bestEligible, (eligible) => {
  if (!eligible) options.best = false
})

// Depth changes can invalidate DPF (requires depth=3 OCS / 4 AGA) and
// strips (depth=3 OCS lores only). Mode/dpf watchers above don't fire
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

// Per-chipset default dither method. Auto-set on chipset entry;
// user can still pick another from the dither gallery.
const CHIPSET_DEFAULT_DITHER: Record<string, string> = {
  c64: 'opt-checker',
}

function applyChipsetDefaults(chipset: string, oldChipset: string): void {
  const d = CHIPSET_DEFAULT_DITHER[chipset]
  if (d && chipset !== oldChipset) options.dither = d
}

// When chipset changes, reset mode if current mode isn't available
watch(() => options.chipset, (chipset, oldChipset) => {
  track('chipset-change', { from: oldChipset, to: chipset })
  maybeResetModeForChipset()
  if (isAtariMode(options.mode) || isFixedBufferMode(options.mode)) options.copper = false
  const max = maxDepth(options.mode, options.chipset)
  if (max > 0 && options.depth > max) options.depth = max
  // strips is OCS-only and DPF requires the chipset-specific depth — both
  // get invalidated when the chipset flips. The mode/dpf/depth watchers
  // above don't fire here, so reset directly.
  if (!dpfAvailable.value) options.dualPlayfield = false
  if (!scapAvailable.value) options.scap = false
  applyChipsetDefaults(chipset, oldChipset)
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
// cga-text snapshot: when the lock toggles ON for cga-text80x100, we
// freeze the current (options.width, options.height) ratio. Subsequent
// commits use that ratio rather than the source PNG aspect — lets the
// user unlock, type a custom 400×400, lock again, and have edits
// preserve THAT 1:1 instead of snapping back to source aspect. Other
// modes keep the simple source-aspect rule.
const cgaTextLockedAspect = ref<number>(0)
watch([aspectLocked, () => options.mode], ([locked, mode]) => {
  cgaTextLockedAspect.value = (locked && isCgaText(mode) &&
                               options.width > 0 && options.height > 0)
    ? options.height / options.width
    : 0
})

function widthHeightRatio(): number {
  if (isCgaText(options.mode) && cgaTextLockedAspect.value > 0) {
    return cgaTextLockedAspect.value
  }
  if (!imageWidth.value || !imageHeight.value) return 0
  return imageHeight.value / imageWidth.value
}

function onWidthCommit() {
  if (!aspectLocked.value) return
  if (!options.width) return
  const r = widthHeightRatio()
  if (!r) return
  options.height = Math.max(2, Math.round(options.width * r))
}

function onHeightCommit() {
  if (!aspectLocked.value) return
  if (!options.height) return
  const r = widthHeightRatio()
  if (!r) return
  options.width = Math.max(16, Math.round(options.height / r / 16) * 16)
}

// Build the options object to pass to WASM (matches wasm_bindings.cpp field names)
function buildWasmOptions(): WasmOptions {
  // Strip Options-only `null` from paletteData (WasmOptions wants `Uint8Array
  // | undefined`) and translate alphaDither's 'none' UI sentinel to the
  // empty-string the C++ side expects. Conditional spread on paletteData so
  // we don't write `paletteData: undefined` under exactOptionalPropertyTypes.
  // Deep-copy the reserves array — Vue's reactive() wraps nested
  // objects in Proxies and Web Workers reject those via structured-
  // clone with "[object Array] could not be cloned." A plain-object
  // map is safe to postMessage.
  const { paletteData, alphaDither, reserves, ...rest } = options
  // The "Reserve palette" panel uses RESERVE semantics: carved-out
  // slots are removed from the dither candidate set. The encoder
  // sizes the quantizer's palette for the unreserved slots only
  // (subtract reserves from qcount in api.cpp), so the rendered
  // image uses exactly max_colors - reserves unique colors.
  const cleanReserves = reserves.filter(r =>
    !(r.index === 0 && options.lockColor0))
  const out: WasmOptions = {
    ...rest,
    alphaDither: alphaDither === 'none' ? '' : alphaDither,
    reserves: cleanReserves.map(r => ({
      index: r.index, r: r.r, g: r.g, b: r.b,
    })),
    ...(paletteData ? { paletteData } : {}),
  }
  return out
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

// Auto-tuning of dither strength + error_clamp lives in the C++ encoder
// (api::run_pipeline resolves the -1.0f sentinel via
// dither_tuning::defaults_for at entry). The web frontend leaves the
// fields at sentinel by default so the encoder picks the tuned value
// for the current (mode, depth, dpf, scap, copper, chipset, method)
// bucket on every encode. No mode-watcher / re-fetch needed here.

// Track slider tweaks (debounced)
let tweakTimer: ReturnType<typeof setTimeout> | null = null
for (const s of SLIDERS) {
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

// Tile-budget framing: when the platform has a hard tile-count cap
// (c64-charset 256, SNES Mode 7 256), denominator = budget; total-cell
// count + dedup % move into the parenthetical so the constraint a
// designer worries about (budget headroom) is the headline.
function formatTileStatsBudget(
    u: number, t: number, ram_kb: string, budget: number,
    storage_label: string): string {
  const pct = t > 0 ? (1 - u / t) * 100 : 0
  return `tiles: ${u}/${budget} (${t} cells, ${pct.toFixed(1)}% dedup, ${ram_kb} KB ${storage_label})`
}

function formatTileStatsGenesis(u: number, t: number, ram_kb: string): string {
  const pct = t > 0 ? (1 - u / t) * 100 : 0
  // Plane-A budget warning: 1280 tiles before sprites/plane B.
  const tag = u > 1280 ? '⚠ ' : ''
  return `${tag}tiles: ${u}/${t} (${pct.toFixed(1)}% dedup, ${ram_kb} KB VRAM)`
}

function formatTileStatsCgaText(result: ConvertResult): string {
  // cga-text80x100 reports cell-grid only (no glyph dedup): cols = w/8,
  // rows derived from genesisTotalCells (re-used as the total cell
  // count). Matches the CLI's "Encoded: 8000 cells (80×100)" line.
  const t = result.genesisTotalCells ?? 0
  if (!t) return ''
  const cols = Math.max(1, Math.floor(result.width / 8))
  const rows = Math.max(1, Math.round(t / cols))
  return `${t} cells (${cols}×${rows})`
}

function formatTileStatsTiled(result: ConvertResult, mode: string): string {
  if (!result.genesisTotalCells || result.genesisUniqueTiles == null) return ''
  const u = result.genesisUniqueTiles
  const t = result.genesisTotalCells
  // tileDataBytes is the authoritative bytes-per-tile-pool number:
  // Genesis = 32 B/tile, SNES Mode 7 = 64 B/tile, c64 charset = 8 B/glyph.
  const ram_kb = ((result.tileDataBytes ?? (u * 32)) / 1024).toFixed(1)
  if (isC64CharsetMode(mode)) {
    const budget = Math.max(1,
      (options.tileBudget || 256) - (options.tileReserve || 0))
    return formatTileStatsBudget(u, t, ram_kb, budget, 'charset')
  }
  if (isSnesMode(mode)) {
    return formatTileStatsBudget(u, t, ram_kb, 256, 'VRAM')
  }
  return formatTileStatsGenesis(u, t, ram_kb)
}

function formatTileStats(result: ConvertResult, mode: string): string {
  if (isCgaText(mode)) return formatTileStatsCgaText(result)
  return formatTileStatsTiled(result, mode)
}

// Combine PSNR + S2 into a single comma-joined fragment so
// formatResultInfo's branch count stays inside the eslint complexity
// budget. Each metric is independently optional.
function formatQualityStats(result: ConvertResult): string {
  const out: string[] = []
  if (result.psnr != null && Number.isFinite(result.psnr)) {
    out.push(`PSNR: ${result.psnr.toFixed(1)} dB`)
  }
  if (result.s2 != null && Number.isFinite(result.s2)) {
    out.push(`S2: ${result.s2.toFixed(1)}`)
  }
  return out.join(', ')
}

function formatResultInfo(result: ConvertResult) {
  // result.colors is non-optional in ConvertResult, so the chain stops there.
  const colorCount = result.totalColors ?? result.colors
  // For DPF the encoded frame is N planes (PF2 + zeroed PF1) but only
  // PF2 = N/2 planes are visible. Report the visible-PF count to match
  // the CLI / .cpp viewer's Palette line.
  const visibleBpl = options.dualPlayfield && result.depth
    ? Math.floor(result.depth / 2) : result.depth
  // For cga-text the api keeps result.height at hw scanlines (200 for
  // canonical, halved for freeform). Show source-pixel dim so the
  // label matches the displayed canvas + user-typed Resize value.
  const dispW = sourceWidthFromResult(result)
  const dispH = sourceHeightFromResult(result)
  const parts = [`${dispW}x${dispH}, ${statusChipset.value}`,
                 `${visibleBpl || '?'}bpl, ${colorCount} colors`]
  pushIf(parts, result.copperChanges, `${(result.copperChanges ?? 0).toFixed(1)} avg sliced/line`)
  const sizeStats = formatSizeStats(result)
  pushIf(parts, sizeStats, sizeStats.slice(2))  // strip leading ", "
  pushIf(parts, result.quantError != null, `error: ${(result.quantError ?? 0).toFixed(2)}`)
  const quality = formatQualityStats(result)
  pushIf(parts, quality, quality)
  const tileStats = formatTileStats(result, options.mode)
  pushIf(parts, tileStats, tileStats)
  return parts.join(', ')
}

function paintPreviewCanvas(result: ConvertResult): { dw: number; dh: number; cssW: number; cssH: number } | null {
  const canvas = canvasRef.value
  if (!canvas || !result.rgba) return null
  const { sx, sy } = previewScale(options.mode)
  const dw = result.width * sx
  const dh = result.height * sy
  canvas.width = dw
  canvas.height = dh
  // Native-PAR preview: keep the canvas backing at integer-NN upscaled
  // resolution for sharp pixels, but CSS-stretch the displayed HEIGHT
  // so each pixel renders with the hardware PAR.
  //   Target CSS aspect = buffer_w * par / buffer_h (the real CRT frame)
  // With width pinned to dw, height becomes dw * buffer_h / (buffer_w * par).
  // PAR < 1 (tall pixels → EGA 640×200 etc.) stretches height UP;
  // PAR > 1 (wide pixels → CGA composite, C64 multicolor) compresses height DOWN.
  //
  // The PAR correction is independent of native_par's resize choice:
  // native_par toggles letterbox-vs-stretch resize, but the *display
  // aspect* of the encoded buffer always reflects what the hardware
  // would emit on a CRT. Fixed-buffer modes always apply modePar at
  // display time so e.g. C64 multicolor's 160×200 buffer renders 4:3-ish
  // regardless of letterbox-vs-stretch.
  const cssW = dw
  let cssH = dh
  if (isEffectiveFixedBuffer.value) {
    const par = modePar(options.mode)
    cssH = Math.round(dw * result.height / (result.width * par))
  }
  canvas.style.width = `${cssW}px`
  canvas.style.height = `${cssH}px`
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
  return { dw, dh, cssW, cssH }
}

// User-facing width matches *source* pixels (1:1 designer convention).
// For c64-charset-multicolor the encoder's result.width is the halved
// 4-px-per-cell logical raster (encoder halves internally — see
// compute_target_dims), so double it back when surfacing to the UI.
function sourceWidthFromResult(result: ConvertResult): number {
  return options.mode === 'c64-charset-multicolor'
    ? result.width * 2 : result.width
}

// User-facing height matches *source* pixels too. For cga-text80x100
// the encoder operates on the hardware-pixel buffer (halved height),
// so double it back when surfacing to the Resize inputs — otherwise
// flipping Resize on prepopulates 640x200 instead of 640x400 for a
// canonical default.
function sourceHeightFromResult(result: ConvertResult): number {
  return isCgaText(options.mode)
    ? result.height * 2 : result.height
}

function maybeSeedSizes(result: ConvertResult): void {
  // If size-override is on but width/height are 0 (fresh image just loaded),
  // seed the inputs with the natural defaults; triggers one idempotent
  // re-convert via the deep options watcher.
  if (!sizeOverride.value || (options.width && options.height)) return
  options.width = sourceWidthFromResult(result)
  options.height = sourceHeightFromResult(result)
}

function updateLastResultRefs(result: ConvertResult): void {
  lastWidth.value = sourceWidthFromResult(result)
  lastHeight.value = sourceHeightFromResult(result)
  lastCopPerLine.value = result.copperChanges ?? 0
  lastPlaneBytes.value = result.planeBytes ?? 0
  lastCopperBytes.value = result.copperBytes ?? 0
  lastChangesPerLine.value = result.changesPerLine ?? 0
  lastMaxMovesPerLine.value = result.maxMovesPerLine ?? 0
  lastAga.value = Boolean(result.aga)
  imageHasAlpha.value = Boolean(result.hasTransparency)
  lastPaletteBytes.value = result.paletteBytes ?? null
  redrawPaletteIfActive()
  maybeSeedSizes(result)
}

// Repaint the palette swatch grid if the user has the palette view
// open. Factored out so updateLastResultRefs stays under the eslint
// complexity gate.
function redrawPaletteIfActive(): void {
  if (!paletteViewActive.value) return
  const cached = lastPaletteBytes.value
  if (!cached) return
  void nextTick(() => { drawPalette(cached) })
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

// Charset diagnostic: paint each unique glyph onto charsetCanvasRef,
// colored by the glyph's first-occurrence cell.
//   hires:      8×8, c0/c1 = (bg/fg) from cell color nibbles.
//   multicolor: 4×8 logical, stretched 2× horizontally for display
//               parity; bg/mc1/mc2 from globals, fg from cell color.

interface CharsetLayout {
  cs: Uint8Array
  firstColor: Int16Array
  unique: number
  mc: boolean
  cellW: number
  xspan: number
  scale: number
  cols: number
  pixelW: number  // canvas width
  bg: number
  mc1: number
  mc2: number
  palName: string
}

function buildFirstColorMap(
    cs: Uint8Array, unique: number, cells: number,
    charsetEnd: number): Int16Array {
  const screen = cs.subarray(charsetEnd, charsetEnd + cells)
  const color = cs.subarray(charsetEnd + cells, charsetEnd + cells * 2)
  const firstColor = new Int16Array(unique).fill(-1)
  for (let c = 0; c < cells; c++) {
    const g = screen[c] ?? 0
    if (g < unique && firstColor[g] === -1) {
      firstColor[g] = color[c] ?? 0
    }
  }
  return firstColor
}

function charsetLayoutSizes(mc: boolean): { cellW: number; xspan: number } {
  return mc ? { cellW: 4, xspan: 2 } : { cellW: 8, xspan: 1 }
}

function charsetSourceValid(
    cs: Uint8Array | undefined, unique: number, cells: number): cs is Uint8Array {
  if (!cs || unique === 0 || cells === 0) return false
  return cs.length >= unique * 8 + cells * 2
}

function buildCharsetLayout(
    result: ConvertResult, mode: string): CharsetLayout | null {
  const unique = result.genesisUniqueTiles ?? 0
  const cells = result.genesisTotalCells ?? 0
  const cs = result.c64CharsetData
  if (!charsetSourceValid(cs, unique, cells)) return null
  const mc = mode === 'c64-charset-multicolor'
  const { cellW, xspan } = charsetLayoutSizes(mc)
  const scale = 1
  const cols = 16
  return {
    cs,
    firstColor: buildFirstColorMap(cs, unique, cells, unique * 8),
    unique, mc, cellW, xspan, scale, cols,
    pixelW: cols * cellW * xspan * scale,
    bg:  result.c64BgColor ?? 0,
    mc1: result.c64Mc1 ?? 0,
    mc2: result.c64Mc2 ?? 0,
    palName: options.c64Palette,
  }
}

function pickGlyphPixel(byte: number, p: number, lo: CharsetLayout,
                        fg: number, c0: number): number {
  if (!lo.mc) return ((byte >> (lo.cellW - 1 - p)) & 1) ? fg : c0
  const q = (byte >> ((lo.cellW - 1 - p) * 2)) & 3
  // 4-entry lookup: bg / mc1 / mc2 / fg, indexed by the 2-bit pair.
  const mcSlots = [lo.bg, lo.mc1, lo.mc2, fg]
  return mcSlots[q] ?? lo.bg
}

interface FillRectArgs {
  px: Uint8ClampedArray
  pixelW: number
  x: number
  y: number
  w: number
  h: number
  rgb: readonly [number, number, number]
}

function fillRect(a: FillRectArgs): void {
  const [r, g, b] = a.rgb
  for (let dy = 0; dy < a.h; dy++) {
    for (let dx = 0; dx < a.w; dx++) {
      const o = ((a.y + dy) * a.pixelW + (a.x + dx)) * 4
      a.px[o] = r; a.px[o + 1] = g; a.px[o + 2] = b; a.px[o + 3] = 255
    }
  }
}

interface PaintGlyphArgs {
  px: Uint8ClampedArray
  lo: CharsetLayout
  glyph: number
  cellColor: number
}

interface RowPaintCtx {
  args: PaintGlyphArgs
  gx0: number
  gy0: number
  fg: number
  c0: number
}

function paintGlyphRow(ctx: RowPaintCtx, byte: number, r: number): void {
  const { args: { px, lo }, gx0, gy0, fg, c0 } = ctx
  for (let p = 0; p < lo.cellW; p++) {
    const pal = pickGlyphPixel(byte, p, lo, fg, c0)
    fillRect({
      px, pixelW: lo.pixelW,
      x: gx0 + p * lo.xspan * lo.scale, y: gy0 + r * lo.scale,
      w: lo.xspan * lo.scale, h: lo.scale,
      rgb: c64PaletteRgb(lo.palName, pal),
    })
  }
}

function paintGlyphBlock(args: PaintGlyphArgs): void {
  const { lo, glyph: g, cellColor: c } = args
  const ctx: RowPaintCtx = {
    args,
    gx0: (g % lo.cols) * lo.cellW * lo.xspan * lo.scale,
    gy0: Math.floor(g / lo.cols) * 8 * lo.scale,
    fg: lo.mc ? (c & 0xF) : ((c >> 4) & 0xF),
    c0: lo.mc ? lo.bg : (c & 0xF),
  }
  for (let r = 0; r < 8; r++) {
    const byte = lo.cs[g * 8 + r] ?? 0
    paintGlyphRow(ctx, byte, r)
  }
}

function paintCharsetCanvas(result: ConvertResult): void {
  const canvas = charsetCanvasRef.value
  if (!canvas) return
  if (!isC64CharsetMode(options.mode)) return
  const lo = buildCharsetLayout(result, options.mode)
  if (!lo) return
  const gridRows = Math.ceil(lo.unique / lo.cols)
  canvas.width = lo.pixelW
  canvas.height = gridRows * 8 * lo.scale
  canvas.style.width = `${canvas.width * 2}px`
  canvas.style.height = `${canvas.height * 2}px`
  canvas.style.imageRendering = 'pixelated'
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  ctx.fillStyle = 'black'
  ctx.fillRect(0, 0, canvas.width, canvas.height)
  const imgData = ctx.createImageData(canvas.width, canvas.height)
  for (let g = 0; g < lo.unique; g++) {
    const c = lo.firstColor[g] ?? -1
    if (c === -1) continue
    paintGlyphBlock({ px: imgData.data, lo, glyph: g, cellColor: c })
  }
  ctx.putImageData(imgData, 0, 0)
}

// ---------------------------------------------------------------------------
// Genesis tile diagnostic.
// ---------------------------------------------------------------------------

// Decode 3-bit channel value to 8-bit sRGB via bit replication
// (c << 5 | c << 2 | c >> 1).
function expand3to8(c: number): number {
  return ((c & 0x7) << 5) | ((c & 0x7) << 2) | ((c & 0x7) >> 1)
}

// Decode a Genesis BGR333 word (LE u16) to an [R, G, B] sRGB triple.
function decodeGenesisColor(word: number): [number, number, number] {
  const r3 = (word >> 1) & 0x7
  const g3 = (word >> 5) & 0x7
  const b3 = (word >> 9) & 0x7
  return [expand3to8(r3), expand3to8(g3), expand3to8(b3)]
}

function fillGenesisPalette(
    palette: Uint8Array): readonly [number, number, number][][] {
  // 4 lines × 16 entries × [R, G, B].
  const out: [number, number, number][][] = []
  for (let line = 0; line < 4; line++) {
    const lineOut: [number, number, number][] = []
    for (let i = 0; i < 16; i++) {
      const off = (line * 16 + i) * 2
      const lo = palette[off] ?? 0
      const hi = palette[off + 1] ?? 0
      lineOut.push(decodeGenesisColor((hi << 8) | lo))
    }
    out.push(lineOut)
  }
  return out
}

interface GenesisFirstLineMap { firstLine: Int8Array }

function genesisFirstLines(
    tilemap: Uint8Array, uniqueTiles: number): GenesisFirstLineMap {
  const firstLine = new Int8Array(uniqueTiles).fill(-1)
  const cellCount = tilemap.length / 2
  for (let c = 0; c < cellCount; c++) {
    const lo = tilemap[c * 2] ?? 0
    const hi = tilemap[c * 2 + 1] ?? 0
    const word = (hi << 8) | lo
    const idx = word & 0x07FF
    const line = (word >> 13) & 0x3
    if (idx < uniqueTiles && firstLine[idx] === -1) {
      firstLine[idx] = line
    }
  }
  return { firstLine }
}

interface GenesisTileCtx {
  px: Uint8ClampedArray
  pixelW: number
  scale: number
  cols: number
  tileBytes: Uint8Array
  palette: readonly (readonly [number, number, number])[][]
}

function paintTilePixel(args: {
  px: Uint8ClampedArray; pixelW: number; scale: number;
  x: number; y: number; rgb: readonly [number, number, number];
}): void {
  const [pr, pg, pb] = args.rgb
  for (let dy = 0; dy < args.scale; dy++) {
    for (let dx = 0; dx < args.scale; dx++) {
      const o = ((args.y + dy) * args.pixelW + (args.x + dx)) * 4
      args.px[o] = pr; args.px[o + 1] = pg
      args.px[o + 2] = pb; args.px[o + 3] = 255
    }
  }
}

function paintGenesisTile(ctx: GenesisTileCtx, g: number, line: number): void {
  const gx0 = (g % ctx.cols) * 8 * ctx.scale
  const gy0 = Math.floor(g / ctx.cols) * 8 * ctx.scale
  const linePal = ctx.palette[line] ?? []
  for (let r = 0; r < 8; r++) {
    for (let p = 0; p < 8; p++) {
      const byte = ctx.tileBytes[g * 32 + r * 4 + (p >> 1)] ?? 0
      const idx = (p & 1) ? (byte & 0xF) : ((byte >> 4) & 0xF)
      paintTilePixel({
        px: ctx.px, pixelW: ctx.pixelW, scale: ctx.scale,
        x: gx0 + p * ctx.scale, y: gy0 + r * ctx.scale,
        rgb: linePal[idx] ?? [0, 0, 0],
      })
    }
  }
}

interface GenesisTileInputs {
  tileBytes: Uint8Array
  tilemap: Uint8Array
  palBytes: Uint8Array
  unique: number
}

function genesisTileInputs(result: ConvertResult): GenesisTileInputs | null {
  const tileBytes = result.genesisTileBytes
  const tilemap = result.genesisTilemapBytes
  const palBytes = result.genesisPaletteBytes
  const unique = result.genesisUniqueTiles ?? 0
  if (!tileBytes || !tilemap || !palBytes || unique === 0) return null
  return { tileBytes, tilemap, palBytes, unique }
}

function paintGenesisTilesCanvas(result: ConvertResult): void {
  const canvas = genesisTilesCanvasRef.value
  if (!canvas || !isGenesisMode(options.mode)) return
  const inputs = genesisTileInputs(result)
  if (!inputs) return
  const palette = fillGenesisPalette(inputs.palBytes)
  const { firstLine } = genesisFirstLines(inputs.tilemap, inputs.unique)
  const scale = 1
  const cols = 16
  const rows = Math.ceil(inputs.unique / cols)
  canvas.width = cols * 8 * scale
  canvas.height = rows * 8 * scale
  canvas.style.width = `${canvas.width * 2}px`
  canvas.style.height = `${canvas.height * 2}px`
  canvas.style.imageRendering = 'pixelated'
  const ctx2d = canvas.getContext('2d')
  if (!ctx2d) return
  ctx2d.fillStyle = 'black'
  ctx2d.fillRect(0, 0, canvas.width, canvas.height)
  const imgData = ctx2d.createImageData(canvas.width, canvas.height)
  const ctx: GenesisTileCtx = {
    px: imgData.data, pixelW: canvas.width, scale, cols,
    tileBytes: inputs.tileBytes, palette,
  }
  for (let g = 0; g < inputs.unique; g++) {
    const line = firstLine[g] ?? -1
    if (line === -1) continue
    paintGenesisTile(ctx, g, line)
  }
  ctx2d.putImageData(imgData, 0, 0)
}

// ---------------------------------------------------------------------------
// SNES Mode 7 tile diagnostic.
// ---------------------------------------------------------------------------

interface SnesTileCtx {
  px: Uint8ClampedArray
  pixelW: number
  scale: number
  cols: number
  tileBytes: Uint8Array
  palette: Uint8Array | undefined  // 256×3 RGB; undefined = Direct
}

function snesPixelRgb(byte: number, palette: Uint8Array | undefined): [number, number, number] {
  if (palette && palette.length >= 768) {
    const off = byte * 3
    return [palette[off] ?? 0, palette[off + 1] ?? 0, palette[off + 2] ?? 0]
  }
  // Direct: BBGGGRRR → expand to 8-bit per channel.
  const r3 = byte & 0x7
  const g3 = (byte >> 3) & 0x7
  const b2 = (byte >> 6) & 0x3
  // 3-bit → 8-bit replication for r/g; 2-bit → 8-bit for b.
  const b8 = ((b2 & 0x3) << 6) | ((b2 & 0x3) << 4) | ((b2 & 0x3) << 2) | (b2 & 0x3)
  return [expand3to8(r3), expand3to8(g3), b8]
}

function paintSnesTile(ctx: SnesTileCtx, g: number): void {
  const gx0 = (g % ctx.cols) * 8 * ctx.scale
  const gy0 = Math.floor(g / ctx.cols) * 8 * ctx.scale
  for (let r = 0; r < 8; r++) {
    for (let p = 0; p < 8; p++) {
      const byte = ctx.tileBytes[g * 64 + r * 8 + p] ?? 0
      paintTilePixel({
        px: ctx.px, pixelW: ctx.pixelW, scale: ctx.scale,
        x: gx0 + p * ctx.scale, y: gy0 + r * ctx.scale,
        rgb: snesPixelRgb(byte, ctx.palette),
      })
    }
  }
}

function paintSnesTilesCanvas(result: ConvertResult): void {
  const canvas = snesTilesCanvasRef.value
  if (!canvas || !isSnesMode(options.mode)) return
  const tileBytes = result.snesTileBytes
  if (!tileBytes) return
  const palette = result.snesPaletteBytes
  const unique = Math.floor(tileBytes.length / 64)
  const scale = 1
  const cols = 16
  const rows = Math.ceil(unique / cols)
  canvas.width = cols * 8 * scale
  canvas.height = rows * 8 * scale
  canvas.style.width = `${canvas.width * 2}px`
  canvas.style.height = `${canvas.height * 2}px`
  canvas.style.imageRendering = 'pixelated'
  const ctx2d = canvas.getContext('2d')
  if (!ctx2d) return
  ctx2d.fillStyle = 'black'
  ctx2d.fillRect(0, 0, canvas.width, canvas.height)
  const imgData = ctx2d.createImageData(canvas.width, canvas.height)
  const ctx: SnesTileCtx = {
    px: imgData.data, pixelW: canvas.width, scale, cols,
    tileBytes, palette,
  }
  for (let g = 0; g < unique; g++) paintSnesTile(ctx, g)
  ctx2d.putImageData(imgData, 0, 0)
}

async function paintAndCacheResult(result: ConvertResult): Promise<boolean> {
  const painted = paintPreviewCanvas(result)
  if (!painted || !result.rgba) return false
  const { cssW, cssH } = painted
  // Cache source for CRT re-render on toggle without re-encoding.
  lastRgba = new Uint8Array(result.rgba)
  lastIndices = result.indices ? new Uint8Array(result.indices) : null
  lastSrc = { w: result.width, h: result.height }
  lastDst = { w: cssW, h: cssH }
  if (crtEnabled.value) {
    const r = await ensureCrtRenderer()
    if (r) renderCrt()
  }
  paintCharsetCanvas(result)
  paintGenesisTilesCanvas(result)
  paintSnesTilesCanvas(result)
  paintScanlinePaletteStrip(result, cssH)
  return true
}

// Generation counters. `convertGen` increments per runConvert start;
// `committedGen` tracks the highest gen that has *already painted*. We
// drop a finishing convert only if a NEWER convert has already painted
// (i.e. its result is on screen) — not merely if a newer one has
// started. Without this distinction, two concurrent runConverts (e.g.
// the default-example load racing with a click on a second example)
// would both check `myGen !== convertGen` against the latest START gen
// and the FIRST-finishing one would always drop, leaving the canvas
// black until the second finished too. Stop bumps both counters so any
// in-flight encode finishing after a stop sees myGen <= committedGen
// and bails.
let convertGen = 0
let committedGen = 0

function handleConvertSuccess(result: ConvertResult,
                               myGen: number,
                               convertStart: number): void {
  if (result.error) {
    errorMsg.value = result.error
    track('error', { type: 'convert', message: result.error, mode: options.mode })
    return
  }
  void (async () => {
    let ok = false
    try { ok = await paintAndCacheResult(result) } catch { /* nop */ }
    if (!ok || myGen <= committedGen) return
    committedGen = myGen
    updateLastResultRefs(result)
    resultInfo.value = formatResultInfo(result)
    trackConvertSuccess(result, performance.now() - convertStart)
  })()
}

async function runConvert(srcBytes: Uint8Array): Promise<void> {
  const myGen = ++convertGen
  errorMsg.value = ''
  const convertStart = performance.now()
  if (spinnerTimer) clearTimeout(spinnerTimer)
  spinnerTimer = setTimeout(() => {
    if (myGen === convertGen) converting.value = true
  }, 100)
  progress.value = 0
  progressStage.value = ''
  const onProgress = (p: number, stage: string) => {
    // Progress ticks: only the latest START gen drives the bar (cheap
    // way to avoid two encodes' progress flickering into each other).
    if (myGen !== convertGen) return
    progress.value = Math.round(p * 100)
    progressStage.value = stage || ''
  }
  try {
    const result = await convertRGBA(srcBytes, buildWasmOptions(), onProgress)
    // Don't drop here based on convertGen: a slower encode that
    // happens to finish first should still get a chance to paint.
    // The committedGen check inside handleConvertSuccess will reject
    // results where a newer one has already painted.
    if (myGen <= committedGen) return
    clearTimeout(spinnerTimer)
    progress.value = 0
    progressStage.value = ''
    handleConvertSuccess(result, myGen, convertStart)
  } catch (error) {
    if (myGen <= committedGen) return
    clearTimeout(spinnerTimer)
    const message = errorMessage(error)
    errorMsg.value = message
    track('error', { type: 'convert-exception', message })
  } finally {
    if (myGen === convertGen) converting.value = false
  }
}

function doConvert() {
  if (debounceTimer) clearTimeout(debounceTimer)
  // If a convert is already running in the WASM worker, abort it so
  // the new settings take effect immediately. Without this, slow
  // encodes (--best, HAM6 sliced, etc.) make the UI feel laggy: the
  // user changes a slider, then watches the previous-settings encode
  // finish before their change kicks in. Bumping convertGen drops the
  // in-flight runConvert's result on completion (myGen != convertGen).
  if (converting.value) {
    abortWasm()
    convertGen++
    committedGen = convertGen
  }
  // Capture bytes at FIRE time, not schedule time — the active image
  // can change during the 150 ms debounce window (drag-drop while a
  // slow encode is still queued) and we want the encode to use the
  // image that's actually selected when the timer fires.
  debounceTimer = setTimeout(() => {
    const bytes = imageBytes.value
    if (!bytes || wasmLoading.value) return
    void runConvert(bytes)
  }, 150)
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

async function downloadPRG() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertPRG(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    downloadBlob(result.data, baseStem() + '.prg', 'application/octet-stream')
    exportCount++
    track('export', { format: 'prg', mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

async function downloadKoa() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertKoa(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    downloadBlob(result.data, baseStem() + '.koa', 'application/octet-stream')
    exportCount++
    track('export', { format: 'koa', mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

async function downloadHir() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertHir(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    downloadBlob(result.data, baseStem() + '.hir', 'application/octet-stream')
    exportCount++
    track('export', { format: 'hir', mode: options.mode, exportCount })
  } catch (error) { errorMsg.value = errorMessage(error) }
  converting.value = false
}

// Plain .h export — for non-Amiga modes the C++ side dispatches per
// platform: c64-charset → c64::charset_header, Genesis → SGDK header,
// SNES → minimal Mode 7 .h. Amiga modes still use cheader::generate.
async function downloadHeader() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const stem = options.symbolName ||
      (imageName.value || 'image').replace(/\.[^.]+$/, '').replaceAll(/\W/g, '_')
    const opts = buildWasmOptions()
    opts.symbolName = stem
    const result = await convertHeader(imageBytes.value, opts, stem)
    if (result.error) { errorMsg.value = result.error; return }
    if (!result.data) return
    downloadBlob(result.data, baseStem() + '.h', 'text/plain')
    exportCount++
    track('export', { format: 'h', mode: options.mode, exportCount })
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
  return result.header ?? (result.data ? new TextDecoder().decode(result.data) : '')
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
    // Map service-side format string to file extension. dos-exe / dos-
    // img are distinct code paths server-side but the user-facing file
    // extension is just exe / img.
    const ext = format.startsWith('dos-') ? format.slice(4) : format
    downloadBlob(blob, baseStem() + '.' + ext, 'application/octet-stream')
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

  // Fetch the new image FIRST, before touching reactive state. This
  // matters because the convertGen race fix (commit 2bafc49) makes
  // doConvert read imageBytes.value at debounce FIRE time, not
  // schedule time. If we set options + then awaited the fetch + then
  // set imageBytes, the debounce could fire mid-fetch and runConvert
  // with the OLD bytes + the NEW options. Doing the fetch first means
  // the bytes/options assignments below happen back-to-back and a
  // single debounce tick covers both.
  const resp = await fetch(`/examples/${example.file}`)
  const buf = await resp.arrayBuffer()

  // Reset to defaults, then apply example-specific settings.
  // nextTick between the two so watchers actually observe the
  // intermediate "all defaults" state — without it Vue batches both
  // Object.assigns into a single flush and watchers compare end vs
  // initial. For strips that means scap stays true→true (no fire,
  // copper never auto-enables) while the copper watcher DOES fire
  // (true→false) and cascades scap off. Net effect: clicking the
  // strips example a second time toggles copper off.
  Object.assign(options, defaultOptions())
  await nextTick()
  if (example.opts) Object.assign(options, example.opts)
  imageBytes.value = new Uint8Array(buf)
  imageName.value = example.file
  const type = example.file.endsWith('.jpg') || example.file.endsWith('.jpeg') ? 'image/jpeg' : 'image/png'
  const blob = new Blob([buf], { type })
  imageUrl.value = URL.createObjectURL(blob)
  // Decode the example's intrinsic dimensions so Resize presets
  // (100% / 50% / 25%) and the aspect-lock pick up the new image.
  // Without this, imageWidth/imageHeight stay at the previous
  // example's dims and presets always emit the same numbers.
  const img = new Image()
  img.addEventListener('load', () => {
    imageWidth.value = img.width
    imageHeight.value = img.height
  })
  img.src = imageUrl.value
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
      <!-- Controls sidebar — give it priority over the preview when the
           viewport narrows. The percentage classes (md:col-4 lg:col-3)
           determine the BASE share, but `controls-col` adds a hard
           min-width so the labels never wrap awkwardly; the preview
           column eats any remaining space (CSS below). -->
      <div class="col-12 md:col-4 lg:col-3 controls-col">
        <div class="flex flex-column gap-3">

          <!-- Upload / Image -->
          <Panel header="Image">
            <div
              role="button" tabindex="0"
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
              @keydown.enter.space.prevent="openPicker(); dismissHint()"
            >
              <template v-if="imageUrl">
                <div class="relative">
                  <img :src="imageUrl" alt="Source image preview" class="original-preview w-full border-round" />
                  <div v-if="showUploadHint" role="button" tabindex="0" class="upload-hint"
                    @click.stop="dismissHint(); openPicker()"
                    @keydown.enter.space.prevent="dismissHint(); openPicker()"
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
                  <span role="button" tabindex="0" class="white-space-nowrap ml-2 cursor-pointer flex-shrink-0"
                    @click.stop="openPicker(); dismissHint()"
                    @keydown.enter.space.prevent="openPicker(); dismissHint()">Change</span>
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
                  v-for="ex in availableExamples" :key="ex.name"
                  role="button" tabindex="0"
                  class="example-thumb cursor-pointer border-round overflow-hidden"
                  :class="{ 'ring-1 ring-primary': imageName === ex.file }"
                  @click="loadExample(ex)"
                  @keydown.enter.space.prevent="loadExample(ex)"
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
              <div v-if="isCgaText(options.mode)" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold"
                  title="Per-cell error metric for the glyph + (fg, bg) brute-force search. Pappas-Neuhoff (perceptual blur) needs the continuous source and hides the dither selector. MSE pairs with pixel-level dither below.">
                  Metric
                </label>
                <div class="col-8">
                  <Select v-model="options.cgaTextMetric" :options="CGA_TEXT_METRICS"
                    optionLabel="label" optionValue="value" class="w-full" />
                </div>
              </div>

              <!-- CGA-text mode only, blur-metric only: blur kernel
                   shape. Auto picks per-mode default (aniso53 for 8×1
                   cells, wide55 for 8×2, wide77 for 8×4 / 8×8) — tuned
                   on examples/{makena, lovers, fantasy, asterix, jungle,
                   photo, maui, electrichues02} via SSIMULACRA2. -->
              <div v-if="isCgaText(options.mode) && options.cgaTextMetric === 'blur'"
                   class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold"
                  title="Pappas-Neuhoff blur-kernel shape. The kernel must match the cell aspect: tall-cell modes (8×1, 8×2) want horizontal-favoring kernels, square-cell modes (8×4, 8×8) want symmetric. Auto selects bench-tuned per-mode defaults; manual override for A/B.">
                  Kernel
                </label>
                <div class="col-8">
                  <Select v-model="options.cgaTextKernel" :options="CGA_TEXT_KERNELS"
                    optionLabel="label" optionValue="value" class="w-full" />
                </div>
              </div>

              <!-- C64 mode only: VIC-II palette selector. The VIC-II's analogue
                   composite output has no canonical sRGB reference; pick the
                   one whose look you prefer. -->
              <div v-if="options.chipset === 'c64'" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold"
                  title="VIC-II 16-color palette. Pepto is the most-cited reference; VICE/Colodore/Deekay/Godot/C64Wiki/Levy are alternative measurements or community standards.">
                  Palette
                </label>
                <div class="col-8">
                  <Select v-model="options.c64Palette" :options="C64_PALETTES"
                    optionLabel="label" optionValue="value" class="w-full" />
                </div>
              </div>

              <!-- PETSCII only: per-cell error metric. Other c64 modes
                   accept the parameter for API symmetry but ignore it
                   (encode_multicolor / hires / fli / afli / charset
                   all use a fixed sRGB MSE-nearest pick) — surfacing
                   it would imply a toggle that does nothing. -->
              <div v-if="options.mode === 'c64-petscii'" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold"
                  title="Per-cell error metric for C64 brute-force scoring. Blur = Pappas-Neuhoff perceptual blur (default). MSE = per-pixel sRGB squared error. SSIM = structural similarity. Try them — they pick noticeably different outputs.">
                  Metric
                </label>
                <div class="col-8">
                  <Select v-model="options.c64Metric" :options="C64_METRICS"
                    optionLabel="label" optionValue="value" class="w-full" />
                </div>
              </div>

              <!-- C64 mode only: match palette range. Stretches the
                   source's OKLab extent to span the VIC-II palette's
                   reachable range so quantisation has headroom on
                   highlights / shadows. Mirrors png2c64. -->
              <div v-if="options.chipset === 'c64'" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold"
                  title="Stretch the source's chroma at every (lightness, hue) slice onto the VIC-II palette's reachable gamut. Hue and luminance preserved, chroma scaled per-hue so the source uses the full palette extent. Replaces the old axis-aligned bounding-box stretch.">
                  Match range
                </label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.matchRange" />
                  <span style="color: #888; font-size: 0.625rem;">fit chroma to palette gamut</span>
                </div>
              </div>

              <!-- PETSCII only: restrict candidate glyphs to graphics
                   subset (no letters / digits / punctuation). -->
              <div v-if="options.mode === 'c64-petscii'" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold"
                  title="Restrict the candidate glyph set to PETSCII semi-graphics + blocks (~130 chars). Skips letters, digits, and punctuation that would look out-of-place in smooth halftone areas.">
                  Graphics only
                </label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.c64PetsciiGraphicsOnly" />
                  <span style="color: #888; font-size: 0.625rem;">no letters/digits/punctuation</span>
                </div>
              </div>

              <div v-if="!(isCgaText(options.mode) && options.cgaTextMetric !== 'mse')
                          && options.mode !== 'c64-petscii'"
                class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Dithering algorithm. Ordered methods use fixed patterns; error diffusion propagates quantization error to neighbors.">Dither</label>
                <div class="col-8">
                  <DitherGallery v-model="options.dither" :groups="groupedDitherOptions" />
                </div>
              </div>

              <!-- sliced — Sliced palette (Amiga copper only) -->
              <div v-if="slicedAvailable" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Sliced palette: per-scanline palette swaps via the Copper coprocessor, picked greedily by OKLab error reduction. Each row gets its own per-line variant of the base palette. Composes with --dpf (palette evolves across the upper PF2 register bank).">Sliced</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.copper" />
                  <span style="color: #888; font-size: 0.625rem;">Sliced palette</span>
                </div>
              </div>
              <!-- Dual playfield (standard Amiga lores/hires + matching depth). -->
              <div v-if="dpfAvailable" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Dual playfield: encode the image into PF2 (upper color registers 8-15 OCS / 16-31 AGA), with PF1 (foreground) bitplanes left zeroed. Requires depth = 3 (OCS) or 4 (AGA). CAMG DBLPF flag set.">Dual PF</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.dualPlayfield" />
                  <span style="color: #888; font-size: 0.625rem;">PF2 only, upper color regs</span>
                </div>
              </div>

              <!-- strips — mid-line palette swaps (OCS lores only, DPF or EHB) -->
              <div v-if="scapAvailable" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Strip palette: mid-line palette swaps inside the displayed area, on top of the sliced palette's per-line evolution. 19 MOVEs per scanline at 16-lores-px stride; slot HPOS table calibrated against real OCS hardware. Two flavours: DPF (3-plane PF2, 8 base colors) and EHB (32 base + 32 hardware-derived half-brites).">Strips</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.scap" />
                  <span style="color: #888; font-size: 0.625rem;">mid-line swaps</span>
                </div>
              </div>

              <!-- Best quality. Sits LAST in the cap section so
                   the user enables it after they've chosen mode + cap +
                   scap + dpf — flipping --best earlier doesn't change
                   the algorithmic choices, just the time budget. Parallel
                   multi-restart sweep over (dither_strength × diversity
                   × image-jitter); picks the best-scoring trial. Active
                   for: HAM+sliced (centroid refinement), plain sliced, EHB+sliced,
                   strips DPF, strips EHB. -->
              <div v-if="bestEligible" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Best-quality search. Spends ~20–30× the encode time but searches many more candidates (jittered base palettes × dither strengths × diversities) and picks the one that looks best.">Best</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.best" />
                  <span style="color: #888; font-size: 0.625rem;">~20–30× slower, parallel</span>
                </div>
              </div>
              <!-- Native PAR (DOS + SNES + Genesis + C64 — modes with fixed
                   hardware buffer): preserve source aspect by letterboxing/
                   pillarboxing the image inside the fixed frame instead of
                   stretching. Stays visible (grayed out) for tile-freeform
                   modes when Resize is on so the layout doesn't jump. -->
              <div v-if="isFixedBufferMode(options.mode) || isTileFreeformMode(options.mode)"
                   class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Preserve source aspect ratio on fixed-buffer hardware (DOS / SNES) by letterboxing (reduce height) or pillarboxing (reduce width). Off = stretch to fill the full buffer.">Native PAR</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.nativePar" :disabled="!isEffectiveFixedBuffer" />
                </div>
              </div>

              <!-- Resize override (not for Atari, DOS, or SNES — fixed hardware buffer) -->
              <div v-if="!isAtariMode(options.mode) && !isFixedBufferMode(options.mode)" class="grid align-items-center">
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
            <!-- IBM PC DOS export buttons. img is a 720KB bootable
                 FAT12 floppy (FreeDOS + AUTOEXEC.BAT + the viewer) —
                 drop into MartyPC / real hardware and it runs. exe is
                 the same viewer alone for users with their own boot
                 setup. c is the generated source for local compile;
                 raw is the bare pixel/palette bytes. -->
            <div v-if="isDosMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="img" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="compileAndDownload('dos-img')"
                title="Download a bootable 720KB FAT12 floppy image (FreeDOS kernel + COMMAND.COM + the viewer on AUTOEXEC). Drop into MartyPC or write to a real floppy and it runs." />
              <Button label="exe" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="compileAndDownload('dos-exe')"
                title="Download a real-mode 16-bit MS-DOS .exe that displays the image. Builds server-side via ia16-elf-gcc; press any key to exit." />
            </div>
            <div v-if="isDosMode(options.mode)" class="flex gap-2">
              <Button label="c" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadViewer"
                title="Download the freestanding 16-bit DOS C viewer source. Build locally: ia16-elf-gcc -march=i80286 -mcmodel=small -Os -o out.exe viewer.c" />
              <Button label="raw" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadRaw"
                :title="isEgaMode(options.mode)
                  ? 'Download raw EGA planar data: 4 bitplanes + 16-byte IrgbIRGB palette (DMA-ready for 0xA0000).'
                  : isVgaMode(options.mode)
                  ? 'Download raw VGA planar data: 4 bitplanes + 16×3-byte 6-bit DAC palette (feed to 0x3C9).'
                  : 'Download raw CGA banked planar data (cga-320: 4-color 2bpp; cga-640: 2-color 1bpp).'" />
            </div>
            <!-- C64 export buttons: PNG preview + .prg displayer + format-specific raw. -->
            <div v-if="isC64Mode(options.mode) && !isC64CharsetMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="prg" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPRG"
                title="Download a runnable C64 .prg with embedded 6502 displayer (Koala for multicolor, Art Studio for hires, FLI/AFLI/PETSCII as appropriate)." />
              <Button v-if="options.mode === 'c64-multicolor'" label="koa" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadKoa"
                title="Download Koala Paint .koa (raw bitmap+screen+d800+bg, no displayer; loads at $6000)." />
              <Button v-else-if="options.mode === 'c64-hires'" label="hir" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadHir"
                title="Download Art Studio .hir (raw bitmap+screen, no displayer; loads at $2000)." />
            </div>
            <!-- C64 charset export: PNG + .h header + .raw bytes. -->
            <div v-if="isC64CharsetMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="h" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadHeader"
                title="Download a self-contained C header: charset[] + screen[] + color[] + palette[] + COLS / ROWS / GLYPHS / BG_COLOR (and MC1 / MC2 for multicolor)." />
              <Button label="raw" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadRaw"
                title="Download raw bytes: charset_data (unique_glyphs × 8) + screen_ram (cols × rows) + color_ram (cols × rows)." />
            </div>
            <!-- Genesis export: PNG + SGDK .h + raw .bin. -->
            <div v-if="isGenesisMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="h" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadHeader"
                title="Download an SGDK-compatible C header: tiles[] + tilemap[] + palette[] + TileSet/TileMap/Palette wrappers ready for VDP_loadTileSet / VDP_setMap / VDP_setPalette." />
              <Button label="raw" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadRaw"
                title="Download raw bytes: tile data (unique × 32) + tilemap (u16 BE per cell) + palette (4 lines × 16 BGR333 words BE)." />
            </div>
            <!-- SNES Mode 7 export: PNG + minimal .h + raw .bin. -->
            <div v-if="isSnesMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="h" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadHeader"
                title="Download a Mode 7 C header: tiles[] + tilemap[] + palette[] (256-mode) or tiles + tilemap (Direct, BBGGGRRR pixel bytes self-decode)." />
              <Button label="raw" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadRaw"
                title="Download raw bytes: 16 KB tilemap (128×128, u8 tile index) + tile data (unique × 64) + 256×3-byte sRGB palette (256 mode only)." />
            </div>
            <!-- Game Boy Advance export: PNG + devkitARM-style .h header.
                 mode4's single .h carries both indices and the BGR555
                 palette, so PNG + .h is sufficient in the browser. -->
            <div v-if="isGbaMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="h" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadHeader"
                title="Download a GBA C header: Bitmap[] of BGR555 words (Mode 3/5) or 8bpp indices + Pal[256] (Mode 4), with Width / Height / BitmapLen defines." />
            </div>
            <!-- Thomson / TED export: PNG preview + generic .h header +
                 native-layout .bin (no IFF / viewer — not Amiga/DOS). -->
            <div v-if="isThomsonMode(options.mode) || isTedMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="h" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadHeader"
                title="Download a generic C header: Thomson Couleur/Forme or PageA/PageB (+ 4-bit r/g/b Palette for TO8); TED Bitmap/Luma/Chroma (+ Bg0/Bg1 for multicolor)." />
              <Button label="raw" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadRaw"
                title="Download native-layout raw bytes (Thomson: pageA then pageB; TED: bitmap, luma, chroma, then globals)." />
            </div>
            <!-- Amiga export buttons -->
            <div v-if="!isAtariMode(options.mode) && !isDosMode(options.mode) && !isSnesMode(options.mode) && !isGenesisMode(options.mode) && !isC64Mode(options.mode) && !isGbaMode(options.mode) && !isThomsonMode(options.mode) && !isTedMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button label="iff" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadIFF"
                :title="options.copper
                  ? 'Download as IFF ILBM with standard PCHG chunk for per-line palette (readable by Recoil, ViewTek, OS3.5+ ilbm.datatype).'
                  : 'Download as IFF ILBM (Deluxe Paint, Personal Paint, WinUAE compatible).'" />
              <Button label="adf" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="compileAndDownload('adf')"
                title="Download bootable Amiga floppy disk image (ADF)." />
            </div>
            <div v-if="!isAtariMode(options.mode) && !isDosMode(options.mode) && !isSnesMode(options.mode) && !isGenesisMode(options.mode) && !isC64Mode(options.mode) && !isGbaMode(options.mode) && !isThomsonMode(options.mode) && !isTedMode(options.mode)" class="flex gap-2">
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
              <!-- Custom palette: HAM has a dynamic per-pixel palette, and
                   supportsCustomPalette() covers the targets where an uploaded
                   palette is either ignored by the encoder (C64, CGA text,
                   SNES, Genesis) or rejected outright (Thomson, TED, GBA
                   direct). -->
              <div v-if="!isHamMode(options.mode) && supportsCustomPalette(options.mode)">
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
                  <input type="checkbox" v-model="options.lockColor0" id="lockColor0" />
                  <label for="lockColor0" class="text-xs text-color-secondary" title="Reserve palette index 0 for black (Amiga border/background color). Disable to use all palette slots for image colors.">Reserve color 0 for black</label>
                </div>
              </div>

              <!-- Reserve palette: 16xN grid; click to reserve/unreserve
                   individual slots. Reserved slots are removed from the
                   dither candidate set — the encoder never routes image
                   pixels through them, but the slot keeps its color for
                   display (CMAP / runtime). Visible only when the encoder
                   emits a global swatch palette (HAM / sliced / strips
                   have none). -->
              <div v-if="numReserveRows > 0" class="pt-3 mt-3 border-top-1 surface-border">
                <label class="block text-xs text-color-secondary font-semibold mb-1"
                       title="Click a swatch to reserve that palette slot. Reserved slots stay in the palette but the encoder won't dither image pixels into them — useful for sprite colors, runtime palette regions, EHB upper-bank carve-outs, etc. Click again to unreserve.">Reserve palette</label>
                <div class="lock-grid" :style="{ gridTemplateRows: `1rem repeat(${numReserveRows}, 0.94rem)` }">
                  <template v-for="item in reserveGridItems" :key="item.key">
                    <div v-if="item.kind === 'corner'"></div>
                    <div v-else-if="item.kind === 'col-label'" class="lock-axis">{{ item.text }}</div>
                    <div v-else-if="item.kind === 'row-label'" class="lock-axis lock-axis-left">{{ item.text }}</div>
                    <div v-else-if="item.kind === 'swatch' && item.idx >= 0 && item.readonly"
                         class="lock-cell lock-cell-readonly"
                         :style="{ background: reserveCellBg(item.idx) }"
                         :title="'Slot 0 is locked to black via &quot;Reserve color 0 for black&quot; — uncheck that to make slot 0 reservable.'"></div>
                    <div v-else-if="item.kind === 'swatch' && item.idx >= 0"
                         class="lock-cell"
                         role="button"
                         tabindex="0"
                         :aria-pressed="isReserved(item.idx)"
                         :aria-label="`Reserve palette index ${item.idx}`"
                         :style="{ background: isReserved(item.idx) ? 'transparent' : reserveCellBg(item.idx) }"
                         @mousedown="reserveCellDown(item.idx, $event)"
                         @mouseenter="reserveCellEnter(item.idx)"
                         @focus="reserveCellEnter(item.idx)"
                         @keydown.enter.prevent="toggleReserve(item.idx)"
                         @keydown.space.prevent="toggleReserve(item.idx)">
                      <svg v-if="isReserved(item.idx)"
                           viewBox="0 0 10 10" class="lock-x">
                        <line x1="1.5" y1="1.5" x2="8.5" y2="8.5"
                              stroke="#000" stroke-width="3" stroke-linecap="round" />
                        <line x1="8.5" y1="1.5" x2="1.5" y2="8.5"
                              stroke="#000" stroke-width="3" stroke-linecap="round" />
                        <line x1="1.5" y1="1.5" x2="8.5" y2="8.5"
                              stroke="#e22" stroke-width="1.5" stroke-linecap="round" />
                        <line x1="8.5" y1="1.5" x2="1.5" y2="8.5"
                              stroke="#e22" stroke-width="1.5" stroke-linecap="round" />
                      </svg>
                    </div>
                    <div v-else class="lock-cell lock-cell-disabled"></div>
                  </template>
                </div>
              </div>

              <!-- sliced changes override -->
              <div v-if="options.copper" class="pt-3 mt-3 border-top-1 surface-border">
                <label class="block text-xs text-color-secondary font-semibold mb-1" title="Sliced palette swaps per scanline. 0 = auto: backend picks the worst-case K that fits the 14-MOVE budget, plus a K+3 retry path. Higher values bypass the budget check and may overshoot real hardware.">Slice changes/line</label>
                <div class="flex gap-2 align-items-center">
                  <InputNumber v-model="options.copperChanges" :min="0" :max="copperMax" class="flex-1 input-sm" placeholder="0 = auto" />
                  <span class="text-xs text-color-secondary">max: {{ copperMax }}</span>
                </div>
                <div class="flex align-items-center gap-2 mt-2">
                  <input type="checkbox" v-model="options.slicedVerticalDither" id="slicedVerticalDither" />
                  <label for="slicedVerticalDither" class="text-xs text-color-secondary" title="Spread copper transitions across rows via 1-D Bayer alternation between old/new palette colors. Smoother on a CRT, slightly worse S2/PSNR.">Vertical palette dither</label>
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
            <div class="canvas-wrap flex flex-row align-items-start"
                 :style="loupeActive ? { transform: `scale(4) translate(${loupeX/4}px, ${loupeY/4}px)`, transformOrigin: '0 0' } : {}">
              <canvas v-show="hasScanlinePalette && paletteViewActive" ref="scanlinePaletteCanvasRef"
                      class="scanline-palette-strip"
                      style="margin-right: 8px;"
                      title="Per-scanline base palette — one column per slot, one row per scanline." />
              <div class="preview-stack relative">
              <canvas ref="canvasRef" class="preview-canvas" v-show="!crtEnabled" />
              <canvas ref="crtCanvasRef" class="preview-canvas" v-show="crtEnabled" />
              <div v-if="converting" class="overlay flex flex-column align-items-center justify-content-center" style="gap: 0.5rem">
                <ProgressSpinner v-if="!progress" style="width: 2rem; height: 2rem" />
                <div v-else style="width: 70%; max-width: 22rem; text-align: center;">
                  <div class="flex align-items-center gap-2">
                    <ProgressBar :value="progress" :show-value="true" style="height: 1.5rem; flex: 1 1 auto" />
                    <button
                      class="stop-btn"
                      type="button"
                      title="Stop encode (terminates the WASM worker)"
                      @click.stop="onStopEncode"
                    >
                      <i class="pi pi-times"></i>
                    </button>
                  </div>
                  <div class="text-xs text-color-secondary mt-1" v-if="progressStage">{{ progressStage }}</div>
                </div>
              </div>
              </div>
            </div>
            <button class="loupe-btn" :class="{ active: loupeActive }" @click.stop="loupeToggle" title="Toggle 4x zoom">
              <i class="pi pi-search"></i>
            </button>
            <button class="loupe-btn palette-btn"
                    :class="{ active: paletteViewActive }"
                    :disabled="!lastPaletteBytes"
                    @click.stop="paletteToggle"
                    title="Show the rendered image's palette as a swatch grid">
              <i class="pi pi-palette"></i>
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
          <!-- Palette swatch grid: 8×8 swatches at 50% scale (16 CSS
               px per swatch), ≤ 32 per row, drawn from
               result.paletteBytes for any mode whose result carries a
               single global palette. Sits above the tile / charset
               diagnostic cards so it's the first thing the user sees
               when toggled on. Toggled by the palette button next to
               zoom/CRT. Hover shows index, hex, and RGB. role=img
               keeps the linter happy on the static canvas with an
               interactive listener. -->
          <!-- eslint-disable vuejs-accessibility/no-static-element-interactions, vuejs-accessibility/mouse-events-have-key-events -->
          <div v-if="paletteViewActive && lastPaletteBytes"
               class="surface-card border-round-lg p-2 palette-card"
               @mousemove="paletteHover"
               @mouseleave="paletteHoverLeave"
               @blur="paletteHoverLeave">
            <canvas ref="paletteCanvasRef" class="palette-canvas"
                    role="img"
                    aria-label="Palette swatches; hover for index and color values" />
          </div>
          <!-- eslint-enable -->
          <!-- Charset diagnostic: actual generated glyphs, colored by
               each glyph's first-occurrence cell. Only for c64-charset. -->
          <div v-if="isC64CharsetMode(options.mode)"
               class="surface-card border-round-lg p-2">
            <canvas ref="charsetCanvasRef" class="charset-canvas" />
          </div>
          <div v-if="isGenesisMode(options.mode)"
               class="surface-card border-round-lg p-2">
            <canvas ref="genesisTilesCanvasRef" class="charset-canvas" />
          </div>
          <div v-if="isSnesMode(options.mode)"
               class="surface-card border-round-lg p-2">
            <canvas ref="snesTilesCanvasRef" class="charset-canvas" />
          </div>
          <!-- Floating tooltip that tracks the cursor across palette
               swatches. Fixed-position so it stays under the mouse
               regardless of scroll. -->
          <div v-if="paletteTooltip.visible"
               class="palette-tooltip"
               :style="{ left: paletteTooltip.x + 'px',
                         top: paletteTooltip.y + 'px' }">
            <span class="palette-tooltip-swatch"
                  :style="{ background: paletteTooltip.swatch }" />
            <span>{{ paletteTooltip.text }}</span>
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
/* PrimeVue caps the list at ~200px which clipped the 8-entry chipset
   dropdown (Amiga OCS / AGA / Atari STF / STE / VGA / EGA / CGA / SNES).
   Bump the cap so the full list shows without scrolling. */
.p-select-overlay .p-select-list,
.p-select-overlay .p-select-list-container {
  max-height: 360px !important;
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

/* Layout priority: controls keep a hard floor; preview shrinks first.
   Without this, PrimeFlex's % grid splits both columns proportionally
   and the controls column squeezes its labels long before the preview
   has run out of slack. */
.controls-col {
  min-width: 22rem;
  flex: 0 0 auto;
}
/* Tighten the label column inside the controls panel: PrimeFlex's
   .col-4 reserves 33.3333% which leaves a noticeable gap after short
   labels (Sliced, Strips, Dither, Dual PF). Override to 25% / 75% so every
   row stays aligned but the gap shrinks by ~22px. */
.controls-col .grid.align-items-center > label.col-4 {
  width: 25%;
}
.controls-col .grid.align-items-center > .col-8 {
  width: 75%;
}
.preview-col {
  position: sticky;
  top: 1rem;
  align-self: start;
  /* Take whatever's left after the controls column claims its
     min-width. flex-basis: 0 + flex-grow: 1 means the preview
     shrinks first when the viewport narrows. */
  flex: 1 1 0;
  min-width: 0;
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
.loupe-btn.palette-btn {
  /* Sit immediately to the left of the loupe button. */
  right: 2.4rem;
}
.loupe-btn.crt-btn {
  /* Sit two slots to the left of the loupe button (past the palette
     button when present). */
  right: 4.4rem;
}
.loupe-btn:disabled {
  cursor: not-allowed;
  opacity: 0.4;
}
.palette-canvas {
  image-rendering: pixelated;
  display: block;
}
.palette-tooltip {
  position: fixed;
  z-index: 1000;
  pointer-events: none;
  background: rgba(0, 0, 0, 0.85);
  color: #fff;
  padding: 0.25rem 0.5rem;
  border-radius: 4px;
  font-size: 0.75rem;
  font-family: ui-monospace, SFMono-Regular, monospace;
  display: flex;
  align-items: center;
  gap: 0.4rem;
  white-space: nowrap;
  box-shadow: 0 2px 6px rgba(0, 0, 0, 0.5);
}
.palette-tooltip-swatch {
  display: inline-block;
  width: 0.9rem;
  height: 0.9rem;
  border-radius: 2px;
  border: 1px solid rgba(255, 255, 255, 0.3);
}

/* PrimeVue's Aura ProgressBar defaults to a 1s ease-in-out CSS
   `transition: width 1s` on .p-progressbar-value. Encoder progress
   ticks fire ~once per cell, so the visible fill always lags the
   percentage label (you can see "50%" while the bar is still around
   ~10% wide). Override with a much shorter transition so the fill
   tracks the value updates in real time. */
:deep(.p-progressbar-determinate .p-progressbar-value) {
  transition: width 0.12s linear !important;
}

.stop-btn {
  flex: 0 0 auto;
  width: 1.75rem;
  height: 1.5rem;
  border: none;
  border-radius: 4px;
  background: rgba(220, 50, 50, 0.85);
  color: #fff;
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 0.7rem;
  transition: background 0.15s;
}
.stop-btn:hover {
  background: rgba(200, 30, 30, 1);
}

.lock-grid {
  display: grid;
  grid-template-columns: 1rem repeat(16, 0.94rem);
  gap: 1px;
  font-family: ui-monospace, monospace;
  font-size: 0.6rem;
  line-height: 1;
  user-select: none;
}
.lock-axis {
  color: #888;
  text-align: center;
  align-self: center;
}
.lock-axis-left { text-align: right; padding-right: 2px; }
.lock-cell {
  position: relative;
  width: 0.94rem;
  height: 0.94rem;
  border: 1px solid rgba(0, 0, 0, 0.25);
  cursor: pointer;
  box-sizing: border-box;
}
.lock-cell:hover { outline: 1px solid #fff; }
.lock-cell-disabled {
  background: transparent !important;
  border: 1px dashed rgba(255, 255, 255, 0.1);
  cursor: default;
}
.lock-cell-readonly {
  cursor: not-allowed;
  border: 1px dashed rgba(255, 255, 255, 0.4);
  opacity: 0.6;
}
.lock-cell-readonly:hover { outline: none; }
.lock-x {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
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

.scanline-palette-strip {
  display: block;
  image-rendering: pixelated;
  image-rendering: crisp-edges;
  border: 1px solid var(--surface-border, #444);
  flex: 0 0 auto;
}

.overlay {
  position: absolute;
  inset: 0;
  background: rgba(0, 0, 0, 0.5);
}
</style>
