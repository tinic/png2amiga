<script setup>
import { ref, reactive, watch, nextTick, computed, onBeforeUnmount } from 'vue'
import { useWasm } from '../composables/useWasm.js'
import { useImageUpload } from '../composables/useImageUpload.js'
import { track } from '../lib/analytics.js'
import {
  MODES, CHIPSETS, DITHER_METHODS, ALPHA_DITHER_METHODS, HAM_QUALITY,
  SLIDERS, DIFFUSION_SLIDERS, EXAMPLES,
  defaultOptions, isHamMode, hamType, isEhbMode, isAtariMode, isErrorDiffusion, isInterlaceMode,
  maxDepth, defaultDepth, effectiveChipset, previewScale,
  modesForChipset, decomposeMode,
} from '../lib/options.js'

import InputNumber from 'primevue/inputnumber'
import Select from 'primevue/select'
import Slider from 'primevue/slider'
import ToggleSwitch from 'primevue/toggleswitch'
import Button from 'primevue/button'
import ProgressSpinner from 'primevue/progressspinner'
import Panel from 'primevue/panel'

const { loading: wasmLoading, error: wasmError, convertRGBA, convertPNG, convertIFF, convertHeader, convertViewer, convertDegas, convertRaw, convertMask, convertMaskRaw } = useWasm()
const { imageBytes, imageName, imageUrl, imageWidth, imageHeight, dragOver, uploadTimestamp, onDrop, onDragOver, onDragLeave, openPicker } = useImageUpload()

const showUploadHint = ref(true)

// Load first example by default once WASM is ready
watch(wasmLoading, (loading) => {
  if (!loading && !wasmError.value && !imageBytes.value) {
    const example = EXAMPLES[0]
    fetch(`/examples/${example.file}`)
      .then(r => r.arrayBuffer())
      .then(buf => {
        imageBytes.value = new Uint8Array(buf)
        imageName.value = example.file
        const type = example.file.endsWith('.jpg') || example.file.endsWith('.jpeg') ? 'image/jpeg' : 'image/png'
        const blob = new Blob([buf], { type })
        imageUrl.value = URL.createObjectURL(blob)
        // Decode dimensions so resize presets work for the default example.
        const img = new Image()
        img.onload = () => {
          imageWidth.value = img.width
          imageHeight.value = img.height
        }
        img.src = imageUrl.value
      })
  }
})

const options = reactive(defaultOptions())
const canvasRef = ref(null)
const converting = ref(false)
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
const paletteData = ref(null)    // raw palette file bytes (Uint8Array)
const paletteColors = ref([])    // parsed CSS color strings for preview
const pageLoadTime = Date.now()

