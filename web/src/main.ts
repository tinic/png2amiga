import { createApp } from 'vue'
import PrimeVue from 'primevue/config'
import Tooltip from 'primevue/tooltip'
import Aura from '@primevue/themes/aura'

// Self-hosted Inter — Vite bundles the WOFF2 files and rewrites the
// @font-face URLs to fingerprinted same-origin paths under /assets/.
// Removes fonts.googleapis.com / fonts.gstatic.com as runtime
// dependencies, eliminating two CSP allowlist entries and the SRI gap
// on per-UA-varying Google Fonts CSS.
//
// Latin subset only. The full @fontsource/inter ships cyrillic,
// cyrillic-ext, greek, vietnamese, latin, latin-ext (6 subsets × 4
// weights × 2 formats = 48 files). The English-only UI text never
// triggers the non-latin unicode-ranges, but the build still publishes
// them. Importing the per-weight latin CSS keeps the bundle to 8 files.
import '@fontsource/inter/latin-400.css'
import '@fontsource/inter/latin-500.css'
import '@fontsource/inter/latin-600.css'
import '@fontsource/inter/latin-700.css'
import 'primeicons/primeicons.css'
import 'primeflex/primeflex.css'
import App from './App.vue'
import { prewarmWasm } from './composables/useWasm.js'

// Kick off the WASM worker before Vue mounts so the fetch + streaming
// compile of png2amiga.wasm runs in parallel with Vue/PrimeVue bootstrap
// instead of after Converter.vue's setup runs. Saves ~100–300 ms on
// first-paint-to-first-convert on typical broadband.
prewarmWasm()

const app = createApp(App)
app.directive('tooltip', Tooltip)
app.use(PrimeVue, {
  theme: {
    preset: Aura,
    options: {
      darkModeSelector: '.dark-mode',
    }
  }
})
app.mount('#app')
