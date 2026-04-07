<script setup>
import { ref, reactive, watch, nextTick, computed } from 'vue'
import { useWasm } from '../composables/useWasm.js'
import { useImageUpload } from '../composables/useImageUpload.js'
import {
  MODES, CHIPSETS, DITHER_METHODS, ALPHA_DITHER_METHODS,
  SLIDERS, DIFFUSION_SLIDERS, EXAMPLES,
  defaultOptions, isHamMode, isEhbMode, isErrorDiffusion,
  maxDepth, defaultDepth, numColors, numBitplanes,
} from '../lib/options.js'

import InputNumber from 'primevue/inputnumber'
import Select from 'primevue/select'
import Slider from 'primevue/slider'
import ToggleSwitch from 'primevue/toggleswitch'
import Button from 'primevue/button'
import ProgressSpinner from 'primevue/progressspinner'
import Panel from 'primevue/panel'

const { loading: wasmLoading, error: wasmError, convertRGBA, convertPNG, convertIFF, convertHeader, convertRaw } = useWasm()
const { imageBytes, imageName, imageUrl, dragOver, onDrop, onDragOver, onDragLeave, openPicker } = useImageUpload()

const showUploadHint = ref(true)

// Load test example by default once WASM is ready
watch(wasmLoading, (loading) => {
  if (!loading && !imageBytes.value) {
    const example = EXAMPLES[0]
    fetch(`/examples/${example.file}`)
      .then(r => r.arrayBuffer())
      .then(buf => {
        imageBytes.value = new Uint8Array(buf)
        imageName.value = example.file
        const blob = new Blob([buf], { type: 'image/png' })
        imageUrl.value = URL.createObjectURL(blob)
      })
  }
})

const options = reactive(defaultOptions())
const canvasRef = ref(null)
const converting = ref(false)
const resultInfo = ref('')
const errorMsg = ref('')

// Flatten dither methods for Select component
const groupedDitherOptions = DITHER_METHODS.map(g => ({
  label: g.group,
  items: g.items.map(d => ({ value: d.value, label: d.label }))
}))

// Whether depth slider should be shown (not for HAM/EHB where depth is fixed)
const showDepthSlider = computed(() => {
  return !isHamMode(options.mode) && !isEhbMode(options.mode)
})

// Whether HAM controls should be shown
const showHamControls = computed(() => isHamMode(options.mode))

// Current depth max
const depthMax = computed(() => maxDepth(options.mode, options.chipset))

// Status line info
const statusColors = computed(() => numColors(options.mode, options.depth))
const statusBitplanes = computed(() => numBitplanes(options.mode, options.depth))
const statusChipset = computed(() => options.chipset.toUpperCase())

// Update depth when mode changes
watch(() => options.mode, (mode) => {
  options.depth = defaultDepth(mode)
  // Auto-set chipset for AGA-only modes
  if (mode === 'ham7' || mode === 'ham8') {
    options.chipset = 'aga'
  }
})

// Clamp depth when chipset changes
watch(() => options.chipset, () => {
  const max = maxDepth(options.mode, options.chipset)
  if (max > 0 && options.depth > max) {
    options.depth = max
  }
})

function buildWasmOptions() {
  return { ...options }
}

let debounceTimer = null

function doConvert() {
  if (!imageBytes.value || wasmLoading.value) return

  clearTimeout(debounceTimer)
  debounceTimer = setTimeout(async () => {
    converting.value = true
    errorMsg.value = ''

    await nextTick()
    await new Promise(r => setTimeout(r, 10))

    try {
      const result = convertRGBA(imageBytes.value, buildWasmOptions())

      if (result.error) {
        errorMsg.value = result.error
        converting.value = false
        return
      }

      const canvas = canvasRef.value
      if (!canvas) return

      const scale = 2
      const dw = result.width * scale
      const dh = result.height * scale
      canvas.width = dw
      canvas.height = dh
      canvas.style.width = `${result.width * 2}px`
      canvas.style.height = `${result.height * 2}px`

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

      resultInfo.value = `${result.width} x ${result.height}`
    } catch (e) {
      errorMsg.value = e.message
    }

    converting.value = false
  }, 150)
}