// Loupe (4x zoom with drag-to-pan)
const loupeActive = ref(false)
const loupeX = ref(0)  // pan offset in CSS pixels (negative = scrolled right/down)
const loupeY = ref(0)
const loupeHeight = ref(null)
const previewColRef = ref(null)
let dragStart = null    // { x, y, ox, oy } while dragging

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
function loupePointerDown(e) {
  if (!loupeActive.value) return
  if (e.target.closest('.loupe-btn')) return
  dragStart = { x: e.clientX, y: e.clientY, ox: loupeX.value, oy: loupeY.value }
  e.currentTarget.setPointerCapture(e.pointerId)
}
function loupePointerMove(e) {
  if (!dragStart) return
  const canvas = canvasRef.value
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
// HAM8 has 6-bit modify channels (~lossless), so error diffusion is useless.
const groupedDitherOptions = computed(() => {
  const hide_diffusion = hamType(options.mode) === 'ham8'
  return DITHER_METHODS
    .filter(g => !(hide_diffusion && g.group === 'Error Diffusion'))
    .map(g => ({
      label: g.group,
      items: g.items.map(d => ({ value: d.value, label: d.label }))
    }))
})

// Flat list of all dither values for prev/next cycling
const allDitherValues = computed(() =>
  groupedDitherOptions.value.flatMap(g => g.items.map(d => d.value))
)

function cycleDither(dir) {
  const vals = allDitherValues.value
  const idx = vals.indexOf(options.dither)
  const next = (idx + dir + vals.length) % vals.length
  options.dither = vals[next]
}

// Whether depth slider should be shown (not for HAM/EHB/Atari where depth is fixed)
const showDepthSlider = computed(() => {
  return !isHamMode(options.mode) && !isEhbMode(options.mode) && !isAtariMode(options.mode)
})

// Raw export tooltip with format layout (HTML for fixed-width font).
// SAFETY: PrimeVue's v-tooltip below uses escape:false so this string is
// inserted as raw HTML. Every interpolation MUST be numeric/bool and is
// coerced via Number() / Boolean() below so a future regression that
// smuggles a string with HTML special characters can't turn into XSS.
const rawTooltipHtml = computed(() => {
  const n = (v) => Number(v) || 0
  const w = n(lastWidth.value)
  const h = n(lastHeight.value)
  const d = n(options.depth || defaultDepth(options.mode))
  const dd = n(defaultDepth(options.mode))
  const bpr = Math.ceil(w / 16) * 2
  // Use the actual chipset reported by the encoder, not the user's option,
  // because mode-driven chipset (e.g. HAM7/8 force AGA) may override.
  const aga = Boolean(lastAga.value)
  const colors = 1 << (isHamMode(options.mode) ? dd - 2 : d)
  const pb = n(lastPlaneBytes.value)
  const cb = n(lastCopperBytes.value)
  const palSize = colors * 2
  // .raw uses fixed [h][cpl] grid with sentinels for unused/skipped slots.
  // copPerPass = h * cpl * 4. cpl reported by the encoder (post auto-stretch).
  const cpl = n(lastChangesPerLine.value)
  const copPerPass = aga && cb ? cb / 2 : cb
  let off = 0
  let lines = []
  lines.push(`Raw binary format (big-endian):`)
  lines.push(``)
  lines.push(`Offset  Size     Content`)
  lines.push(`------  -------  ----------------------------`)
  lines.push(`0x${off.toString(16).padStart(4,'0')}  ${pb.toLocaleString().padStart(7)}  Bitplanes`)
  lines.push(`                 (${d}bpl, ${bpr}B/row, interleaved)`)
  off += pb
  lines.push(`0x${off.toString(16).padStart(4,'0')}  ${palSize.toLocaleString().padStart(7)}  Palette${aga ? ' hi' : ''}`)
  lines.push(`                 (${colors} * u16, ${aga ? 'hi nibbles 0x0RGB' : '0x0RGB'})`)
  off += palSize
  if (aga) {
    lines.push(`0x${off.toString(16).padStart(4,'0')}  ${palSize.toLocaleString().padStart(7)}  Palette lo`)
    lines.push(`                 (${colors} * u16, lo nibbles 0x0RGB)`)
    off += palSize
  }
  if (cb > 0) {
    lines.push(`0x${off.toString(16).padStart(4,'0')}  ${copPerPass.toLocaleString().padStart(7)}  Copper${aga ? ' hi' : ''}`)
    lines.push(`                 ((u8:0+u8:reg+u16:col) * ${cpl}/line, ${h} lines)`)
    off += copPerPass
    if (aga) {
      lines.push(`0x${off.toString(16).padStart(4,'0')}  ${copPerPass.toLocaleString().padStart(7)}  Copper lo`)
      lines.push(`                 ((u8:0+u8:reg+u16:col) * ${cpl}/line, ${h} lines)`)
      off += copPerPass
    }
  }
  lines.push(`------  -------  ----------------------------`)
  lines.push(`Total:  ${off.toLocaleString().padStart(7)}  ${w}x${h}, ${d}bpl, ${aga ? 'AGA 24-bit' : 'OCS 12-bit'}`)
  return `<pre style="margin:0;font-size:0.7rem;line-height:1.3;white-space:pre">${lines.join('\n')}</pre>`
})

// Whether HAM controls should be shown
const showHamControls = computed(() => isHamMode(options.mode))

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

// Update depth when mode changes — only clamp, don't reset
watch(() => options.mode, (mode, oldMode) => {
  const max = maxDepth(mode, options.chipset)
  if (max === 0) {
    // Fixed depth modes (HAM, EHB) — use their default
    options.depth = defaultDepth(mode)
  } else if (options.depth > max) {
    options.depth = max
  }
  // Copper not compatible with interlace
  if (isInterlaceMode(mode)) options.copper = false
  // HAM8: error diffusion is useless (6-bit ≈ lossless), reset to ordered/none
  if (hamType(mode) === 'ham8' && isErrorDiffusion(options.dither))
    options.dither = 'none'
  track('mode-change', { from: oldMode, to: mode })
})

// When chipset changes, reset mode if current mode isn't available
watch(() => options.chipset, (chipset, oldChipset) => {
  track('chipset-change', { from: oldChipset, to: chipset })
  const modes = modesForChipset(options.chipset)
  if (!modes.find(m => m.value === options.mode)) {
    options.mode = modes[0].value
    options.depth = defaultDepth(options.mode)
  }
  if (isAtariMode(options.mode)) options.copper = false
  const max = maxDepth(options.mode, options.chipset)
  if (max > 0 && options.depth > max) {
    options.depth = max
  }
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
function setSizePreset(scale) {
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

// Track slider tweaks (debounced)
let tweakTimer = null
for (const s of [...SLIDERS, ...DIFFUSION_SLIDERS]) {
  watch(() => options[s.key], (val) => {
    clearTimeout(tweakTimer)
    tweakTimer = setTimeout(() => track('setting-tweak', { key: s.key, value: val }), 500)
  })
}

// Session duration on page unload
onBeforeUnmount(() => {
  track('session-duration', { seconds: Math.round((Date.now() - pageLoadTime) / 1000) })
})
if (typeof window !== 'undefined') {
  window.addEventListener('beforeunload', () => {
    track('session-duration', { seconds: Math.round((Date.now() - pageLoadTime) / 1000) })
  })
}

let debounceTimer = null
let spinnerTimer = null

function doConvert() {
  if (!imageBytes.value || wasmLoading.value) return

  clearTimeout(debounceTimer)
  debounceTimer = setTimeout(async () => {
    errorMsg.value = ''
    const convertStart = performance.now()

    // Show spinner only if conversion takes longer than 100ms
    clearTimeout(spinnerTimer)
    spinnerTimer = setTimeout(() => { converting.value = true }, 100)

    try {
      const result = await convertRGBA(imageBytes.value, buildWasmOptions())

      clearTimeout(spinnerTimer)

      if (result.error) {
        errorMsg.value = result.error
        track('error', { type: 'convert', message: result.error, mode: options.mode })
        converting.value = false
        return
      }

      const canvas = canvasRef.value
      if (!canvas) { converting.value = false; return }

      const { sx, sy } = previewScale(options.mode)
      const dw = result.width * sx
      const dh = result.height * sy
      canvas.width = dw
      canvas.height = dh
      canvas.style.width = `${dw}px`
      canvas.style.height = `${dh}px`

      const ctx = canvas.getContext('2d')
      ctx.imageSmoothingEnabled = false
      const tmp = document.createElement('canvas')
      tmp.width = result.width
      tmp.height = result.height
      const tmpCtx = tmp.getContext('2d')
      const imgData = new ImageData(
        new Uint8ClampedArray(result.rgba),
        result.width, result.height
      )
      tmpCtx.putImageData(imgData, 0, 0)
      ctx.drawImage(tmp, 0, 0, dw, dh)

      lastWidth.value = result.width
      lastHeight.value = result.height
      lastCopPerLine.value = result.copperChanges || 0
      lastPlaneBytes.value = result.planeBytes || 0
      lastCopperBytes.value = result.copperBytes || 0
      lastChangesPerLine.value = result.changesPerLine || 0
      lastMaxMovesPerLine.value = result.maxMovesPerLine || 0
      lastAga.value = !!result.aga
      imageHasAlpha.value = !!result.hasTransparency

      // If we're in size-override mode but width/height got reset to 0
      // (a new image just loaded), populate the inputs with the freshly
      // computed natural defaults for the new image. Triggers one more
      // (idempotent) convert via the deep options watcher.
      if (sizeOverride.value && (!options.width || !options.height)) {
        options.width = result.width
        options.height = result.height
      }

      let info = `${result.width}x${result.height}, ${statusChipset.value}`
      info += `, ${result.depth || '?'}bpl, ${result.totalColors || result.colors || 0} colors`
      if (result.copperChanges) info += `, ${result.copperChanges.toFixed(1)} avg CAP/line`
      // Size stats
      const pb = result.planeBytes || 0
      const cb = result.copperBytes || 0  // per-scanline copper change data in .raw output
      const depth = result.depth || 0
      const colors = result.colors || 0
      const aga = !!result.aga
      const maxMoves = result.maxMovesPerLine || 0

      // Disk: bitplanes + palette (AGA has hi+lo) + copper change data
      // .raw layout: pb + colors*2 (hi pal) + colors*2 (lo pal, AGA only) + cb
      const palBytes = colors * (aga ? 4 : 2)
      const diskBytes = pb + palBytes + cb

      // Chip RAM: bitplanes + actual copper list
      //   - Initial setup: ~80 bytes (BPLCON0/1/2/3 + DDF + DIW + FMODE)
      //   - Per-frame palette write: 1 instruction per color (OCS) or 2 (AGA hi+lo)
      //   - Per-scanline copper writes: WAIT + max_moves instructions per line, ×4 bytes
      //     (uses worst-case max_moves_per_line — actual is slightly less on most lines)
      //   - End markers + blank-below: ~32 bytes
      const initSetup = 80
      const palSetup = colors * (aga ? 8 : 4)
      const perLineCopper = (1 + maxMoves) * 4
      const copList = cb ? result.height * perLineCopper : 0
      const chipBytes = pb + initSetup + palSetup + copList + 32
      if (pb > 0) {
        const fmt = (b) => b >= 1024 ? `${(b / 1024).toFixed(1)}K` : `${b}B`
        info += `, disk: ${fmt(diskBytes)}, chip: ${fmt(chipBytes)}`
      }
      if (result.quantError != null) info += `, error: ${result.quantError.toFixed(2)}`
      resultInfo.value = info

      const convertMs = performance.now() - convertStart
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
    } catch (e) {
      clearTimeout(spinnerTimer)
      errorMsg.value = e.message
      track('error', { type: 'convert-exception', message: e.message })
    }

    converting.value = false
  }, 150)
}

watch([imageBytes, () => ({ ...options })], doConvert, { deep: true })

async function downloadPNG() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertPNG(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    const blob = new Blob([result.data], { type: 'image/png' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '-amiga.png'
    a.click()
    URL.revokeObjectURL(url)
    exportCount++
    track('export', { format: 'png', mode: options.mode, exportCount, secsSinceUpload: uploadTimestamp.value ? Math.round((Date.now() - uploadTimestamp.value) / 1000) : undefined })
  } catch (e) { errorMsg.value = e.message }
  converting.value = false
}

async function downloadDegas() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertDegas(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    const ext = options.mode.endsWith('-hi') ? '.pi3'
               : options.mode.endsWith('-med') ? '.pi2' : '.pi1'
    const blob = new Blob([result.data], { type: 'application/octet-stream' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + ext
    a.click()
    URL.revokeObjectURL(url)
    exportCount++
    track('export', { format: ext, mode: options.mode, exportCount })
  } catch (e) { errorMsg.value = e.message }
  converting.value = false
}

async function downloadIFF() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertIFF(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    const blob = new Blob([result.data], { type: 'application/octet-stream' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '.iff'
    a.click()
    URL.revokeObjectURL(url)
    exportCount++
    track('export', { format: 'iff', mode: options.mode, exportCount })
  } catch (e) { errorMsg.value = e.message }
  converting.value = false
}

async function downloadViewer() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const stem = options.symbolName ||
      (imageName.value || 'image').replace(/\.[^.]+$/, '').replace(/[^a-zA-Z0-9_]/g, '_')
    const opts = buildWasmOptions()
    opts.symbolName = stem
    const result = await convertViewer(imageBytes.value, opts)
    if (result.error) { errorMsg.value = result.error; return }
    const blob = new Blob([result.header || result.data], { type: 'text/plain' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = stem + '.cpp'
    a.click()
    URL.revokeObjectURL(url)
    exportCount++
    track('export', { format: 'cpp', mode: options.mode, exportCount })
  } catch (e) { errorMsg.value = e.message }
  converting.value = false
}

async function downloadRaw() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const result = await convertRaw(imageBytes.value, buildWasmOptions())
    if (result.error) { errorMsg.value = result.error; return }
    const blob = new Blob([result.data], { type: 'application/octet-stream' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '.raw'
    a.click()
    URL.revokeObjectURL(url)
    exportCount++
    track('export', { format: 'raw', mode: options.mode, exportCount })
  } catch (e) { errorMsg.value = e.message }
  converting.value = false
}

async function downloadMaskPNG() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const opts = buildWasmOptions()
    opts.maskInvert = !!options.maskInvert
    const result = await convertMask(imageBytes.value, opts)
    if (result.error) { errorMsg.value = result.error; return }
    const blob = new Blob([result.data], { type: 'image/png' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '-mask.png'
    a.click()
    URL.revokeObjectURL(url)
    exportCount++
    track('export', { format: 'mask-png', mode: options.mode, exportCount })
  } catch (e) { errorMsg.value = e.message }
  converting.value = false
}

async function downloadMaskRaw() {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const opts = buildWasmOptions()
    opts.maskInvert = !!options.maskInvert
    const result = await convertMaskRaw(imageBytes.value, opts)
    if (result.error) { errorMsg.value = result.error; return }
    const blob = new Blob([result.data], { type: 'application/octet-stream' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '-mask.raw'
    a.click()
    URL.revokeObjectURL(url)
    exportCount++
    track('export', { format: 'mask-raw', mode: options.mode, exportCount })
  } catch (e) { errorMsg.value = e.message }
  converting.value = false
}

async function compileAndDownload(format) {
  if (!imageBytes.value) return
  converting.value = true
  try {
    const stem = options.symbolName ||
      (imageName.value || 'image').replace(/\.[^.]+$/, '').replace(/[^a-zA-Z0-9_]/g, '_')
    const opts = buildWasmOptions()
    opts.symbolName = stem
    const result = await convertViewer(imageBytes.value, opts)
    if (result.error) { errorMsg.value = result.error; return }
    const source = result.header || new TextDecoder().decode(result.data)
    const resp = await fetch(`/api/compile?format=${format}`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: source,
    })
    if (!resp.ok) {
      errorMsg.value = await resp.text()
      return
    }
    const blob = await resp.blob()
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '.' + format
    a.click()
    URL.revokeObjectURL(url)
    exportCount++
    track('export', { format, mode: options.mode, exportCount })
  } catch (e) { errorMsg.value = e.message }
  converting.value = false
}

function resetOptions() {
  Object.assign(options, defaultOptions())
  clearPalette()
  track('reset')
}

function loadPalette() {
  const input = document.createElement('input')
  input.type = 'file'
  input.accept = '.gpl,.hex,.txt,.pal,.iff,.ilbm,.lbm'
  input.onchange = async () => {
    const file = input.files[0]
    if (!file) return
    const buf = await file.arrayBuffer()
    const bytes = new Uint8Array(buf)
    paletteData.value = bytes
    options.copper = false  // copper not compatible with custom palette
    // Parse colors for preview
    const text = new TextDecoder().decode(bytes)
    const colors = []
    if (text.startsWith('GIMP Palette')) {
      // GPL format
      for (const line of text.split('\n')) {
        const m = line.match(/^\s*(\d+)\s+(\d+)\s+(\d+)/)
        if (m) colors.push(`rgb(${m[1]},${m[2]},${m[3]})`)
      }
    } else if (bytes.length >= 12 && String.fromCharCode(...bytes.slice(0, 4)) === 'FORM') {
      // IFF: scan for CMAP chunk
      let pos = 12
      while (pos + 8 <= bytes.length) {
        const id = String.fromCharCode(...bytes.slice(pos, pos + 4))
        const size = (bytes[pos+4] << 24) | (bytes[pos+5] << 16) | (bytes[pos+6] << 8) | bytes[pos+7]
        pos += 8
        if (id === 'CMAP') {
          for (let i = 0; i + 2 < size && pos + i + 2 <= bytes.length; i += 3)
            colors.push(`rgb(${bytes[pos+i]},${bytes[pos+i+1]},${bytes[pos+i+2]})`)
          break
        }
        pos += size + (size & 1)  // chunks are word-aligned
      }
    } else {
      // Try hex text (one RRGGBB per line)
      for (const line of text.split('\n')) {
        const m = line.trim().match(/^#?([0-9a-fA-F]{6})$/)
        if (m) colors.push(`#${m[1]}`)
      }
    }
    if (colors.length === 0) {
      // Binary .pal: 2 bytes per color, big-endian 0x0RGB
      for (let i = 0; i + 1 < bytes.length; i += 2) {
        const w = (bytes[i] << 8) | bytes[i + 1]
        const r = ((w >> 8) & 0xF) * 17
        const g = ((w >> 4) & 0xF) * 17
        const b = (w & 0xF) * 17
        colors.push(`rgb(${r},${g},${b})`)
      }
    }
    paletteColors.value = colors
    // Pass raw file bytes to WASM for auto-format detection
    options.paletteData = bytes
  }
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

async function loadExample(example) {
  dismissHint()
  track('example', { name: example.name })
  // Reset to defaults, then apply example-specific settings
  Object.assign(options, defaultOptions())
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
                    @dragleave="onDragLeave($event)"
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

              <div class="grid align-items-center">
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

              <!-- Copper-Augmented Palette (not available for interlace or Atari) -->
              <div v-if="!isInterlaceMode(options.mode) && !isAtariMode(options.mode) && !paletteData" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Copper-Augmented Palette: per-scanline palette swaps via the copper, picked greedily by OKLab error reduction. Each row gets its own per-line variant of the base palette.">Copper</label>
                <div class="col-8 flex align-items-center gap-2">
                  <ToggleSwitch v-model="options.copper" />
                  <span style="color: #888; font-size: 0.625rem;">Copper-Augmented Palette</span>
                </div>
              </div>

              <!-- HAM options (inline) -->
              <template v-if="showHamControls">
                <div class="grid align-items-center">
                  <label class="col-4 text-xs text-color-secondary font-semibold" title="HAM quality: fast = greedy per-pixel, optimal = DP beam search for minimum perceptual error.">Quality</label>
                  <div class="col-8">
                    <Select v-model="options.hamQuality" :options="HAM_QUALITY" optionValue="value" optionLabel="label" class="w-full" />
                  </div>
                </div>

                <div v-if="options.hamQuality === 'optimal'" class="grid align-items-center">
                  <label class="col-4 text-xs text-color-secondary font-semibold" title="Beam width for DP search. Higher = better quality, slower. Range 1-256.">Beam</label>
                  <div class="col-5">
                    <Slider v-model="options.hamBeam" :min="1" :max="256" :step="1" class="w-full" />
                  </div>
                  <div class="col-3">
                    <InputNumber v-model="options.hamBeam" :min="1" :max="256" :step="1" class="w-full input-sm" />
                  </div>
                </div>
              </template>

              <!-- Resize override (not for Atari — fixed resolution) -->
              <div v-if="!isAtariMode(options.mode)" class="grid align-items-center">
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
            <!-- Amiga export buttons -->
            <div v-if="!isAtariMode(options.mode)" class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download the converted image as a PNG preview file." />
              <Button v-if="!options.copper" label="iff" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadIFF"
                title="Download as IFF ILBM (Deluxe Paint, Personal Paint, WinUAE compatible)." />
              <Button label="adf" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="compileAndDownload('adf')"
                title="Download bootable Amiga floppy disk image (ADF)." />
            </div>
            <div v-if="!isAtariMode(options.mode)" class="flex gap-2">
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
              <canvas ref="canvasRef" class="preview-canvas" />
              <div v-if="converting" class="overlay flex align-items-center justify-content-center">
                <ProgressSpinner style="width: 2rem; height: 2rem" />
              </div>
            </div>
            <button class="loupe-btn" :class="{ active: loupeActive }" @click.stop="loupeToggle" title="Toggle 4x zoom">
              <i class="pi pi-search"></i>
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
