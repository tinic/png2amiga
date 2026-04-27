// Umami event tracking helper
// https://umami.is/docs/tracker-functions

interface Umami {
  track(eventName: string, data?: Record<string, unknown>): void
}

declare global {
  // `var` is required inside `declare global` for global augmentation.
  var umami: Umami | undefined
}

export function track(eventName: string, data?: Record<string, unknown>): void {
  globalThis.umami?.track(eventName, data)
}
