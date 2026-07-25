#!/usr/bin/env node
// Drift-detector between the Embind C++ surface in src/wasm_bindings.cpp
// and the hand-written TS declarations in src/vite-env.d.ts.
//
// Two checks:
//   1. EMSCRIPTEN_BINDINGS function names ↔ Png2AmigaModule methods.
//   2. parse_js_options hasOwnProperty keys ↔ WasmOptions interface keys.
// Both compare set-equality and fail with a list of the missing entries.

import { readFile } from 'node:fs/promises'
import path from 'node:path'

const CPP_PATH = path.resolve(import.meta.dirname, '../../src/wasm_bindings.cpp')
const DTS_PATH = path.resolve(import.meta.dirname, '../src/vite-env.d.ts')

const cpp = await readFile(CPP_PATH, 'utf8')
const dts = await readFile(DTS_PATH, 'utf8')

// Extract the EMSCRIPTEN_BINDINGS block and pull every `function("name", …)`.
const bindingsBlock = /EMSCRIPTEN_BINDINGS\([^)]*\)\s*\{([\s\S]*?)\}/.exec(cpp)
if (!bindingsBlock) {
  console.error('check-wasm-bindings: EMSCRIPTEN_BINDINGS block not found in ' + CPP_PATH)
  process.exit(2)
}
const cppExports = new Set(
  bindingsBlock[1].matchAll(/function\("([^"]+)"/g).map(m => m[1])
)

// Extract the Png2AmigaModule interface and pull every method-like line:
// `name(args): ReturnType`.
const moduleBlock = /interface[ \t]+Png2AmigaModule[ \t]*\{([\s\S]*?)\n[ \t]*\}/.exec(dts)
if (!moduleBlock) {
  console.error('check-wasm-bindings: Png2AmigaModule interface not found in ' + DTS_PATH)
  process.exit(2)
}
const dtsExports = new Set(
  moduleBlock[1].matchAll(/^[ \t]*([A-Za-z_$][A-Za-z0-9_$]*)[ \t]*\(/gm).map(m => m[1])
)

const missingInDts = [...cppExports.difference(dtsExports)]
const missingInCpp = [...dtsExports.difference(cppExports)]

console.log(`  C++ exports (${cppExports.size}): ${[...cppExports].sort().join(', ')}`)
console.log(`  TS exports  (${dtsExports.size}): ${[...dtsExports].sort().join(', ')}`)

let failed = false
if (missingInDts.length > 0 || missingInCpp.length > 0) {
  console.error('\ncheck-wasm-bindings: WASM ↔ TS export sets differ!')
  if (missingInDts.length > 0) console.error('  In C++ but not in vite-env.d.ts:', missingInDts.join(', '))
  if (missingInCpp.length > 0) console.error('  In vite-env.d.ts but not in C++:', missingInCpp.join(', '))
  console.error('\nFix: declare the missing binding in src/vite-env.d.ts (Png2AmigaModule)')
  console.error('     or remove the dead declaration if the C++ side dropped it.')
  failed = true
} else {
  console.log('  [OK] Embind exports and Png2AmigaModule declarations match.')
}

// Cross-check 2: WasmOptions key set vs parse_js_options hasOwnProperty checks.
// `onProgress` is special — it's injected by the worker, not read by C++.
const cppOptionKeys = new Set(
  cpp.matchAll(/hasOwnProperty\("([^"]+)"\)/g).map(m => m[1])
)
cppOptionKeys.delete('onProgress')

const optionsBlock = /interface[ \t]+WasmOptions[ \t]*\{([\s\S]*?)\n[ \t]*\}/.exec(dts)
if (!optionsBlock) {
  console.error('check-wasm-bindings: WasmOptions interface not found in ' + DTS_PATH)
  process.exit(2)
}
// Match `name?: …` and `name: …` field declarations (skip comments).
const dtsOptionKeys = new Set(
  optionsBlock[1].matchAll(/^[ \t]*([a-zA-Z_$][a-zA-Z0-9_$]*)\??[ \t]*:/gm).map(m => m[1])
)
dtsOptionKeys.delete('onProgress')

const optMissingInDts = [...cppOptionKeys.difference(dtsOptionKeys)]
const optMissingInCpp = [...dtsOptionKeys.difference(cppOptionKeys)]

console.log(`  C++ options (${cppOptionKeys.size}): ${[...cppOptionKeys].sort().join(', ')}`)
console.log(`  TS  options (${dtsOptionKeys.size}): ${[...dtsOptionKeys].sort().join(', ')}`)

if (optMissingInDts.length > 0 || optMissingInCpp.length > 0) {
  console.error('\ncheck-wasm-bindings: WasmOptions ↔ parse_js_options key sets differ!')
  if (optMissingInDts.length > 0) console.error('  Read by C++ but not declared in WasmOptions:', optMissingInDts.join(', '))
  if (optMissingInCpp.length > 0) console.error('  Declared in WasmOptions but not read by C++:', optMissingInCpp.join(', '))
  console.error('\nFix: align src/vite-env.d.ts (WasmOptions) with parse_js_options in')
  console.error('     src/wasm_bindings.cpp. Don\'t silently drop fields — runtime ignores')
  console.error('     unknown keys but TS will reject mistypes at the call site.')
  failed = true
} else {
  console.log('  [OK] WasmOptions interface and parse_js_options keys match.')
}

if (failed) process.exit(1)
