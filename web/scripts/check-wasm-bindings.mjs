#!/usr/bin/env node
// Drift-detector between the Embind exports in src/wasm_bindings.cpp and
// the hand-written Png2AmigaModule TS declaration in src/vite-env.d.ts.
//
// Doesn't (yet) validate parameter shapes — just the *set* of exported
// function names. A full generator would parse the C++ signatures, but
// most drift bugs are "we added a binding and forgot to declare it" or
// vice versa, which a name-set diff catches.

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
  [...bindingsBlock[1].matchAll(/function\("([^"]+)"/g)].map(m => m[1])
)

// Extract the Png2AmigaModule interface and pull every method-like line:
// `name(args): ReturnType`.
const moduleBlock = /interface\s+Png2AmigaModule\s*\{([\s\S]*?)\n\s*\}/.exec(dts)
if (!moduleBlock) {
  console.error('check-wasm-bindings: Png2AmigaModule interface not found in ' + DTS_PATH)
  process.exit(2)
}
const dtsExports = new Set(
  [...moduleBlock[1].matchAll(/^\s*([A-Za-z_$][A-Za-z0-9_$]*)\s*\(/gm)].map(m => m[1])
)

const missingInDts = [...cppExports].filter(n => !dtsExports.has(n))
const missingInCpp = [...dtsExports].filter(n => !cppExports.has(n))

console.log(`  C++ exports (${cppExports.size}): ${[...cppExports].sort().join(', ')}`)
console.log(`  TS exports  (${dtsExports.size}): ${[...dtsExports].sort().join(', ')}`)

if (missingInDts.length > 0 || missingInCpp.length > 0) {
  console.error('\ncheck-wasm-bindings: WASM ↔ TS export sets differ!')
  if (missingInDts.length > 0) console.error('  In C++ but not in vite-env.d.ts:', missingInDts.join(', '))
  if (missingInCpp.length > 0) console.error('  In vite-env.d.ts but not in C++:', missingInCpp.join(', '))
  console.error('\nFix: declare the missing binding in src/vite-env.d.ts (Png2AmigaModule)')
  console.error('     or remove the dead declaration if the C++ side dropped it.')
  process.exit(1)
}

console.log('  [OK] Embind exports and Png2AmigaModule declarations match.')
