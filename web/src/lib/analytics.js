// Umami event tracking helper
// https://umami.is/docs/tracker-functions

export function track(eventName, data) {
  if (typeof window.umami !== 'undefined') {
    window.umami.track(eventName, data)
  }
}
