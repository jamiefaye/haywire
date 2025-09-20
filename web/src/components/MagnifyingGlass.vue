<template>
  <div
    v-if="visible"
    class="magnifying-glass"
    :style="containerStyle"
    @mousedown="startDrag"
  >
    <div class="header" @mousedown="startMove">
      <span class="title">Magnifier {{ zoom }}x{{ locked ? ' 🔒' : '' }}</span>
      <div class="controls">
        <select v-model="zoom" class="zoom-select">
          <option v-for="z in zoomLevels" :key="z" :value="z">{{ z }}x</option>
        </select>
        <button @click="close" class="close-btn">×</button>
      </div>
    </div>
    <canvas
      ref="canvas"
      :width="canvasWidth"
      :height="canvasHeight"
    />
    <div class="resize-handle" @mousedown="startResize"></div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted, nextTick } from 'vue'

const props = defineProps<{
  sourceCanvas: HTMLCanvasElement | null
  centerX: number
  centerY: number
  visible: boolean
  initialLocked?: boolean
}>()

const emit = defineEmits<{
  close: []
  update: [x: number, y: number]
}>()

const canvas = ref<HTMLCanvasElement>()
const zoom = ref(4)
const zoomLevels = [2, 3, 4, 5, 6, 8, 10, 12, 16]
const locked = ref(props.initialLocked || false)

const width = ref(200)
const height = ref(200)
const posX = ref(100)
const posY = ref(100)

const canvasWidth = computed(() => width.value)
const canvasHeight = computed(() => height.value)

const containerStyle = computed(() => ({
  left: `${posX.value}px`,
  top: `${posY.value}px`,
  width: `${width.value}px`,
  height: `${height.value + 30}px`,
  zIndex: 1000
}))

let isDragging = false
let isResizing = false
let isMoving = false
let dragStartX = 0
let dragStartY = 0
let startWidth = 0
let startHeight = 0
let startPosX = 0
let startPosY = 0

function startMove(e: MouseEvent) {
  if (e.target !== e.currentTarget && !(e.target as HTMLElement).classList.contains('title')) return
  isMoving = true
  dragStartX = e.clientX
  dragStartY = e.clientY
  startPosX = posX.value
  startPosY = posY.value
  e.preventDefault()
}

function startDrag(e: MouseEvent) {
  const target = e.target as HTMLElement
  if (target.classList.contains('resize-handle')) return
  if (target.closest('.header')) return
}

function startResize(e: MouseEvent) {
  isResizing = true
  dragStartX = e.clientX
  dragStartY = e.clientY
  startWidth = width.value
  startHeight = height.value
  e.preventDefault()
}

function onMouseMove(e: MouseEvent) {
  if (!locked.value && !isMoving && !isResizing && props.visible) {
    // Update position based on mouse
    emit('update', e.clientX, e.clientY)
  }

  if (isMoving) {
    const dx = e.clientX - dragStartX
    const dy = e.clientY - dragStartY
    posX.value = startPosX + dx
    posY.value = startPosY + dy
  }

  if (isResizing) {
    const dx = e.clientX - dragStartX
    const dy = e.clientY - dragStartY
    width.value = Math.max(100, startWidth + dx)
    height.value = Math.max(100, startHeight + dy)
  }
}

function onMouseUp() {
  isMoving = false
  isResizing = false
}

function close() {
  emit('close')
}

