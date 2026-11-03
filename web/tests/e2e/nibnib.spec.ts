import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'

import { test, expect } from '@playwright/test'

function expandPair(pair: number): number[] {
  return [pair & 0xFFFF, pair >>> 16].flatMap(pixel => {
    const r = pixel >>> 11, g = (pixel >>> 5) & 63, b = pixel & 31
    return [(r << 3) | (r >>> 2), (g << 2) | (g >>> 4), (b << 3) | (b >>> 2), 255]
  })
}

function decode(raw: Buffer): number[] {
  const width = raw.readUInt16LE(0), height = raw.readUInt16LE(2)
  const stride = Math.ceil((4 + (width - 2) / 2) / 4) * 4
  const pixels: number[] = []
  for (let y = 0; y < height; y++) {
    const offset = 1028 + y * stride
    let pair = raw.readUInt32LE(offset)
    for (let x = 0; x < width; x += 2) {
      if (x > 0) pair = ((pair >>> 16) * 0x10001 + raw.readUInt32LE(4 + 4 * raw.readUInt8(offset + 3 + x / 2))) >>> 0
      pixels.push(...expandPair(pair))
    }
  }
  return pixels
}

test('nibnib Best is available and its download decodes to the PNG preview', async ({ page }) => {
  await page.goto('/')
  await expect(page.locator('.p-progressspinner')).toHaveCount(0, { timeout: 30_000 })
  const chooser = page.waitForEvent('filechooser')
  await page.getByRole('button', { name: 'Change', exact: true }).click()
  await (await chooser).setFiles(fileURLToPath(new URL('../fixtures/nibnib.png', import.meta.url)))
  await expect(page.getByText('nibnib.png', { exact: false })).toBeVisible()
  await page.getByRole('combobox').nth(0).click()
  await page.getByRole('option', { name: 'RP2350: nibnib (RGB565)', exact: true }).click()
  const best = page.getByLabel('Best', { exact: true })
  await best.check()
  await expect(best).toBeChecked()
  await expect(page.getByText('palette / objective sweep', { exact: true })).toBeVisible()
  const downloaded = page.waitForEvent('download')
  await page.getByRole('button', { name: 'nibnib', exact: true }).click()
  const file = await downloaded
  expect(file.suggestedFilename()).toMatch(/\.nibnib$/)
  // eslint-disable-next-line security/detect-non-literal-fs-filename -- Playwright owns the download path.
  const raw = await readFile(await file.path())
  const width = raw.readUInt16LE(0), height = raw.readUInt16LE(2)
  expect([width, height]).toEqual([16, 8])
  const stride = Math.ceil((4 + (width - 2) / 2) / 4) * 4
  expect(raw).toHaveLength(1028 + stride * height)
  const pngDownload = page.waitForEvent('download')
  await page.getByRole('button', { name: 'png', exact: true }).click()
  // eslint-disable-next-line security/detect-non-literal-fs-filename -- Playwright owns the download path.
  const png = await readFile(await (await pngDownload).path())
  const pixels = await page.evaluate(async data => {
    const bitmap = await createImageBitmap(new Blob([Uint8Array.from(data)], { type: 'image/png' }))
    const canvas = document.createElement('canvas')
    canvas.width = bitmap.width
    canvas.height = bitmap.height
    const context = canvas.getContext('2d')
    if (!context) throw new Error('Missing canvas context')
    context.drawImage(bitmap, 0, 0)
    bitmap.close()
    return [...context.getImageData(0, 0, canvas.width, canvas.height).data]
  }, [...png])
  expect(pixels).toEqual(decode(raw))
})
