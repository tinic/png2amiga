import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'

import { test, expect } from '@playwright/test'

interface TandyMode { width: number; colors: number; size: number; banks: number; stride: number; bios: string }

const modes: TandyMode[] = [
  { width: 160, colors: 16, size: 16384, banks: 2, stride: 80, bios: '08' },
  { width: 320, colors: 16, size: 32768, banks: 4, stride: 160, bios: '09' },
  { width: 640, colors: 4, size: 32768, banks: 4, stride: 160, bios: '0A' },
]

function indexAt(raw: Buffer, mode: TandyMode, x: number, y: number): number {
  const row = (y % mode.banks) * 8192 + Math.floor(y / mode.banks) * mode.stride
  if (mode.width === 640) {
    const offset = row + 2 * Math.floor(x / 8)
    const lo = raw[offset] ?? 0
    const hi = raw[offset + 1] ?? 0
    const bit = 7 - (x % 8)
    return ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)
  }
  const byte = raw[row + Math.floor(x / 2)] ?? 0
  return (byte >> (x % 2 ? 0 : 4)) & 15
}

function expectedColor(mode: TandyMode, x: number, y: number): number {
  const q = (x + 5 * y + Math.floor(x / 7)) % mode.colors
  return mode.colors === 4 ? ([1, 6, 10, 15][q] ?? 0) : q
}

function verifyRaw(raw: Buffer, mode: TandyMode): void {
  expect(raw).toHaveLength(mode.size + 16)
  for (let y = 0; y < 200; y++) {
    for (let x = 0; x < mode.width; x++) {
      const index = indexAt(raw, mode, x, y)
      if (raw[mode.size + index] !== expectedColor(mode, x, y)) {
        throw new Error(`Wrong RGBI pixel at ${x},${y}`)
      }
    }
  }
}

// Reversed fixture palettes prevent relying on source PNG numeric indices.
for (const mode of modes) {
  const { width, colors, bios } = mode
  test(`Tandy ${width} native pixels and DOS exports`, async ({ page }) => {
    await page.goto('/')
    await expect(page.locator('.p-progressspinner')).toHaveCount(0, { timeout: 30_000 })
    if (width === 160) {
      // Switching from an uploaded Amiga palette must not send stale,
      // unsupported palette data to a fixed-RGBI target.
      const advanced = page.locator('.p-panel').filter({ has: page.getByText('Advanced', { exact: true }) })
      await advanced.getByRole('button').first().click()
      const paletteChooser = page.waitForEvent('filechooser')
      await advanced.getByRole('button', { name: 'Load', exact: true }).click()
      await (await paletteChooser).setFiles({
        name: 'test.hex', mimeType: 'text/plain', buffer: Buffer.from('000000\nFFFFFF\nAA0000\n00AA00\n'),
      })
      await expect(advanced.getByText('4 colors', { exact: true })).toBeVisible()
    }
    await page.getByRole('combobox').nth(0).click()
    await page.getByRole('option', { name: 'Tandy 1000 (RGBI)', exact: true }).click()
    await page.getByRole('combobox').nth(1).click()
    await page.getByRole('option', { name: `Tandy ${width}x200 (${colors} colors)`, exact: true }).click()
    const chooser = page.waitForEvent('filechooser')
    await page.getByRole('button', { name: 'Change', exact: true }).click()
    await (await chooser).setFiles(fileURLToPath(new URL(`../fixtures/tandy-${width}.png`, import.meta.url)))
    await expect(page.getByText(`tandy-${width}.png`, { exact: false })).toBeVisible()
    await expect(page.getByLabel('Cell refine', { exact: true })).toHaveCount(0)
    await expect(page.getByLabel('Reserve color 0 for black', { exact: true })).toBeDisabled()
    await expect(page.getByLabel('Reserve color 0 for black', { exact: true })).not.toBeChecked()
    await page.locator('.dither-trigger').click()
    await page.locator('.dither-card').filter({ hasText: 'Bayer 4×4' }).click()
    const downloaded = page.waitForEvent('download')
    await page.getByRole('button', { name: 'raw', exact: true }).click()
    // eslint-disable-next-line security/detect-non-literal-fs-filename -- Playwright owns the download path.
    const raw = await readFile(await (await downloaded).path())
    verifyRaw(raw, mode)
    const viewerDownload = page.waitForEvent('download')
    await page.getByRole('button', { name: 'c', exact: true }).click()
    const viewer = await viewerDownload
    expect(viewer.suggestedFilename()).toMatch(/\.c$/)
    // eslint-disable-next-line security/detect-non-literal-fs-filename -- Playwright owns the download path.
    const source = await readFile(await viewer.path(), 'utf8')
    expect(source).toContain(`set_mode(0x${bios})`)
    expect(source).toContain('outb(0x3DE')
    expect(source).toContain('outb(0x3DA, 0)')
    await page.route('**/api/compile?format=*', async route => {
      expect(route.request().postData()).toBe(source)
      await route.fulfill({ contentType: 'application/octet-stream', body: 'test compiler response' })
    })
    for (const name of ['exe', 'img']) {
      const exported = page.waitForEvent('download')
      await page.getByRole('button', { name, exact: true }).click()
      await exported
    }
  })
}
