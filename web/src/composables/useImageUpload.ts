import { ref, onUnmounted, type Ref } from 'vue'

import { track } from '../lib/analytics.js'

export interface UseImageUploadReturn {
  imageBytes: Ref<Uint8Array | null>
  imageName: Ref<string>
  imageUrl: Ref<string | null>
  imageWidth: Ref<number>
  imageHeight: Ref<number>
  dragOver: Ref<boolean>
  uploadTimestamp: Ref<number>
  onDrop: (e: DragEvent) => void
  onDragOver: (e: DragEvent) => void
  onDragLeave: () => void
  openPicker: () => void
  pasteFromClipboard: () => Promise<boolean>
}

export function useImageUpload(): UseImageUploadReturn {
  const imageBytes = ref<Uint8Array | null>(null)
  const imageName = ref('')
  const imageUrl = ref<string | null>(null)
  const imageWidth = ref(0)
  const imageHeight = ref(0)
  const dragOver = ref(false)
  const uploadTimestamp = ref(0)

  function revokeUrl(): void {
    if (imageUrl.value) {
      URL.revokeObjectURL(imageUrl.value)
      imageUrl.value = null
    }
  }

  async function handleBlob(blob: Blob, name: string): Promise<void> {
    if (!blob.type.startsWith('image/')) return
    const buf = await blob.arrayBuffer()
    imageBytes.value = new Uint8Array(buf)
    imageName.value = name
    revokeUrl()
    imageUrl.value = URL.createObjectURL(blob)
    uploadTimestamp.value = Date.now()
    const img = new Image()
    img.addEventListener('load', () => {
      imageWidth.value = img.width
      imageHeight.value = img.height
      track('upload', { type: blob.type, size: Math.round(blob.size / 1024), width: img.width, height: img.height })
    })
    img.src = imageUrl.value
  }

  async function handleFiles(files: FileList | null): Promise<void> {
    if (!files || files.length === 0) return
    const file = files[0]
    if (file) await handleBlob(file, file.name)
  }

  // Async Clipboard API path (button click). Firefox has no
  // clipboard.read() — the paste-event listener below covers it there.
  async function pasteFromClipboard(): Promise<boolean> {
    if (typeof navigator.clipboard.read !== 'function') return false
    let items: ClipboardItems
    try {
      items = await navigator.clipboard.read()
    } catch {
      return false
    }
    for (const item of items) {
      const type = item.types.find((t) => t.startsWith('image/'))
      if (!type) continue
      const blob = await item.getType(type)
      await handleBlob(blob, `clipboard.${type.split('/', 2)[1] ?? 'png'}`)
      return true
    }
    return false
  }

  // Ctrl/Cmd+V anywhere on the page, except while typing in a field.
  function isEditableTarget(t: EventTarget | null): boolean {
    const el = t instanceof HTMLElement ? t : null
    return el !== null && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable)
  }

  function onPaste(e: ClipboardEvent): void {
    if (isEditableTarget(e.target)) return
    const items = [...(e.clipboardData?.items ?? [])]
    const file = items
      .filter((i) => i.kind === 'file' && i.type.startsWith('image/'))
      .map((i) => i.getAsFile())
      .find((f) => f !== null)
    if (!file) return
    e.preventDefault()
    void handleBlob(file, file.name || 'clipboard.png')
  }
  window.addEventListener('paste', onPaste)

  function onDrop(e: DragEvent): void {
    e.preventDefault()
    dragOver.value = false
    if (e.dataTransfer) void handleFiles(e.dataTransfer.files)
  }

  function onDragOver(e: DragEvent): void {
    e.preventDefault()
    dragOver.value = true
  }

  function onDragLeave(): void {
    dragOver.value = false
  }

  function openPicker(): void {
    const input = document.createElement('input')
    input.type = 'file'
    input.accept = 'image/*'
    input.addEventListener('change', () => { void handleFiles(input.files) })
    input.click()
  }

  onUnmounted(() => {
    window.removeEventListener('paste', onPaste)
    revokeUrl()
  })

  return { imageBytes, imageName, imageUrl, imageWidth, imageHeight, dragOver, uploadTimestamp, onDrop, onDragOver, onDragLeave, openPicker, pasteFromClipboard }
}