watch([imageBytes, () => ({ ...options })], doConvert, { deep: true })

function downloadPNG() {
  if (!imageBytes.value) return
  try {
    const result = convertPNG(imageBytes.value, buildWasmOptions())
    if (result.error) {
      errorMsg.value = result.error
      return
    }
    const blob = new Blob([result.data], { type: 'image/png' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '-amiga.png'
    a.click()
    URL.revokeObjectURL(url)
  } catch (e) {
    errorMsg.value = e.message
  }
}

function downloadIFF() {
  if (!imageBytes.value) return
  try {
    const result = convertIFF(imageBytes.value, buildWasmOptions())
    if (result.error) {
      errorMsg.value = result.error
      return
    }
    const blob = new Blob([result.data], { type: 'application/octet-stream' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '.iff'
    a.click()
    URL.revokeObjectURL(url)
  } catch (e) {
    errorMsg.value = e.message
  }
}

function downloadHeader() {
  if (!imageBytes.value) return
  try {
    const stem = (imageName.value || 'image').replace(/\.[^.]+$/, '').replace(/[^a-zA-Z0-9]/g, '_')
    const result = convertHeader(imageBytes.value, buildWasmOptions(), stem)
    if (result.error) {
      errorMsg.value = result.error
      return
    }
    const blob = new Blob([result.data], { type: 'text/plain' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = stem + '.h'
    a.click()
    URL.revokeObjectURL(url)
  } catch (e) {
    errorMsg.value = e.message
  }
}

function downloadRaw() {
  if (!imageBytes.value) return
  try {
    const result = convertRaw(imageBytes.value, buildWasmOptions())
    if (result.error) {
      errorMsg.value = result.error
      return
    }
    const blob = new Blob([result.data], { type: 'application/octet-stream' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '.raw'
    a.click()
    URL.revokeObjectURL(url)
  } catch (e) {
    errorMsg.value = e.message
  }
}

function resetOptions() {
  Object.assign(options, defaultOptions())
}

function dismissHint() {
  showUploadHint.value = false
}

async function loadExample(example) {
  dismissHint()
  Object.assign(options, defaultOptions())
  if (example.opts) Object.assign(options, example.opts)
  const resp = await fetch(`/examples/${example.file}`)
  const buf = await resp.arrayBuffer()
  imageBytes.value = new Uint8Array(buf)
  imageName.value = example.file
  const blob = new Blob([buf], { type: 'image/png' })
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

          <!-- Upload -->
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
                  </div>
                </div>
                <div class="text-xs text-color-secondary mt-2 px-1 flex justify-content-between overflow-hidden">
                  <span class="overflow-hidden text-overflow-ellipsis" style="min-width: 0; display: block;">{{ imageName }}</span>
                  <span class="white-space-nowrap ml-2 cursor-pointer flex-shrink-0" @click.stop="openPicker(); dismissHint()">Change</span>
                </div>
              </template>
              <template v-else>
                <i class="pi pi-image text-4xl text-color-secondary mb-2"></i>
                <div class="font-semibold text-sm">Drop image here</div>
                <div class="text-xs text-color-secondary">or click to browse</div>
              </template>
            </div>
            <div v-if="EXAMPLES.length > 1" class="mt-3">
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

          <!-- Mode / Chipset / Depth -->
          <Panel header="Mode">
            <div class="flex flex-column gap-2">
              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Amiga graphics mode. Lores: 320px, Hires: 640px, HAM: Hold-And-Modify, EHB: Extra Half-Brite.">Mode</label>
                <div class="col-8">
                  <Select v-model="options.mode" :options="MODES" optionValue="value" optionLabel="label" class="w-full" />
                </div>
              </div>

              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="OCS: Original Chip Set (12-bit, max 6 planes). AGA: Advanced Graphics Architecture (24-bit, max 8 planes).">Chipset</label>
                <div class="col-8">
                  <Select v-model="options.chipset" :options="CHIPSETS" optionValue="value" optionLabel="label" class="w-full" />
                </div>
              </div>

              <div v-if="showDepthSlider" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Number of bitplanes (1-8). More planes = more colors but more memory.">Depth</label>
                <div class="col-5">
                  <Slider v-model="options.depth" :min="1" :max="depthMax || 6" :step="1" class="w-full" />
                </div>
                <div class="col-3">
                  <InputNumber v-model="options.depth" :min="1" :max="depthMax || 6" :step="1" class="w-full input-sm" />
                </div>
              </div>

              <!-- Flags -->
              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Set LACE bit in CAMG for interlaced display (double vertical resolution).">Interlace</label>
                <div class="col-8">
                  <ToggleSwitch v-model="options.interlace" />
                </div>
              </div>

              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Per-scanline copper palette changes. Each row gets its own optimal palette.">Copper</label>
                <div class="col-8">
                  <ToggleSwitch v-model="options.copper" />
                </div>
              </div>

            </div>
          </Panel>

          <!-- Dithering -->
          <Panel header="Color Dithering">
            <div class="flex flex-column gap-2">
              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Dithering algorithm. Ordered methods use fixed patterns; error diffusion propagates quantization error to neighbors.">Method</label>
                <div class="col-8">
                  <Select
                    v-model="options.dither"
                    :options="groupedDitherOptions"
                    optionValue="value"
                    optionLabel="label"
                    optionGroupLabel="label"
                    optionGroupChildren="items"
                    class="w-full"
                  />
                </div>
              </div>
            </div>
          </Panel>

          <!-- Alpha -->
          <Panel header="Alpha" :toggleable="true" :collapsed="true">
            <div class="flex flex-column gap-2">
              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Alpha threshold: pixels below this value become transparent.">Threshold</label>
                <div class="col-5">
                  <Slider v-model="options.alphaThreshold" :min="0" :max="1.0" :step="0.05" class="w-full" />
                </div>
                <div class="col-3">
                  <InputNumber v-model="options.alphaThreshold" :min="0" :max="1.0" :step="0.05"
                    :minFractionDigits="2" :maxFractionDigits="2" class="w-full input-sm" />
                </div>
              </div>

              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Alpha dithering method. Empty = hard threshold.">Dither</label>
                <div class="col-8">
                  <Select v-model="options.alphaDither" :options="ALPHA_DITHER_METHODS" optionValue="value" optionLabel="label" class="w-full" />
                </div>
              </div>

              <div v-if="options.alphaDither" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Alpha dither strength.">Strength</label>
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

          <!-- HAM controls -->
          <Panel v-if="showHamControls" header="HAM Encoding">
            <div class="flex flex-column gap-2">
              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="HAM quality: fast = greedy per-pixel, optimal = DP beam search.">Quality</label>
                <div class="col-8">
                  <Select v-model="options.hamQuality" :options="[
                    { value: 'fast', label: 'Fast (greedy)' },
                    { value: 'optimal', label: 'Optimal (beam search)' },
                  ]" optionValue="value" optionLabel="label" class="w-full" />
                </div>
              </div>

              <div v-if="options.hamQuality === 'optimal'" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Beam width for DP search. Higher = better quality, slower. 16-256.">Beam</label>
                <div class="col-5">
                  <Slider v-model="options.hamBeam" :min="1" :max="256" :step="1" class="w-full" />
                </div>
                <div class="col-3">
                  <InputNumber v-model="options.hamBeam" :min="1" :max="256" :step="1" class="w-full input-sm" />
                </div>
              </div>
            </div>
          </Panel>

          <!-- Adjustments -->
          <Panel header="Adjustments">
            <div class="flex flex-column gap-2">
              <div class="grid align-items-center">
                <label class="col-3 text-xs text-color-secondary font-semibold" title="Remap image color range to fit the palette range in OKLab space.">Match Range</label>
                <div class="col-9">
                  <ToggleSwitch v-model="options.matchRange" />
                </div>
              </div>

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

          <!-- Crop -->
          <Panel header="Crop" :toggleable="true" :collapsed="true">
            <div class="flex flex-column gap-2">
              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Auto-crop to target mode aspect ratio (center crop).">Auto</label>
                <div class="col-8">
                  <ToggleSwitch v-model="options.cropAuto" />
                </div>
              </div>

              <template v-if="!options.cropAuto">
                <div class="grid align-items-center">
                  <label class="col-3 text-xs text-color-secondary font-semibold">X</label>
                  <div class="col-9">
                    <InputNumber v-model="options.cropX" :min="0" :step="1" class="w-full input-sm" />
                  </div>
                </div>
                <div class="grid align-items-center">
                  <label class="col-3 text-xs text-color-secondary font-semibold">Y</label>
                  <div class="col-9">
                    <InputNumber v-model="options.cropY" :min="0" :step="1" class="w-full input-sm" />
                  </div>
                </div>
                <div class="grid align-items-center">
                  <label class="col-3 text-xs text-color-secondary font-semibold">W</label>
                  <div class="col-9">
                    <InputNumber v-model="options.cropW" :min="0" :step="1" class="w-full input-sm" placeholder="0 = no crop" />
                  </div>
                </div>
                <div class="grid align-items-center">
                  <label class="col-3 text-xs text-color-secondary font-semibold">H</label>
                  <div class="col-9">
                    <InputNumber v-model="options.cropH" :min="0" :step="1" class="w-full input-sm" placeholder="0 = no crop" />
                  </div>
                </div>
              </template>
            </div>
          </Panel>

          <!-- Size Override -->
          <Panel header="Size" :toggleable="true" :collapsed="true">
            <div class="flex flex-column gap-2">
              <div class="grid align-items-center">
                <label class="col-3 text-xs text-color-secondary font-semibold" title="Override output width (0 = mode default).">Width</label>
                <div class="col-9">
                  <InputNumber v-model="options.width" :min="0" :step="16" class="w-full input-sm" placeholder="0 = auto" />
                </div>
              </div>
              <div class="grid align-items-center">
                <label class="col-3 text-xs text-color-secondary font-semibold" title="Override output height (0 = from aspect ratio).">Height</label>
                <div class="col-9">
                  <InputNumber v-model="options.height" :min="0" :step="2" class="w-full input-sm" placeholder="0 = auto" />
                </div>
              </div>
            </div>
          </Panel>

          <!-- Actions -->
          <div class="flex flex-column gap-2">
            <div class="flex gap-2">
              <Button label="png" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG"
                title="Download converted image as PNG preview." />
              <Button label="iff" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadIFF"
                title="Download as IFF ILBM (Deluxe Paint, WinUAE compatible)." />
              <Button label="h" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadHeader"
                title="Download C header with bitplane arrays and palette for Amiga C projects." />
              <Button label="raw" icon="pi pi-download" class="flex-1" severity="secondary" :disabled="!imageBytes || converting" @click="downloadRaw"
                title="Download raw interleaved bitplane data." />
            </div>
            <Button label="Reset" icon="pi pi-refresh" severity="secondary" outlined class="w-full" @click="resetOptions"
              title="Reset all parameters to defaults." />
          </div>

        </div>
      </div>

      <!-- Preview (sticky) -->
      <div class="col-12 md:col-8 lg:col-9 preview-col">
        <div v-if="!imageBytes" class="surface-card border-round-lg flex align-items-center justify-content-center" style="min-height: 500px;">
          <div class="text-center text-color-secondary">
            <i class="pi pi-upload text-5xl mb-3 block"></i>
            <div>Upload an image to get started</div>
          </div>
        </div>

        <div v-else class="flex flex-column gap-2">
          <div class="preview-container surface-card border-round-lg overflow-hidden relative">
            <canvas ref="canvasRef" class="preview-canvas" />
            <div v-if="converting" class="overlay flex align-items-center justify-content-center">
              <ProgressSpinner style="width: 2rem; height: 2rem" />
            </div>
          </div>
          <div class="flex justify-content-between align-items-center px-1">
            <span class="text-xs text-color-secondary">
              {{ resultInfo }}
              <template v-if="resultInfo"> | </template>
              {{ statusBitplanes }} planes, {{ statusColors }} colors, {{ statusChipset }}
            </span>
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

.preview-col {
  position: sticky;
  top: 1rem;
  align-self: start;
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

.preview-canvas {
  display: block;
  image-rendering: pixelated;
  image-rendering: crisp-edges;
}

.overlay {
  position: absolute;
  inset: 0;
  background: rgba(0, 0, 0, 0.5);
}
</style>
