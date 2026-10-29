import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'

import { test, expect } from '@playwright/test'

// Exercise the production WASM path and the actual bytes offered for DOS.
test('CGA 160x100 uses direct pixels and exports half-block DOS data', async ({ page }) => {
  await page.goto('/')
  await expect(page.locator('.p-progressspinner')).toHaveCount(0, { timeout: 30_000 })
  const chooser = page.waitForEvent('filechooser')
  await page.getByRole('button', { name: 'Change', exact: true }).click()
  await (await chooser).setFiles(fileURLToPath(new URL('../fixtures/cga160.png', import.meta.url)))
  await expect(page.getByText(/cga160.png/)).toBeVisible()
  await page.getByRole('combobox').nth(0).click()
  await page.getByRole('option', { name: 'IBM PC CGA (fixed palette)', exact: true }).click()
  await page.getByRole('combobox').nth(1).click()
  await page.getByRole('option', { name: 'CGA Text 80x100', exact: true }).click()
  await page.getByLabel('Cell refine', { exact: true }).check()
  await page.getByRole('combobox').nth(1).click()
  await page.getByRole('option', { name: 'CGA 160x100 (16 colors)', exact: true }).click()
  await expect(page.getByLabel('Cell refine', { exact: true })).toHaveCount(0)
  await expect(page.getByText('Native PAR', { exact: true })).toHaveCount(0)
  await expect(page.getByText('Resize', { exact: true })).toHaveCount(0)
  await page.locator('.dither-trigger').click()
  await expect(page.locator('.dither-card').filter({ hasText: 'Bayer 4×4' })).toBeVisible()
  await page.locator('.dither-card').filter({ hasText: 'Bayer 4×4' }).click()

  const rawDownload = page.waitForEvent('download')
  await page.getByRole('button', { name: 'raw', exact: true }).click()
  // eslint-disable-next-line security/detect-non-literal-fs-filename -- Playwright owns the download path.
  const raw = await readFile(await (await rawDownload).path())
  expect(raw).toHaveLength(16000)
  for (let i = 0; i < raw.length; i += 2) {
    expect(raw[i]).toBe(0xDE)
    expect(raw[i + 1]).toBe((i / 2) % 256)
  }

  const pngDownload = page.waitForEvent('download')
  await page.getByRole('button', { name: 'png', exact: true }).click()
  // eslint-disable-next-line security/detect-non-literal-fs-filename -- Playwright owns the download path.
  const png = await readFile(await (await pngDownload).path())
  expect(png.readUInt32BE(16)).toBe(160)
  expect(png.readUInt32BE(20)).toBe(100)
  expect(png[25]).toBe(3) // indexed PNG
  const paletteStart = png.indexOf('PLTE') + 4
  const colors = [0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF]
  for (const [i, rgb] of colors.entries()) {
    expect(png.readUIntBE(paletteStart + i * 3, 3)).toBe(rgb)
  }

  const cDownload = page.waitForEvent('download')
  await page.getByRole('button', { name: 'c', exact: true }).click()
  const cFile = await cDownload
  expect(cFile.suggestedFilename()).toMatch(/\.c$/)
  // eslint-disable-next-line security/detect-non-literal-fs-filename -- Playwright owns the download path.
  const source = await readFile(await cFile.path(), 'utf8')
  expect(source).toContain('fixed 0xDE')
  expect(source).toContain('crtc_init[14]')
  expect(source).toContain('outb(0x3D8, 0x09)')

  // Both compiler-service exports must receive this same viewer source.
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

test('CGA 160x200 preserves pixels using the 80x200 layout and gates DOS viewers', async ({ page }) => {
  await page.goto('/')
  await expect(page.locator('.p-progressspinner')).toHaveCount(0, { timeout: 30_000 })
  const chooser = page.waitForEvent('filechooser')
  await page.getByRole('button', { name: 'Change', exact: true }).click()
  await (await chooser).setFiles(fileURLToPath(new URL('../fixtures/cga160.png', import.meta.url)))
  await expect(page.getByText(/cga160.png/)).toBeVisible()
  await page.getByRole('combobox').nth(0).click()
  await page.getByRole('option', { name: 'IBM PC CGA (fixed palette)', exact: true }).click()
  await page.getByRole('combobox').nth(1).click()
  await page.getByRole('option', { name: 'CGA 160x200 (16 colors)', exact: true }).click()
  await expect(page.getByText(/32 KB exceeds standard CGA/)).toBeVisible()
  for (const name of ['img', 'exe', 'c']) {
    await expect(page.getByRole('button', { name, exact: true })).toBeDisabled()
  }
  await expect(page.getByLabel('Cell refine', { exact: true })).toHaveCount(0)
  await page.locator('.dither-trigger').click()
  await page.locator('.dither-card').filter({ hasText: 'Bayer 4×4' }).click()
  const rawDownload = page.waitForEvent('download')
  await page.getByRole('button', { name: 'raw', exact: true }).click()
  // eslint-disable-next-line security/detect-non-literal-fs-filename -- Playwright owns the download path.
  const raw = await readFile(await (await rawDownload).path())
  expect(raw).toHaveLength(32000)
  for (let y = 0; y < 200; y++) {
    for (let cell = 0; cell < 80; cell++) {
      const offset = (y * 80 + cell) * 2
      expect(raw[offset]).toBe(0xDE)
      expect(raw[offset + 1]).toBe((Math.floor(y / 2) * 80 + cell) % 256)
    }
  }
  const pngDownload = page.waitForEvent('download')
  await page.getByRole('button', { name: 'png', exact: true }).click()
  // eslint-disable-next-line security/detect-non-literal-fs-filename -- Playwright owns the download path.
  const png = await readFile(await (await pngDownload).path())
  expect(png.readUInt32BE(16)).toBe(160)
  expect(png.readUInt32BE(20)).toBe(200)
  expect(png[25]).toBe(3)
})
