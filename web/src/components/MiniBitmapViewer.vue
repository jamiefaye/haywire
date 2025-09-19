<template>
  <div
    :id="`mini-viewer-${id}`"
    class="mini-bitmap-viewer"
    :style="{
      left: `${position.x}px`,
      top: `${position.y}px`,
      width: `${size.width}px`,
      height: `${size.height + 40}px`
    }"
    @mousedown="startDrag"
  >
    <!-- Title Bar -->
    <div class="viewer-header">
      <button class="settings-btn" @click.stop="toggleSettings" title="Settings">☰</button>
      <span class="viewer-title">{{ title }}</span>
      <button class="close-btn" @click="$emit('close')">×</button>
    </div>

    <!-- Settings Popup -->
    <div v-if="showSettings" class="settings-popup" @click.stop>
      <div class="settings-section">
        <label class="settings-label">Anchor Mode:</label>
        <select v-model="anchorMode" class="settings-select">
          <option value="address">Stick to Address</option>
          <option value="position">Stick to Position</option>
        </select>
      </div>

      <div class="settings-section">
        <label class="settings-label">Width:</label>
        <input
          type="number"
          v-model.number="localWidth"
          @change="updateConfig"
          class="settings-input"
          min="32"
          max="2048"
        >
      </div>

      <div class="settings-section">
        <label class="settings-label">Height:</label>
        <input
          type="number"
          v-model.number="localHeight"
          @change="updateConfig"
          class="settings-input"
          min="32"
          max="2048"
        >
      </div>

      <div class="settings-section">
        <label class="settings-label">Format:</label>
        <select v-model.number="localFormat" @change="updateConfig" class="settings-select">
          <option :value="PixelFormat.GRAYSCALE">Grayscale</option>
          <option :value="PixelFormat.RGB565">RGB565</option>
          <option :value="PixelFormat.RGB888">RGB888</option>
          <option :value="PixelFormat.RGBA8888">RGBA8888</option>
          <option :value="PixelFormat.BGR888">BGR888</option>
          <option :value="PixelFormat.BGRA8888">BGRA8888</option>
          <option :value="PixelFormat.ARGB8888">ARGB8888</option>
          <option :value="PixelFormat.ABGR8888">ABGR8888</option>
          <option :value="PixelFormat.BINARY">Binary</option>
          <option :value="PixelFormat.HEX_PIXEL">Hex Pixel</option>
          <option :value="PixelFormat.CHAR_8BIT">Char 8-bit</option>
        </select>
      </div>

      <div class="settings-section">
        <label class="settings-checkbox">
          <input
            type="checkbox"
            v-model="localSplitComponents"
            @change="updateConfig"
          >
          Split Components
        </label>
      </div>

      <div class="settings-section">
        <label class="settings-checkbox">
          <input
            type="checkbox"
            v-model="localColumnMode"
            @change="updateConfig"
          >
          Column Mode
        </label>
      </div>

      <div v-if="localColumnMode" class="settings-section">
        <label class="settings-label">Col Width:</label>
        <input
          type="number"
          v-model.number="localColumnWidth"
          @change="updateConfig"
          class="settings-input"
          min="16"
          max="512"
        >
      </div>

      <div v-if="localColumnMode" class="settings-section">
        <label class="settings-label">Col Gap:</label>
        <input
          type="number"
          v-model.number="localColumnGap"
          @change="updateConfig"
          class="settings-input"
          min="0"
          max="32"
        >
      </div>
    </div>

    <!-- Controls -->
    <div class="viewer-controls">
      <select v-model.number="localFormat" class="format-select">
        <option :value="PixelFormat.RGB888">RGB888</option>
        <option :value="PixelFormat.RGBA8888">RGBA8888</option>
        <option :value="PixelFormat.BGR888">BGR888</option>
        <option :value="PixelFormat.BGRA8888">BGRA8888</option>
        <option :value="PixelFormat.ARGB8888">ARGB8888</option>
        <option :value="PixelFormat.ABGR8888">ABGR8888</option>
        <option :value="PixelFormat.RGB565">RGB565</option>
        <option :value="PixelFormat.GRAYSCALE">Grayscale</option>
        <option :value="PixelFormat.BINARY">Binary</option>
        <option :value="PixelFormat.HEX_PIXEL">Hex Pixel</option>
        <option :value="PixelFormat.CHAR_8BIT">Char 8-bit</option>
      </select>

      <label class="checkbox-label">
        <input type="checkbox" v-model="localSplitComponents">
        Split
      </label>

      <input
        type="number"
        v-model.number="localWidth"
        class="dimension-input"
        min="1"
        max="1024"
        title="Width"
      >
      ×
      <input
        type="number"
        v-model.number="localHeight"
        class="dimension-input"
        min="1"
        max="1024"
        title="Height"
      >
    </div>

    <!-- Canvas -->
    <canvas
      ref="canvasRef"
      :width="size.width"
      :height="size.height"
      class="viewer-canvas"
      @mousedown.stop
    />

    <!-- Resize Handle -->
    <div
      class="resize-handle"
      @mousedown.stop="startResize"
    />
  </div>

  <!-- Leader Line (drawn in SVG overlay) -->
  <svg
    v-if="showLeader && anchorPoint"
    class="leader-line-svg"
    :style="{
      position: 'fixed',
      top: 0,
      left: 0,
      width: '100%',
      height: '100%',
      pointerEvents: 'none',
      zIndex: 999
    }"
  >
    <line
      :x1="anchorPoint.x"
      :y1="anchorPoint.y"
      :x2="position.x + size.width / 2"
      :y2="position.y + 20"
      stroke="#00ff00"
      stroke-width="2"
      stroke-dasharray="5,5"
      opacity="0.7"
    />
    <circle
      :cx="anchorPoint.x"
      :cy="anchorPoint.y"
      r="8"
      fill="#00ff00"
      opacity="0.8"
      :style="{ pointerEvents: 'auto', cursor: isDraggingAnchor ? 'grabbing' : 'grab' }"
      @mousedown.stop="startAnchorDrag"
    />
  </svg>
