<template>
  <div class="memory-canvas-container">
    <canvas
      ref="canvasRef"
      :width="width"
      :height="height"
      @click="handleClick"
      @mousemove="handleMouseMove"
      class="memory-canvas"
    />
    <div v-if="isLoading" class="loading-overlay">
      Loading WASM renderer...
    </div>
    <div v-if="error" class="error-overlay">
      {{ error }}
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, watch, onMounted, nextTick } from 'vue'
import { useWasmRenderer, PixelFormat } from '../composables/useWasmRenderer'

interface Props {
  memoryData: Uint8Array | null
  width?: number
  height?: number
  format?: PixelFormat
  sourceOffset?: number
  stride?: number
  splitComponents?: boolean
  columnMode?: boolean
  columnWidth?: number
  columnGap?: number
}

const props = withDefaults(defineProps<Props>(), {
  width: 1024,
  height: 768,
  format: PixelFormat.BGR888,
  sourceOffset: 0,
  stride: 0,
  splitComponents: false,
  columnMode: false,
  columnWidth: 256,
  columnGap: 8
})

const emit = defineEmits<{
  memoryClick: [offset: number]
  memoryHover: [offset: number]
}>()

const canvasRef = ref<HTMLCanvasElement>()
const { isLoading, error, renderMemory, pixelToMemoryCoordinate } = useWasmRenderer()

// Helper to get base format and split flag
function getFormatAndSplit(format: PixelFormat): { baseFormat: PixelFormat, split: boolean } {
  switch (format) {
    case PixelFormat.RGBA8888_SPLIT:
      return { baseFormat: PixelFormat.RGBA8888, split: true }
    case PixelFormat.BGRA8888_SPLIT:
      return { baseFormat: PixelFormat.BGRA8888, split: true }
    case PixelFormat.ARGB8888_SPLIT:
      return { baseFormat: PixelFormat.ARGB8888, split: true }
    case PixelFormat.ABGR8888_SPLIT:
      return { baseFormat: PixelFormat.ABGR8888, split: true }
    default:
      return { baseFormat: format, split: false }
  }
}

// Function to trigger render
function doRender() {
  if (props.memoryData && canvasRef.value && !isLoading.value) {
    const { baseFormat, split } = getFormatAndSplit(props.format)
    renderMemory(props.memoryData, canvasRef.value, {
      sourceOffset: props.sourceOffset,
      displayWidth: props.width,
      displayHeight: props.height,
      stride: props.stride || props.width,
      format: baseFormat,
      splitComponents: props.splitComponents || split,
      columnMode: props.columnMode,
      columnWidth: props.columnWidth,
      columnGap: props.columnGap
    })
  }
}

// Re-render when memory data changes
watch(() => props.memoryData, () => {
  nextTick(doRender)
}, { deep: true })

// Also re-render when other props change
watch([
  () => props.format,
  () => props.splitComponents,
  () => props.columnMode,
  () => props.columnWidth,
  () => props.columnGap,
  () => props.sourceOffset
], () => {
  nextTick(doRender)
})

// Handle mouse clicks
function handleClick(event: MouseEvent) {
  try {
    if (!canvasRef.value || isLoading.value || !props.memoryData) return

    const rect = canvasRef.value.getBoundingClientRect()
    const x = event.clientX - rect.left
    const y = event.clientY - rect.top

    const { baseFormat, split } = getFormatAndSplit(props.format)
    const coords = pixelToMemoryCoordinate(
      x, y,
      props.width, props.height,
      props.stride || props.width,
      baseFormat,
      props.splitComponents || split,
      props.columnMode,
      props.columnWidth,
      props.columnGap
    )

    if (coords.x >= 0 && coords.y >= 0) {
      // Calculate memory offset
      const bytesPerPixel = getBytesPerPixelForFormat(baseFormat)
      const offset = coords.y * (props.stride || props.width) * bytesPerPixel + coords.x * bytesPerPixel
      emit('memoryClick', props.sourceOffset + offset)
    }
  } catch (err) {
    console.warn('Error in handleClick:', err)
  }
}

// Handle mouse movement
function handleMouseMove(event: MouseEvent) {
  try {
    if (!canvasRef.value || isLoading.value || !props.memoryData) return

    const rect = canvasRef.value.getBoundingClientRect()
    const x = event.clientX - rect.left
    const y = event.clientY - rect.top

    const { baseFormat, split } = getFormatAndSplit(props.format)
    const coords = pixelToMemoryCoordinate(
      x, y,
      props.width, props.height,
      props.stride || props.width,
      baseFormat,
      props.splitComponents || split,
      props.columnMode,
      props.columnWidth,
      props.columnGap
    )

    if (coords.x >= 0 && coords.y >= 0) {
      const bytesPerPixel = getBytesPerPixelForFormat(baseFormat)
      const offset = coords.y * (props.stride || props.width) * bytesPerPixel + coords.x * bytesPerPixel
      emit('memoryHover', props.sourceOffset + offset)
    }
  } catch (err) {
    console.warn('Error in handleMouseMove:', err)
  }
}

// Helper function for bytes per pixel
function getBytesPerPixelForFormat(format: PixelFormat): number {
  // This should match the C++ implementation
  switch (format) {
    case PixelFormat.GRAYSCALE:
    case PixelFormat.BINARY:
    case PixelFormat.CHAR_8BIT:
      return 1
    case PixelFormat.RGB565:
      return 2
    case PixelFormat.RGB888:
    case PixelFormat.BGR888:
      return 3
    case PixelFormat.RGBA8888:
    case PixelFormat.BGRA8888:
    case PixelFormat.ARGB8888:
    case PixelFormat.ABGR8888:
    case PixelFormat.HEX_PIXEL:
    case PixelFormat.RGBA8888_SPLIT:
    case PixelFormat.BGRA8888_SPLIT:
    case PixelFormat.ARGB8888_SPLIT:
    case PixelFormat.ABGR8888_SPLIT:
      return 4
    default:
      return 1
  }
}

// Expose the canvas element for other components to use
defineExpose({
  canvas: canvasRef
})

// Initial render when mounted
onMounted(() => {
  // Wait a tick to ensure DOM is ready
  nextTick(doRender)

  // Also watch for WASM module loading
  watch(isLoading, (loading) => {
    if (!loading) {
      nextTick(doRender)
    }
  })
})
</script>

<style scoped>
.memory-canvas-container {
  position: relative;
  display: inline-block;
}

.memory-canvas {
  border: 1px solid #333;
  cursor: crosshair;
  image-rendering: pixelated;
  image-rendering: -moz-crisp-edges;
  image-rendering: crisp-edges;
}

.loading-overlay,
.error-overlay {
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  padding: 20px;
  background: rgba(0, 0, 0, 0.8);
  color: white;
  border-radius: 8px;
  font-family: monospace;
}

.error-overlay {
  background: rgba(255, 0, 0, 0.8);
}
</style>