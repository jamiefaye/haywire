<template>
  <div class="overview-pane" :style="{ width: width + 'px' }">
    <div class="overview-header">
      <div class="title">Memory Map</div>
      <div class="stats" v-if="state">
        <div class="stat">{{ formatSize(state.totalSize) }}</div>
        <div class="stat">{{ state.chunks.length }} chunks</div>
      </div>
    </div>

    <div class="overview-canvas-container">
      <canvas
        ref="canvasRef"
        :width="canvasWidth"
        :height="canvasHeight"
        @click="handleClick"
        @mousemove="handleMouseMove"
        @mouseleave="handleMouseLeave"
        class="overview-canvas"
      />

      <!-- Viewport indicator -->
      <div
        v-if="viewportIndicator"
        class="viewport-indicator"
        :style="viewportIndicatorStyle"
      />
    </div>

    <div class="overview-legend">
      <div class="legend-item">
        <span class="legend-color zero"></span>
        <span class="legend-label">Zero</span>
      </div>
      <div class="legend-item">
        <span class="legend-color changed"></span>
        <span class="legend-label">Changed</span>
      </div>
      <div class="legend-item">
        <span class="legend-color data"></span>
        <span class="legend-label">Data</span>
      </div>
    </div>

    <!-- Tooltip -->
    <div
      v-if="tooltip"
      class="overview-tooltip"
      :style="tooltipStyle"
    >
      <div>Offset: 0x{{ tooltip.offset.toString(16) }}</div>
      <div>Size: {{ formatSize(tooltip.size) }}</div>
      <div v-if="tooltip.isZero" class="zero-marker">ZERO</div>
      <div v-else>Checksum: 0x{{ tooltip.checksum.toString(16) }}</div>
      <div v-if="tooltip.hasChanged" class="changed-marker">CHANGED</div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import type { ChangeDetectionState, ChunkInfo } from '../composables/useChangeDetection'

interface Props {
  width?: number
  state: ChangeDetectionState | null
  currentOffset?: number
  viewSize?: number
}

const props = withDefaults(defineProps<Props>(), {
  width: 128,
  currentOffset: 0,
  viewSize: 1048576 // 1MB default view size
})

const emit = defineEmits<{
  jumpToOffset: [offset: number]
}>()

const canvasRef = ref<HTMLCanvasElement>()
const tooltip = ref<ChunkInfo | null>(null)
const tooltipStyle = ref<any>({})
const hoveredChunkIndex = ref<number>(-1)

// Canvas dimensions
const canvasWidth = computed(() => props.width - 16) // Leave padding
const canvasHeight = computed(() => {
  if (!props.state) return 400
  const rows = Math.ceil(props.state.chunks.length / columnsPerRow.value)
  return Math.min(rows * pixelSize.value, 600) // Cap at 600px height
})

// Visualization parameters
const columnsPerRow = computed(() => Math.floor(canvasWidth.value / pixelSize.value))
const pixelSize = ref(6) // Size of each chunk pixel

// Viewport indicator
const viewportIndicator = computed(() => {
  if (!props.state) return null

  const startChunk = Math.floor(props.currentOffset / props.state.chunkSize)
  const endChunk = Math.ceil((props.currentOffset + props.viewSize) / props.state.chunkSize)

  return {
    startChunk,
    endChunk,
    chunkCount: endChunk - startChunk
  }
})

const viewportIndicatorStyle = computed(() => {
  if (!viewportIndicator.value) return {}

  const { startChunk, chunkCount } = viewportIndicator.value
  const startRow = Math.floor(startChunk / columnsPerRow.value)
  const startCol = startChunk % columnsPerRow.value

  // Simple single-row indicator for now
  const top = startRow * pixelSize.value
  const left = startCol * pixelSize.value
  const width = Math.min(chunkCount, columnsPerRow.value - startCol) * pixelSize.value

  return {
    top: `${top}px`,
    left: `${left}px`,
    width: `${width}px`,
    height: `${pixelSize.value}px`
  }
})

