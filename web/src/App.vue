<script setup>
import Converter from './components/Converter.vue'
import { track } from './lib/analytics.js'

const version = __APP_VERSION__
</script>

<template>
  <div class="surface-ground min-h-screen">
    <div class="max-w-screen-xl mx-auto p-4">
      <header class="mb-5 pb-4 border-bottom-1 surface-border">
        <div class="flex align-items-center gap-3">
          <img src="/boing.svg" alt="Amiga Boing Ball" class="logo" />
          <div>
            <h1 class="m-0 title">png2amiga<span class="version">v{{ version }}</span></h1>
            <p class="m-0 mt-1 text-sm text-color-secondary">
              Convert images to Commodore Amiga OCS/AGA formats with perceptual color matching
              — <a href="https://github.com/tinic/png2amiga" target="_blank" class="github-link" @click="track('github-click')">GitHub <i class="pi pi-github"></i></a>
              — Sister project: <a href="https://png2c64.app" target="_blank" class="github-link">png2c64</a>
            </p>
          </div>
        </div>
      </header>
      <Converter />
    </div>
  </div>
</template>

<style scoped>
.logo {
  width: 3rem;
  height: 3rem;
  flex-shrink: 0;
}

.title {
  font-size: 1.5rem;
  font-weight: 700;
  letter-spacing: -0.03em;
  background: linear-gradient(135deg, #e0e0e0, #a0a0a0);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  background-clip: text;
}

.version {
  margin-left: 0.5rem;
  font-size: 0.7rem;
  font-weight: 400;
  letter-spacing: 0;
  color: #888;
  -webkit-text-fill-color: #888;
  vertical-align: middle;
}

.github-link {
  color: inherit;
  text-decoration: none;
  opacity: 0.7;
  transition: opacity 0.15s;
}
.github-link:hover {
  opacity: 1;
}
</style>
