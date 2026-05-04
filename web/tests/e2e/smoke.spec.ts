import { test, expect } from '@playwright/test'

// Smoke test: the WASM module loads, the default example renders, the
// preview canvas has non-empty content, and a setting change triggers
// a re-encode (visible as a different canvas hash).

test('default example loads and renders to canvas', async ({ page }) => {
  await page.goto('/')

  // Wait for WASM to finish loading. The loading spinner has a known
  // ProgressSpinner DOM signature; once it's gone the encoder is ready.
  await expect(page.locator('.p-progressspinner')).toHaveCount(0, { timeout: 30_000 })

  // The default example loader fires once WASM is ready and seeds the
  // image-name display.
  await expect(page.getByText(/electrichues/)).toBeVisible({ timeout: 15_000 })

  // Preview canvas exists and has non-trivial dimensions (lores 320×… px
  // at 2× scale ≈ 640+).
  const canvas = page.locator('canvas.preview-canvas').first()
  await expect(canvas).toBeVisible()
  const dims = await canvas.evaluate((el) => {
    const c = el as HTMLCanvasElement
    return { w: c.width, h: c.height }
  })
  expect(dims.w).toBeGreaterThan(100)
  expect(dims.h).toBeGreaterThan(100)
})

test('switching example image re-encodes the preview', async ({ page }) => {
  await page.goto('/')
  await expect(page.locator('.p-progressspinner')).toHaveCount(0, { timeout: 30_000 })

  const canvas = page.locator('canvas.preview-canvas').first()
  await expect(canvas).toBeVisible()

  const sample = (el: HTMLElement) => {
    const c = el as HTMLCanvasElement
    const ctx = c.getContext('2d')
    if (!ctx) return ''
    return ctx.getImageData(0, 0, Math.min(c.width, 64), Math.min(c.height, 64)).data.join(',')
  }

  const before = await canvas.evaluate(sample)
  expect(before.length).toBeGreaterThan(0)

  // Click a different example thumbnail. The first example loads on
  // bootstrap, so picking the 2nd guarantees a different source image.
  const secondExample = page.locator('.example-thumb').nth(1)
  await secondExample.scrollIntoViewIfNeeded()
  await secondExample.click()

  // Poll for the canvas to actually change. HAM6 cold-start on a CI
  // runner can take well over 2 s; a fixed timeout was flaky.
  await expect.poll(
    () => canvas.evaluate(sample),
    { timeout: 20_000, intervals: [200, 500, 1000] }
  ).not.toBe(before)
})