// Render the overview
function render() {
  if (!canvasRef.value || !props.state) return

  const ctx = canvasRef.value.getContext('2d')
  if (!ctx) return

  // Clear canvas
  ctx.fillStyle = '#1a1a1a'
  ctx.fillRect(0, 0, canvasWidth.value, canvasHeight.value)

  // Draw chunks
  props.state.chunks.forEach((chunk, i) => {
    const row = Math.floor(i / columnsPerRow.value)
    const col = i % columnsPerRow.value
    const x = col * pixelSize.value
    const y = row * pixelSize.value

    // Choose color based on chunk state
    if (chunk.isZero) {
      ctx.fillStyle = '#202020' // Dark gray for zero
    } else if (chunk.hasChanged) {
      ctx.fillStyle = '#00ff00' // Bright green for changed
    } else {
      // Blue gradient based on checksum
      const intensity = (chunk.checksum % 128) + 128
      ctx.fillStyle = `rgb(0, ${intensity / 2}, ${intensity})`
    }

    // Draw chunk pixel
    ctx.fillRect(x, y, pixelSize.value - 1, pixelSize.value - 1)

    // Highlight hovered chunk
    if (i === hoveredChunkIndex.value) {
      ctx.strokeStyle = '#ffff00'
      ctx.lineWidth = 2
      ctx.strokeRect(x, y, pixelSize.value - 1, pixelSize.value - 1)
    }
  })
}

// Handle mouse click
function handleClick(event: MouseEvent) {
  if (!canvasRef.value || !props.state) return

  const rect = canvasRef.value.getBoundingClientRect()
  const x = event.clientX - rect.left
  const y = event.clientY - rect.top

  const col = Math.floor(x / pixelSize.value)
  const row = Math.floor(y / pixelSize.value)
  const chunkIndex = row * columnsPerRow.value + col

  if (chunkIndex < props.state.chunks.length) {
    const chunk = props.state.chunks[chunkIndex]
    emit('jumpToOffset', chunk.offset)
  }
}

// Handle mouse move
function handleMouseMove(event: MouseEvent) {
  if (!canvasRef.value || !props.state) return

  const rect = canvasRef.value.getBoundingClientRect()
  const x = event.clientX - rect.left
  const y = event.clientY - rect.top

  const col = Math.floor(x / pixelSize.value)
  const row = Math.floor(y / pixelSize.value)
  const chunkIndex = row * columnsPerRow.value + col

  if (chunkIndex < props.state.chunks.length) {
    hoveredChunkIndex.value = chunkIndex
    const chunk = props.state.chunks[chunkIndex]
    tooltip.value = chunk

    // Position tooltip
    tooltipStyle.value = {
      left: `${event.clientX + 10}px`,
      top: `${event.clientY - 40}px`
    }

    render() // Re-render to show highlight
  } else {
    handleMouseLeave()
  }
}

// Handle mouse leave
function handleMouseLeave() {
  hoveredChunkIndex.value = -1
  tooltip.value = null
  render()
}

// Format size for display
function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes}B`
  if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)}KB`
  if (bytes < 1073741824) return `${(bytes / 1048576).toFixed(1)}MB`
  return `${(bytes / 1073741824).toFixed(1)}GB`
}

// Watch for state changes
watch(() => props.state, render, { deep: true })
watch(() => props.currentOffset, render)

onMounted(() => {
  render()
})
</script>

<style scoped>
.overview-pane {
  background: #0a0a0a;
  border-right: 1px solid #333;
  display: flex;
  flex-direction: column;
  height: 100%;
  padding: 8px;
}

.overview-header {
  margin-bottom: 8px;
}

.title {
  color: #888;
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: 1px;
  margin-bottom: 4px;
}

.stats {
  display: flex;
  gap: 8px;
  font-size: 10px;
  color: #666;
}

.stat {
  white-space: nowrap;
}

.overview-canvas-container {
  position: relative;
  flex: 1;
  overflow-y: auto;
  overflow-x: hidden;
  margin-bottom: 8px;
}

.overview-canvas {
  image-rendering: pixelated;
  image-rendering: -moz-crisp-edges;
  image-rendering: crisp-edges;
  cursor: pointer;
}

.viewport-indicator {
  position: absolute;
  border: 2px solid #ff0;
  pointer-events: none;
  box-shadow: 0 0 10px rgba(255, 255, 0, 0.5);
}

.overview-legend {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding-top: 8px;
  border-top: 1px solid #333;
}

.legend-item {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 10px;
  color: #888;
}

.legend-color {
  width: 12px;
  height: 12px;
  border: 1px solid #444;
}

.legend-color.zero {
  background: #202020;
}

.legend-color.changed {
  background: #00ff00;
}

.legend-color.data {
  background: linear-gradient(90deg, #0060c0, #0080ff);
}

.overview-tooltip {
  position: fixed;
  background: rgba(0, 0, 0, 0.9);
  border: 1px solid #444;
  padding: 8px;
  border-radius: 4px;
  font-size: 11px;
  color: #ccc;
  pointer-events: none;
  z-index: 1000;
  font-family: monospace;
  white-space: nowrap;
}

.overview-tooltip div {
  margin: 2px 0;
}

.zero-marker {
  color: #666;
  font-weight: bold;
}

.changed-marker {
  color: #00ff00;
  font-weight: bold;
}
</style>