</template>

<script setup lang="ts">
import { ref, watch, onMounted, onUnmounted, nextTick } from 'vue'
import { useWasmRenderer, PixelFormat } from '../composables/useWasmRenderer'

interface Props {
  id: number
  memoryData: Uint8Array
  offset: number
  initialWidth?: number
  initialHeight?: number
  initialFormat?: PixelFormat
  initialSplitComponents?: boolean
  initialColumnMode?: boolean
  initialColumnWidth?: number
  initialColumnGap?: number
  title?: string
  anchorPoint?: { x: number, y: number } | null
  showLeader?: boolean
  initialAnchorMode?: 'address' | 'position'
  relativeAnchorPos?: { x: number, y: number }
}

const props = withDefaults(defineProps<Props>(), {
  initialWidth: 256,
  initialHeight: 256,
  initialFormat: PixelFormat.BGR888,
  initialSplitComponents: false,
  initialColumnMode: false,
  initialColumnWidth: 64,
  initialColumnGap: 4,
  title: 'Bitmap Viewer',
  anchorPoint: null,
  showLeader: true,
  initialAnchorMode: 'address',
  relativeAnchorPos: () => ({ x: 0.5, y: 0.5 })
})

const emit = defineEmits<{
  close: []
  updateConfig: [config: {
    width: number,
    height: number,
    format: PixelFormat,
    splitComponents: boolean,
    columnMode: boolean,
    columnWidth: number,
    columnGap: number
  }]
  dragStateChanged: [dragging: boolean]
  anchorDrag: [position: { x: number, y: number }]
  anchorModeChanged: [mode: 'address' | 'position', relativePos?: { x: number, y: number }]
}>()

// Component state
const canvasRef = ref<HTMLCanvasElement>()
const position = ref({ x: 100, y: 100 })
const size = ref({ width: props.initialWidth, height: props.initialHeight })

// Settings popup
const showSettings = ref(false)
const anchorMode = ref<'address' | 'position'>(props.initialAnchorMode)

// Configuration state
const localWidth = ref(props.initialWidth)
const localHeight = ref(props.initialHeight)
const localFormat = ref(props.initialFormat)
const localSplitComponents = ref(props.initialSplitComponents)
const localColumnMode = ref(props.initialColumnMode)
const localColumnWidth = ref(props.initialColumnWidth)
const localColumnGap = ref(props.initialColumnGap)

// Dragging state
const isDragging = ref(false)
const dragStart = ref({ x: 0, y: 0 })

// Anchor dragging state
const isDraggingAnchor = ref(false)

// Resizing state
const isResizing = ref(false)
const resizeStart = ref({ x: 0, y: 0, width: 0, height: 0 })

// Use WASM renderer
const { renderMemory } = useWasmRenderer()

// Render the memory to canvas
function doRender() {
  if (props.memoryData && canvasRef.value) {
    renderMemory(props.memoryData, canvasRef.value, {
      sourceOffset: props.offset,
      displayWidth: localWidth.value,
      displayHeight: localHeight.value,
      stride: localWidth.value,
      format: localFormat.value,
      splitComponents: localSplitComponents.value,
      columnMode: localColumnMode.value,
      columnWidth: localColumnWidth.value,
      columnGap: localColumnGap.value
    })
  }
}

