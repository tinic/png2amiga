#!/usr/bin/env node
// Bench the WASM encoder's wall-clock time on a representative encode.
// Loads ../../build-wasm/png2amiga.{js,wasm}, runs convertRGBA N times
// on examples/chuck30.png at lores d=5 + floyd-steinberg, prints
// per-iteration timing + median. Use to A/B build flag changes (-O3
// vs -Oz vs -O2 etc.) without deploying to the web.

import { readFile } from 'node:fs/promises'
import { existsSync, readFileSync } from 'node:fs'
import path from 'node:path'

const WASM_DIR = path.resolve(import.meta.dirname, '../../build-wasm')
const IMG_PATH = path.resolve(import.meta.dirname, '../../examples/chuck30.png')
const ITERS = Number(process.env.ITERS ?? 5)
const MODE = process.env.MODE ?? 'lores'
const DEPTH = Number(process.env.DEPTH ?? 5)
const DITHER = process.env.DITHER ?? 'floyd-steinberg'
const BEST = process.env.BEST === '1'

function median(xs) {
  const s = [...xs].sort((a, b) => a - b)
  const m = Math.floor(s.length / 2)
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2
}

async function main() {
  const wasmJs = path.join(WASM_DIR, 'png2amiga.js')
  const wasmBin = path.join(WASM_DIR, 'png2amiga.wasm')
  if (!existsSync(wasmJs) || !existsSync(wasmBin)) {
    console.error(`bench-wasm: ${WASM_DIR}/png2amiga.{js,wasm} not found — build WASM first.`)
    process.exit(2)
  }
  const wasmBytes = await readFile(wasmBin)
  const wasmStat = readFileSync(wasmBin).byteLength

  // Mute Emscripten's noisy streaming-compile fallback warning.
  const origErr = console.error
  console.error = (...a) => {
    const s = String(a[0] ?? '')
    if (s.startsWith('wasm streaming compile failed') ||
        s.startsWith('falling back to ArrayBuffer instantiation')) return
    origErr(...a)
  }
  const { default: createPng2Amiga } = await import(wasmJs)
  const Module = await createPng2Amiga({ wasmBinary: wasmBytes })
  console.error = origErr

  const pngBytes = await readFile(IMG_PATH)
  const baseOpts = {
    mode: MODE,
    depth: DEPTH,
    dither: DITHER,
    best: BEST,
  }

  console.log(`bench-wasm: ${path.basename(IMG_PATH)}  mode=${MODE}  d=${DEPTH}  dither=${DITHER}  best=${BEST}`)
  console.log(`            wasm: ${(wasmStat/1024/1024).toFixed(2)} MB raw`)
  console.log(`            iters: ${ITERS}`)
  console.log('')

  // Warmup (avoid V8 JIT noise in the first measured iteration).
  Module.convertRGBA(pngBytes, baseOpts)

  const times = []
  for (let i = 0; i < ITERS; i++) {
    const t0 = process.hrtime.bigint()
    const r = Module.convertRGBA(pngBytes, baseOpts)
    const t1 = process.hrtime.bigint()
    const ms = Number(t1 - t0) / 1e6
    times.push(ms)
    if (r.error) { console.error(`  iter ${i}: ${r.error}`); process.exit(1) }
    console.log(`  iter ${i + 1}: ${ms.toFixed(1)} ms`)
  }
  const med = median(times)
  const min = Math.min(...times)
  const max = Math.max(...times)
  console.log('')
  console.log(`  median: ${med.toFixed(1)} ms   min: ${min.toFixed(1)}   max: ${max.toFixed(1)}`)
}

await main()
