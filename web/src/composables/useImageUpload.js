import { ref, onUnmounted } from 'vue'
import { track } from '../lib/analytics.js'

export function useImageUpload() {
  const imageBytes = ref(null)
  const imageName = ref('')
  const imageUrl = ref(null)
  const dragOver = ref(false)
  const uploadTimestamp = ref(0)

  function revokeUrl() {
    if (imageUrl.value) {
      URL.revokeObjectURL(imageUrl.value)
      imageUrl.value = null
    }
  }

  async function handleFiles(files) {
    if (!files.length) return
    const file = files[0]
    if (!file.type.startsWith('image/')) return
    const buf = await file.arrayBuffer()
    imageBytes.value = new Uint8Array(buf)
    imageName.value = file.name
    revokeUrl()
    imageUrl.value = URL.createObjectURL(file)
    uploadTimestamp.value = Date.now()
    const img = new Image()
    img.onload = () => {
      track('upload', { type: file.type, size: Math.round(file.size / 1024), width: img.width, height: img.height })
    }
    img.src = imageUrl.value
  }

  function onDrop(e) {
    e.preventDefault()
    dragOver.value = false
    handleFiles(e.dataTransfer.files)
  }

  function onDragOver(e) {
    e.preventDefault()
    dragOver.value = true
  }

  function onDragLeave() {
    dragOver.value = false
  }

  function openPicker() {
    const input = document.createElement('input')
    input.type = 'file'
    input.accept = 'image/*'
    input.onchange = () => handleFiles(input.files)
    input.click()
  }

  onUnmounted(revokeUrl)

  return { imageBytes, imageName, imageUrl, dragOver, uploadTimestamp, onDrop, onDragOver, onDragLeave, openPicker }
}