// Drag handling
function startDrag(e: MouseEvent) {
  if ((e.target as HTMLElement).classList.contains('viewer-header') ||
      (e.target as HTMLElement).classList.contains('viewer-title')) {
    isDragging.value = true
    emit('dragStateChanged', true)
    dragStart.value = {
      x: e.clientX - position.value.x,
      y: e.clientY - position.value.y
    }
    e.preventDefault()
  }
}

function handleMouseMove(e: MouseEvent) {
  if (isDragging.value) {
    position.value = {
      x: e.clientX - dragStart.value.x,
      y: e.clientY - dragStart.value.y
    }
  } else if (isResizing.value) {
    const newWidth = Math.max(128, resizeStart.value.width + (e.clientX - resizeStart.value.x))
    const newHeight = Math.max(128, resizeStart.value.height + (e.clientY - resizeStart.value.y))
    size.value = { width: newWidth, height: newHeight }
    localWidth.value = newWidth
    localHeight.value = newHeight
  } else if (isDraggingAnchor.value) {
    // When dragging anchor, update the viewer's offset based on mouse position
    emit('anchorDrag', { x: e.clientX, y: e.clientY })
  }
}

function handleMouseUp() {
  if (isDragging.value) {
    isDragging.value = false
    emit('dragStateChanged', false)
  }
  if (isResizing.value) {
    isResizing.value = false
    emit('dragStateChanged', false)
  }
  if (isDraggingAnchor.value) {
    isDraggingAnchor.value = false
    emit('dragStateChanged', false)
  }
}

// Resize handling
function startResize(e: MouseEvent) {
  isResizing.value = true
  emit('dragStateChanged', true)
  resizeStart.value = {
    x: e.clientX,
    y: e.clientY,
    width: size.value.width,
    height: size.value.height
  }
  e.preventDefault()
}

// Anchor dragging
function startAnchorDrag(e: MouseEvent) {
  isDraggingAnchor.value = true
  emit('dragStateChanged', true)
  e.preventDefault()
  e.stopPropagation()
}

// Settings popup
function toggleSettings() {
  showSettings.value = !showSettings.value

  // If opening settings, check if popup would go off screen and adjust
  if (showSettings.value) {
    nextTick(() => {
      const viewer = document.querySelector(`[id="mini-viewer-${props.id}"]`) as HTMLElement
      if (!viewer) return
      const popup = viewer.querySelector('.settings-popup') as HTMLElement
      if (popup) {
        const rect = popup.getBoundingClientRect()

        // If popup goes off right edge, position it to the left of viewer instead
        if (rect.right > window.innerWidth - 10) {
          popup.style.left = 'auto'
          popup.style.right = `calc(100% + 8px)`
        }

        // If popup goes off bottom, position it above the title bar
        if (rect.bottom > window.innerHeight - 10) {
          popup.style.top = 'auto'
          popup.style.bottom = `calc(100% - 28px)`
        }
      }
    })
  }
}

// Close settings when clicking outside
function handleClickOutside(e: MouseEvent) {
  const target = e.target as HTMLElement
  if (!target.closest('.settings-popup') && !target.closest('.settings-btn')) {
    showSettings.value = false
  }
}

// Watch anchor mode changes
watch(anchorMode, (newMode) => {
  if (newMode === 'position' && props.anchorPoint) {
    // Calculate relative position when switching to position mode
    const canvas = document.querySelector('.memory-canvas') as HTMLCanvasElement
    if (canvas) {
      const rect = canvas.getBoundingClientRect()
      const relX = (props.anchorPoint.x - rect.left) / rect.width
      const relY = (props.anchorPoint.y - rect.top) / rect.height
      emit('anchorModeChanged', newMode, { x: relX, y: relY })
    }
  } else {
    emit('anchorModeChanged', newMode)
  }
})

// Set initial random position
onMounted(() => {
  // Add click outside listener for settings popup
  document.addEventListener('click', handleClickOutside)
  // Position randomly within viewport
  const margin = 50
  position.value = {
    x: margin + Math.random() * (window.innerWidth - size.value.width - margin * 2),
    y: margin + Math.random() * (window.innerHeight - size.value.height - margin * 2 - 40)
  }

  // Add global event listeners
  document.addEventListener('mousemove', handleMouseMove)
  document.addEventListener('mouseup', handleMouseUp)

  // Initial render
  doRender()
})

onUnmounted(() => {
  document.removeEventListener('mousemove', handleMouseMove)
  document.removeEventListener('mouseup', handleMouseUp)
  document.removeEventListener('click', handleClickOutside)
})

