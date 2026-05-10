#!/usr/bin/env node
// Bundle-size budget check. Runs after vite build. Fails the build if any
// JS / WASM / CSS asset in service/html/assets exceeds its budget.
//
// Budgets are intentionally loose for headroom but tight enough to flag
// accidental dep bloat. Tighten over time. Sizes are RAW bytes, not gzip,
// because that's what the user agent fetches when CDN-compression isn't
// available (e.g. local dev, MITM proxies).

import { readdir, stat } from 'node:fs/promises'
import path from 'node:path'

const ASSETS = path.resolve(import.meta.dirname, '../../service/html/assets')

// pattern → max bytes. Patterns checked in order; first match wins.
const BUDGETS = [
  { match: /^index-.*\.js$/,        max: 700 * 1024, label: 'app bundle' },
  // 64 KB up from 50 KB — Emscripten's pthread shim adds the worker
  // bootstrap + Atomics.wait helpers + dispatch-queue plumbing to the
  // glue. Without pthreads we sit at ~24 KB; with pthreads ~50 KB.
  // Headroom for future Emscripten updates.
  { match: /^png2amiga-.*\.js$/,    max:  64 * 1024, label: 'wasm glue'  },
  // 2.40 MB up from 2.25 MB — vendored nlohmann/json for spec-
  // compliant palette JSON parsing (tolerates key reordering /
  // pretty-printing / comments). Adds ~50 KB to the WASM binary.
  { match: /^png2amiga-.*\.wasm$/,  max:   2.4 * 1024 * 1024, label: 'wasm binary' },
  { match: /^wasm\.worker-.*\.js$/, max:  20 * 1024, label: 'worker'     },
  { match: /^crt-.*\.js$/,          max:  30 * 1024, label: 'crt module' },
  { match: /^index-.*\.css$/,       max: 500 * 1024, label: 'css'        },
]

const fmt = (b) => b >= 1024 ? `${(b / 1024).toFixed(1)} KB` : `${b} B`

let failed = false
const files = await readdir(ASSETS)
for (const file of files.sort()) {
  // Skip pre-compressed siblings (.gz / .br) — we only check the original.
  if (file.endsWith('.gz') || file.endsWith('.br')) continue
  const budget = BUDGETS.find(b => b.match.test(file))
  if (!budget) continue
  const { size } = await stat(path.join(ASSETS, file))
  const pct = ((size / budget.max) * 100).toFixed(0)
  const status = size > budget.max ? 'FAIL' : 'OK'
  console.log(`  [${status}] ${file.padEnd(40)} ${fmt(size).padStart(10)} / ${fmt(budget.max).padStart(10)} (${pct}%)`)
  if (size > budget.max) failed = true
}

if (failed) {
  console.error('\nbundle-size: budget exceeded. Either trim deps / split chunks, or raise the budget in scripts/check-bundle-size.mjs (with a justification).')
  process.exit(1)
}