function draw() {
  const ctx = canvas.value?.getContext('2d', { imageSmoothingEnabled: false })
  const sourceCtx = props.sourceCanvas?.getContext('2d', { imageSmoothingEnabled: false })
  if (!ctx || !sourceCtx || !props.sourceCanvas) return

  // Clear canvas
  ctx.fillStyle = '#000'
  ctx.fillRect(0, 0, canvasWidth.value, canvasHeight.value)

  // Calculate source region size (in pixels)
  const sourcePixelsX = Math.ceil(width.value / zoom.value)
  const sourcePixelsY = Math.ceil(height.value / zoom.value)

  // Calculate top-left of source region
  const sourceLeft = Math.max(0, props.centerX - Math.floor(sourcePixelsX / 2))
  const sourceTop = Math.max(0, props.centerY - Math.floor(sourcePixelsY / 2))

  // Get the source image data from the main canvas
  const sourceWidth = Math.min(sourcePixelsX, props.sourceCanvas.width - sourceLeft)
  const sourceHeight = Math.min(sourcePixelsY, props.sourceCanvas.height - sourceTop)

  if (sourceWidth <= 0 || sourceHeight <= 0) return

  try {
    const imageData = sourceCtx.getImageData(sourceLeft, sourceTop, sourceWidth, sourceHeight)

    // Create a temporary canvas to hold the source data
    const tempCanvas = document.createElement('canvas')
    tempCanvas.width = sourceWidth
    tempCanvas.height = sourceHeight
    const tempCtx = tempCanvas.getContext('2d', { imageSmoothingEnabled: false })!
    tempCtx.putImageData(imageData, 0, 0)

    // Draw scaled up version
    ctx.imageSmoothingEnabled = false
    ctx.drawImage(tempCanvas, 0, 0, sourceWidth, sourceHeight,
                  0, 0, sourceWidth * zoom.value, sourceHeight * zoom.value)
  } catch (e) {
    console.error('Failed to get image data:', e)
    return
  }

  // Draw grid lines for zoom >= 3x
  if (zoom.value >= 3) {
    ctx.strokeStyle = 'rgba(128, 128, 128, 0.3)'
    ctx.lineWidth = 1

    // Vertical lines
    for (let x = 0; x <= sourcePixelsX; x++) {
      ctx.beginPath()
      ctx.moveTo(x * zoom.value, 0)
      ctx.lineTo(x * zoom.value, height.value)
      ctx.stroke()
    }

    // Horizontal lines
    for (let y = 0; y <= sourcePixelsY; y++) {
      ctx.beginPath()
      ctx.moveTo(0, y * zoom.value)
      ctx.lineTo(width.value, y * zoom.value)
      ctx.stroke()
    }
  }
}

// Keyboard handler
function onKeyDown(e: KeyboardEvent) {
  if (!props.visible) return

  if (e.key === 'l' || e.key === 'L') {
    locked.value = !locked.value
  } else if (e.key === 'Escape') {
    close()
  } else if (e.key === '+' || e.key === '=') {
    const currentIndex = zoomLevels.indexOf(zoom.value)
    if (currentIndex < zoomLevels.length - 1) {
      zoom.value = zoomLevels[currentIndex + 1]
    }
  } else if (e.key === '-') {
    const currentIndex = zoomLevels.indexOf(zoom.value)
    if (currentIndex > 0) {
      zoom.value = zoomLevels[currentIndex - 1]
    }
  }
}

// Watch for prop changes to trigger redraw
watch([() => props.sourceCanvas, () => props.centerX, () => props.centerY, zoom, width, height], () => {
  nextTick(() => draw())
})

// Update locked state when prop changes
watch(() => props.initialLocked, (newVal) => {
  if (newVal !== undefined) {
    locked.value = newVal
  }
})

onMounted(() => {
  document.addEventListener('mousemove', onMouseMove)
  document.addEventListener('mouseup', onMouseUp)
  document.addEventListener('keydown', onKeyDown)

  // Position near mouse initially
  posX.value = props.centerX + 20
  posY.value = props.centerY + 20

  draw()
})

onUnmounted(() => {
  document.removeEventListener('mousemove', onMouseMove)
  document.removeEventListener('mouseup', onMouseUp)
  document.removeEventListener('keydown', onKeyDown)
})
</script>

<style scoped>
.magnifying-glass {
  position: fixed;
  background: #1e1e1e;
  border: 1px solid #444;
  border-radius: 4px;
  box-shadow: 0 4px 8px rgba(0, 0, 0, 0.5);
  overflow: hidden;
  display: flex;
  flex-direction: column;
}

.header {
  height: 30px;
  background: #2a2a2a;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 8px;
  cursor: move;
  user-select: none;
}

.title {
  color: #ccc;
  font-size: 12px;
  font-family: monospace;
}

.controls {
  display: flex;
  gap: 8px;
  align-items: center;
}

.zoom-select {
  background: #1e1e1e;
  color: #ccc;
  border: 1px solid #444;
  border-radius: 2px;
  font-size: 11px;
  padding: 2px 4px;
}

button {
  background: #333;
  color: #ccc;
  border: 1px solid #444;
  border-radius: 2px;
  padding: 2px 8px;
  cursor: pointer;
  font-size: 12px;
}

button:hover {
  background: #444;
}

button.active {
  background: #555;
  border-color: #666;
}

.close-btn {
  padding: 2px 6px;
  font-size: 16px;
  line-height: 1;
}

canvas {
  flex: 1;
  image-rendering: pixelated;
}

.resize-handle {
  position: absolute;
  bottom: 0;
  right: 0;
  width: 12px;
  height: 12px;
  cursor: nwse-resize;
  background: linear-gradient(135deg, transparent 50%, #666 50%);
}
</style>