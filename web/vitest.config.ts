import { defineConfig } from 'vitest/config'

// Vitest config — kept separate from vite.config.ts so we can exclude the
// Playwright e2e directory without confusing the production build pipeline.
// Vitest automatically picks up *.test.ts and *.spec.ts under src/ and tests/;
// we exclude tests/e2e/ because those are Playwright specs (run by
// `npm run test:e2e`, not by `npm test`).
export default defineConfig({
  test: {
    include: ['src/**/*.{test,spec}.ts'],
    exclude: ['node_modules', 'tests/e2e/**', 'dist'],
  },
})
