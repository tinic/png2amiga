#!/usr/bin/env node
// Generate one PNG thumbnail per dither method into web/public/dither-thumbs/.
// Each thumb is the WASM encoder's output on a 48×48 45° black→white ramp,
// converted to lores depth=1 (pure 2-color) so the dither pattern reads
// cleanly without palette-induced muddiness.

import { mkdir, writeFile, readFile } from 'node:fs/promises'
import { existsSync } from 'node:fs'
import { deflateSync, crc32 } from 'node:zlib'
import path from 'node:path'

const OUT_DIR = path.resolve(import.meta.dirname, '../public/dither-thumbs')
const RAMP_W = 32
const RAMP_H = 32

// Pure-JS PNG encoder (RGBA8 only). Output: a single IDAT-compressed buffer
// wrapped with the 8-byte signature, IHDR, IDAT and IEND chunks. Adequate
// for our 48×48 thumbs — no need for a dependency.
function encodePNG(rgba, width, height) {
  const sig = Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])

  const ihdr = Buffer.alloc(13)
  ihdr.writeUInt32BE(width, 0)
  ihdr.writeUInt32BE(height, 4)
  ihdr[8] = 8       // bit depth
  ihdr[9] = 6       // color type RGBA
  ihdr[10] = 0      // compression: deflate
  ihdr[11] = 0      // filter: standard set
  ihdr[12] = 0      // no interlace

  // Pre-deflate: prepend a filter byte (0 = None) to each row.
  const stride = width * 4
  const raw = Buffer.alloc((stride + 1) * height)
  const src = Buffer.isBuffer(rgba)
    ? rgba
    : Buffer.from(rgba.buffer, rgba.byteOffset, rgba.byteLength)
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = 0
    src.copy(raw, y * (stride + 1) + 1, y * stride, y * stride + stride)
  }
  const idat = deflateSync(raw, { level: 9 })

  const chunk = (type, data) => {
    const len = Buffer.alloc(4)
    len.writeUInt32BE(data.length, 0)
    const t = Buffer.from(type, 'ascii')
    const td = Buffer.concat([t, data])
    const crc = Buffer.alloc(4)
    crc.writeUInt32BE(crc32(td) >>> 0, 0)
    return Buffer.concat([len, td, crc])
  }

  return Buffer.concat([
    sig,
    chunk('IHDR', ihdr),
    chunk('IDAT', idat),
    chunk('IEND', Buffer.alloc(0)),
  ])
}

// Deterministic mulberry32 — seeded so thumb generation is reproducible.
function mulberry32(seed) {
  let s = seed >>> 0
  return () => {
    s = (s + 0x6D2B79F5) >>> 0
    let t = s
    t = Math.imul(t ^ (t >>> 15), t | 1)
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
    return ((t ^ (t >>> 14)) >>> 0) / 0x1_0000_0000
  }
}

// 45° diagonal black→white ramp with white peaking in the bottom-right
// (4th-quadrant) corner: luma = (x+y) / (w+h-2). A small amount of
// ±2/255 white-noise jitter breaks the lock-step that error-diffusion
// kernels otherwise lock onto on a perfectly smooth ramp — without
// jitter, F-S/Atkinson/etc. produce eerily regular patterns that look
// indistinguishable from ordered dithers.
function makeRamp(width, height) {
  const rgba = new Uint8Array(width * height * 4)
  const denom = width + height - 2
  const rand = mulberry32(0xC0FFEE)
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const t = (x + y) / denom
      const noise = (rand() - 0.5) * 4   // ±2/255
      const v = Math.max(0, Math.min(255, Math.round(255 * t + noise)))
      const i = (y * width + x) * 4
      rgba[i] = v; rgba[i + 1] = v; rgba[i + 2] = v; rgba[i + 3] = 255
    }
  }
  return rgba
}

// Pull the dither method values out of options.ts so we never drift from
// the source list. We re-parse the file because options.ts has Vite-only
// imports that don't resolve in plain Node.
async function readDitherMethods() {
  const optsPath = path.resolve(import.meta.dirname, '../src/lib/options.ts')
  const src = await readFile(optsPath, 'utf8')
  const m = /export const DITHER_METHODS:[^=]*=\s*(\[[\s\S]*?\n\])/.exec(src)
  if (!m) throw new Error('DITHER_METHODS array not found in options.ts')
  // Match every `value: '<method>'` inside the array.
  const values = [...m[1].matchAll(/value:\s*'([^']+)'/g)].map(x => x[1])
  if (values.length === 0) throw new Error('No dither values parsed from options.ts')
  return values
}

async function main() {
  const wasmPath = path.resolve(import.meta.dirname, '../../build-wasm/png2amiga.js')
  if (!existsSync(wasmPath)) {
    console.error(`gen-dither-thumbs: ${wasmPath} not found — build the WASM module first.`)
    process.exit(2)
  }

  // The WASM module is built with -sENVIRONMENT=web,worker, so the
  // factory's URL-fetch path is the only one wired up. Side-step it by
  // pre-loading the .wasm bytes ourselves and passing them as wasmBinary.
  const wasmBytes = await readFile(
    path.resolve(import.meta.dirname, '../../build-wasm/png2amiga.wasm')
  )
  // Mute the runtime's "wasm streaming compile failed" warning that fires
  // before our wasmBinary fallback kicks in — it's noisy but harmless.
  const origErr = console.error
  console.error = (...a) => {
    const s = String(a[0] ?? '')
    if (s.startsWith('wasm streaming compile failed') ||
        s.startsWith('falling back to ArrayBuffer instantiation')) return
    origErr(...a)
  }

  const { default: createPng2Amiga } = await import(wasmPath)
  const Module = await createPng2Amiga({ wasmBinary: wasmBytes })
  console.error = origErr

  const methods = await readDitherMethods()
  await mkdir(OUT_DIR, { recursive: true })

  const rampRGBA = makeRamp(RAMP_W, RAMP_H)
  const rampPNG = encodePNG(rampRGBA, RAMP_W, RAMP_H)

  // Force a 4-gray palette at depth=2 so the dither pattern fills the
  // whole thumbnail. With depth=1 (B/W), only the narrow band straddling
  // 50% luma actually dithers — the rest snaps to pure black or white.
  // With 4 evenly-spaced grays the ramp passes through three dither
  // bands and pattern is visible across the entire image.
  const palette = new TextEncoder().encode('000000\n555555\nAAAAAA\nFFFFFF\n')

  let fail = 0
  for (const method of methods) {
    const opts = {
      mode: 'lores',
      depth: 2,
      dither: method,
      ditherStrength: 2,
      width: RAMP_W,
      height: RAMP_H,
      paletteData: palette,
    }
    const result = Module.convertRGBA(rampPNG, opts)
    if (result.error || !result.rgba) {
      console.error(`  [FAIL] ${method}: ${result.error ?? 'no rgba returned'}`)
      fail++
      continue
    }
    const png = encodePNG(Buffer.from(result.rgba), result.width, result.height)
    const outPath = path.join(OUT_DIR, `${method}.png`)
    await writeFile(outPath, png)
    console.log(`  [OK]   ${method}  ${result.width}x${result.height}  ${png.length} bytes`)
  }
  if (fail > 0) {
    console.error(`gen-dither-thumbs: ${fail} method(s) failed`)
    process.exit(1)
  }
}

await main()
