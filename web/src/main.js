import { createApp } from 'vue'
import PrimeVue from 'primevue/config'
import Tooltip from 'primevue/tooltip'
import Aura from '@primevue/themes/aura'
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
