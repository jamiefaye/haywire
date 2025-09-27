<template>
  <div class="overview-pane" :style="{ width: width + 'px' }">
    <div class="overview-header">
      <div class="title">Memory Map</div>
      <div class="stats" v-if="state">
        <div class="stat">{{ formatSize(state.totalSize) }}</div>
        <div class="stat">{{ state.chunks.length }} chunks</div>
      </div>
    </div>

    <div class="overview-canvas-container" ref="canvasContainerRef">
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
const canvasContainerRef = ref<HTMLDivElement>()
const tooltip = ref<ChunkInfo | null>(null)
const tooltipStyle = ref<any>({})
const hoveredChunkIndex = ref<number>(-1)
const containerHeight = ref(600) // Default height, will be updated

// Canvas dimensions
const canvasWidth = computed(() => props.width - 16) // Leave padding
const canvasHeight = computed(() => {
  // Use the actual measured container height
  return containerHeight.value
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
  if (!viewportIndicator.value || !props.state) return {}

  const { startChunk, endChunk, chunkCount } = viewportIndicator.value

  // Get the memory map's current display range
  const chunkSize = props.state.chunkSize
  const viewportStartChunk = Math.floor(props.currentOffset / chunkSize)
  const viewportEndChunk = Math.ceil((props.currentOffset + props.viewSize) / chunkSize)
  const viewportCenterChunk = Math.floor((viewportStartChunk + viewportEndChunk) / 2)
  const maxRows = Math.floor(canvasHeight.value / pixelSize.value)
  const chunksPerRow = columnsPerRow.value
  const totalVisibleChunks = maxRows * chunksPerRow
  const mapStartChunk = Math.max(0, viewportCenterChunk - Math.floor(totalVisibleChunks / 2))

  // Check if viewport is within the visible memory map range
  if (endChunk < mapStartChunk || startChunk >= mapStartChunk + totalVisibleChunks) {
    return { display: 'none' } // Viewport is outside visible range
  }

  // Calculate relative positions within the memory map display
  const relativeStartChunk = Math.max(0, startChunk - mapStartChunk)
  const relativeEndChunk = Math.min(totalVisibleChunks, endChunk - mapStartChunk)

  const startRow = Math.floor(relativeStartChunk / columnsPerRow.value)
  const startCol = relativeStartChunk % columnsPerRow.value
  const endRow = Math.floor((relativeEndChunk - 1) / columnsPerRow.value)
  const endCol = (relativeEndChunk - 1) % columnsPerRow.value

  // Calculate the actual viewport rectangle
  const top = startRow * pixelSize.value
  const left = startCol * pixelSize.value

  // For single row
  if (startRow === endRow) {
    const width = (endCol - startCol + 1) * pixelSize.value
    return {
      top: `${top}px`,
      left: `${left}px`,
      width: `${width}px`,
      height: `${pixelSize.value}px`
    }
  }

  // For multiple rows - create a proper rectangle
  const numRows = endRow - startRow + 1
  const height = numRows * pixelSize.value

  // For now, show full width when spanning multiple rows
  // (More complex shape rendering would require multiple divs or SVG)
  return {
    top: `${top}px`,
    left: '0px',
    width: `${canvasWidth.value}px`,
    height: `${height}px`
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

  // Calculate visible window of chunks based on current offset
  const chunkSize = props.state.chunkSize

  // Calculate the center of the viewport (yellow indicator area)
  const viewportStartChunk = Math.floor(props.currentOffset / chunkSize)
  const viewportEndChunk = Math.ceil((props.currentOffset + props.viewSize) / chunkSize)
  const viewportCenterChunk = Math.floor((viewportStartChunk + viewportEndChunk) / 2)

  // Calculate how many chunks we can display in the memory map
  const maxRows = Math.floor(canvasHeight.value / pixelSize.value)
  const chunksPerRow = columnsPerRow.value
  const totalVisibleChunks = maxRows * chunksPerRow

  // Center the memory map so the viewport appears in the middle
  const startChunk = Math.max(0, viewportCenterChunk - Math.floor(totalVisibleChunks / 2))
  const totalChunks = Math.ceil(props.state.totalSize / props.state.chunkSize)
  const endChunk = Math.min(totalChunks, startChunk + totalVisibleChunks)

  // Draw chunks in the visible window
  for (let i = startChunk; i < endChunk; i++) {
    const chunk = props.state.chunks[i]
    if (!chunk) {
      continue
    }

    // Calculate position in the display grid
    const displayIndex = i - startChunk
    const row = Math.floor(displayIndex / chunksPerRow)
    const col = displayIndex % chunksPerRow
    const x = col * pixelSize.value
    const y = row * pixelSize.value

    // Choose color based on chunk state
    if (!chunk.scanned) {
      ctx.fillStyle = '#0a0a0a' // Very dark for unscanned
    } else if (chunk.isZero) {
      ctx.fillStyle = '#202020' // Dark gray for zero
    } else if (chunk.scanCount && chunk.scanCount <= 1) {
      // First scan - show as neutral blue (not a change)
      ctx.fillStyle = '#4080c0'
    } else if (chunk.lastChangeTime && chunk.lastChangeTime > 0) {
      // Has changed at some point - use logarithmic decay from hot to cool
      const timeSinceChange = Date.now() - chunk.lastChangeTime
      const secondsSinceChange = timeSinceChange / 1000

      // Logarithmic decay: starts hot, cools down but never reaches unchanged color
      // log(1) = 0 (just changed), log(10) = 1, log(100) = 2, etc.
      const decay = Math.log10(Math.max(1, secondsSinceChange))
      const maxDecay = 3 // After 1000 seconds (~17 min), reaches coolest
      const normalizedDecay = Math.min(decay / maxDecay, 1)

      // Interpolate from bright green (fresh) to yellow to orange to red (old change)
      if (normalizedDecay < 0.33) {
        // Green to yellow
        const t = normalizedDecay * 3
        const r = Math.floor(255 * t)
        ctx.fillStyle = `rgb(${r}, 255, 0)`
      } else if (normalizedDecay < 0.66) {
        // Yellow to orange
        const t = (normalizedDecay - 0.33) * 3
        const g = Math.floor(255 * (1 - t * 0.5))
        ctx.fillStyle = `rgb(255, ${g}, 0)`
      } else {
        // Orange to deep red
        const t = (normalizedDecay - 0.66) * 3
        const r = Math.floor(255 * (1 - t * 0.3))
        ctx.fillStyle = `rgb(${r}, ${Math.floor(128 * (1 - t))}, 0)`
      }
    } else {
      // Never changed (stable data)
      ctx.fillStyle = '#4080c0' // Stable blue for unchanged data
    }

    // Draw chunk pixel
    ctx.fillRect(x, y, pixelSize.value - 1, pixelSize.value - 1)

    // Highlight hovered chunk
    if (i === hoveredChunkIndex.value) {
      ctx.strokeStyle = '#ffff00'
      ctx.lineWidth = 2
      ctx.strokeRect(x, y, pixelSize.value - 1, pixelSize.value - 1)
    }
  }
}

// Handle mouse click
function handleClick(event: MouseEvent) {
  if (!canvasRef.value || !props.state) return

  const rect = canvasRef.value.getBoundingClientRect()
  const x = event.clientX - rect.left
  const y = event.clientY - rect.top

  const col = Math.floor(x / pixelSize.value)
  const row = Math.floor(y / pixelSize.value)

  // Calculate actual chunk index based on visible window
  const chunkSize = props.state.chunkSize
  const centerChunk = Math.floor(props.currentOffset / chunkSize)
  const maxRows = Math.floor(canvasHeight.value / pixelSize.value)
  const totalVisibleChunks = maxRows * columnsPerRow.value
  const startChunk = Math.max(0, centerChunk - Math.floor(totalVisibleChunks / 2))

  const displayIndex = row * columnsPerRow.value + col
  const chunkIndex = startChunk + displayIndex

  if (chunkIndex < props.state.chunks.length) {
    const chunk = props.state.chunks[chunkIndex]
    if (chunk) {
      emit('jumpToOffset', chunk.offset)
    }
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

  // Calculate actual chunk index based on visible window
  const chunkSize = props.state.chunkSize
  const centerChunk = Math.floor(props.currentOffset / chunkSize)
  const maxRows = Math.floor(canvasHeight.value / pixelSize.value)
  const totalVisibleChunks = maxRows * columnsPerRow.value
  const startChunk = Math.max(0, centerChunk - Math.floor(totalVisibleChunks / 2))

  const displayIndex = row * columnsPerRow.value + col
  const chunkIndex = startChunk + displayIndex

  if (chunkIndex < props.state.chunks.length) {
    hoveredChunkIndex.value = chunkIndex
    const chunk = props.state.chunks[chunkIndex]
    if (chunk) {
      tooltip.value = chunk

      // Position tooltip
      tooltipStyle.value = {
        left: `${event.clientX + 10}px`,
        top: `${event.clientY - 40}px`
      }

      render() // Re-render to show highlight
    }
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

// Animation frame for continuous updates
let animationFrameId: number | null = null

function startContinuousRender() {
  const renderLoop = () => {
    render()
    animationFrameId = requestAnimationFrame(renderLoop)
  }
  renderLoop()
}

function stopContinuousRender() {
  if (animationFrameId !== null) {
    cancelAnimationFrame(animationFrameId)
    animationFrameId = null
  }
}

// Watch for state changes
watch(() => props.state, (newState) => {
  if (newState) {
    startContinuousRender()
  } else {
    stopContinuousRender()
  }
})
watch(() => props.currentOffset, render)
watch(canvasHeight, render) // Re-render when height changes

let resizeObserver: ResizeObserver | null = null

onMounted(() => {
  // Set up resize observer to track container height
  if (canvasContainerRef.value) {
    resizeObserver = new ResizeObserver((entries) => {
      for (const entry of entries) {
        // Get the actual height of the container
        const height = entry.contentRect.height
        if (height > 0) {
          containerHeight.value = Math.floor(height)
        }
      }
    })
    resizeObserver.observe(canvasContainerRef.value)

    // Get initial height
    const rect = canvasContainerRef.value.getBoundingClientRect()
    if (rect.height > 0) {
      containerHeight.value = Math.floor(rect.height)
    }
  }

  // Start continuous rendering if we have state
  if (props.state) {
    startContinuousRender()
  }
})

onUnmounted(() => {
  stopContinuousRender()
  if (resizeObserver) {
    resizeObserver.disconnect()
    resizeObserver = null
  }
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