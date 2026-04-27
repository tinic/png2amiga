// Umami event tracking helper
// https://umami.is/docs/tracker-functions

export function track(eventName, data) {
  if (globalThis.umami !== undefined) {
    globalThis.umami.track(eventName, data)
  }
}
