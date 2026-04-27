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

  async function handleFiles(files: FileList | null): Promise<void> {
    if (!files || files.length === 0) return
    const file = files[0]
    if (!file || !file.type.startsWith('image/')) return
    const buf = await file.arrayBuffer()
    imageBytes.value = new Uint8Array(buf)
    imageName.value = file.name
    revokeUrl()
    imageUrl.value = URL.createObjectURL(file)
    uploadTimestamp.value = Date.now()
    const img = new Image()
    img.addEventListener('load', () => {
      imageWidth.value = img.width
      imageHeight.value = img.height
      track('upload', { type: file.type, size: Math.round(file.size / 1024), width: img.width, height: img.height })
    })
    img.src = imageUrl.value
  }

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

  onUnmounted(revokeUrl)

  return { imageBytes, imageName, imageUrl, imageWidth, imageHeight, dragOver, uploadTimestamp, onDrop, onDragOver, onDragLeave, openPicker }
}