// Watch for data changes
watch(() => props.memoryData, () => {
  doRender()
})

// Watch for offset changes
watch(() => props.offset, () => {
  doRender()
})

// Update config function
function updateConfig() {
  emit('updateConfig', {
    width: localWidth.value,
    height: localHeight.value,
    format: localFormat.value,
    splitComponents: localSplitComponents.value,
    columnMode: localColumnMode.value,
    columnWidth: localColumnWidth.value,
    columnGap: localColumnGap.value
  })
}

// Watch for dimension changes - auto-update when width or height changes
watch([localWidth, localHeight], () => {
  size.value = { width: localWidth.value, height: localHeight.value }
  updateConfig()
  // Use nextTick to ensure canvas is resized before rendering
  nextTick(() => {
    doRender()
  })
})

// Watch for format and column changes
watch([localFormat, localSplitComponents, localColumnMode, localColumnWidth, localColumnGap], () => {
  doRender()
  updateConfig()
})
</script>

<style scoped>
.mini-bitmap-viewer {
  position: fixed;
  background: #2d2d30;
  border: 1px solid #555;
  border-radius: 4px;
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.5);
  z-index: 1000;
  display: flex;
  flex-direction: column;
  user-select: none;
}

.viewer-header {
  background: #3c3c3c;
  padding: 4px 8px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  cursor: move;
  border-bottom: 1px solid #555;
}

.viewer-title {
  color: #e0e0e0;
  font-size: 12px;
  font-weight: bold;
}

.close-btn {
  background: none;
  border: none;
  color: #e0e0e0;
  font-size: 18px;
  cursor: pointer;
  padding: 0;
  width: 20px;
  height: 20px;
  line-height: 18px;
  text-align: center;
}

.close-btn:hover {
  color: #ff6b6b;
}

.viewer-controls {
  padding: 4px;
  background: #2d2d30;
  display: flex;
  gap: 4px;
  align-items: center;
  font-size: 11px;
  border-bottom: 1px solid #555;
}

.format-select {
  background: #3c3c3c;
  color: #e0e0e0;
  border: 1px solid #555;
  padding: 2px 4px;
  font-size: 11px;
  border-radius: 2px;
  cursor: pointer;
  flex: 1;
  max-width: 100px;
}

.checkbox-label {
  display: flex;
  align-items: center;
  gap: 2px;
  color: #e0e0e0;
  font-size: 11px;
}

.dimension-input {
  background: #3c3c3c;
  color: #e0e0e0;
  border: 1px solid #555;
  padding: 2px 4px;
  width: 48px;
  font-size: 11px;
  border-radius: 2px;
}

.viewer-canvas {
  flex: 1;
  image-rendering: pixelated;
  image-rendering: -moz-crisp-edges;
  image-rendering: crisp-edges;
  cursor: crosshair;
}

.resize-handle {
  position: absolute;
  bottom: 0;
  right: 0;
  width: 12px;
  height: 12px;
  cursor: nwse-resize;
  background: linear-gradient(135deg, transparent 0%, transparent 50%, #666 50%, #666 100%);
}

.leader-line-svg {
  pointer-events: none;
}

.settings-btn {
  background: transparent;
  border: 1px solid #555;
  color: #e0e0e0;
  font-size: 14px;
  width: 24px;
  height: 20px;
  padding: 0;
  cursor: pointer;
  border-radius: 2px;
}

.settings-btn:hover {
  background: #3c3c3c;
}

.settings-popup {
  position: absolute;
  top: 28px;
  left: calc(100% + 8px); /* Position to the right of the viewer */
  background: #2d2d30;
  border: 1px solid #555;
  border-radius: 4px;
  padding: 8px;
  z-index: 1000;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
  min-width: 200px;
  max-height: 400px;
  overflow-y: auto;
}

.settings-section {
  margin-bottom: 8px;
  display: flex;
  align-items: center;
  gap: 8px;
}

.settings-section:last-child {
  margin-bottom: 0;
}

.settings-label {
  font-size: 11px;
  color: #e0e0e0;
  min-width: 80px;
}

.settings-select,
.settings-input {
  flex: 1;
  background: #3c3c3c;
  border: 1px solid #555;
  color: #e0e0e0;
  padding: 2px 4px;
  border-radius: 2px;
  font-size: 11px;
}

.settings-checkbox {
  display: flex;
  align-items: center;
  gap: 4px;
  font-size: 11px;
  color: #e0e0e0;
  cursor: pointer;
}

.settings-checkbox input {
  cursor: pointer;
}
</style>