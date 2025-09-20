<template>
  <div class="autocorrelator" :style="containerStyle">
    <canvas
      ref="canvas"
      :width="width"
      :height="height"
      @mousemove="onMouseMove"
      @mouseleave="onMouseLeave"
    />
    <div v-if="hoveredValue !== null" class="tooltip" :style="tooltipStyle">
      Offset: {{ hoveredOffset }}, Value: {{ hoveredValue.toFixed(3) }}
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'
import { wasmModule } from '@/composables/wasmSingleton'

const props = defineProps<{
  memoryData: Uint8Array
  width: number
  height?: number
  enabled?: boolean
}>()

const emit = defineEmits<{
  peakDetected: [offset: number]
}>()

const canvas = ref<HTMLCanvasElement>()
const correlationData = ref<Float32Array>(new Float32Array(2048))
const peaks = ref<number[]>([])
const hoveredOffset = ref<number | null>(null)
const hoveredValue = ref<number | null>(null)
const tooltipX = ref(0)
const tooltipY = ref(0)

const height = computed(() => props.height || 100)

const containerStyle = computed(() => ({
  width: `${props.width}px`,
  height: `${height.value}px`,
  position: 'relative',
  backgroundColor: '#141414',
  borderTop: '1px solid #333'
}))

const tooltipStyle = computed(() => ({
  left: `${tooltipX.value}px`,
  top: `${tooltipY.value}px`
}))

// Compute autocorrelation using WASM
function computeCorrelation() {
  if (!props.enabled || !wasmModule.value || !props.memoryData || props.memoryData.length === 0) {
    return
  }

  try {
    // Allocate memory for input and output
    const dataSize = Math.min(props.memoryData.length, 16384) // Limit to 16K samples
    const dataPtr = wasmModule.value._allocateMemory(dataSize)
    const outputPtr = wasmModule.value._allocateFloatBuffer(2048)

    // Copy data to WASM heap
    wasmModule.value.HEAPU8.set(props.memoryData.subarray(0, dataSize), dataPtr)

    // Compute autocorrelation
    wasmModule.value._autoCorrelate(dataPtr, dataSize, outputPtr, 2048)

    // Read results
    const outputArray = new Float32Array(wasmModule.value.HEAPF32.buffer, outputPtr, 2048)
    correlationData.value = new Float32Array(outputArray)

    // Find peaks
    const threshold = 0.3
    const peaksPtr = wasmModule.value._allocateMemory(40) // 10 peaks * 4 bytes
    wasmModule.value._getCorrelationPeaks(dataPtr, dataSize, peaksPtr, 10, threshold)

    const peaksArray = new Int32Array(wasmModule.value.HEAP32.buffer, peaksPtr, 10)
    peaks.value = []
    for (let i = 0; i < 10; i++) {
      if (peaksArray[i] >= 0) {
        peaks.value.push(peaksArray[i])
      }
    }

    // Free memory
    wasmModule.value._freeMemory(dataPtr)
    wasmModule.value._freeFloatBuffer(outputPtr)
    wasmModule.value._freeMemory(peaksPtr)

    drawCorrelation()
  } catch (error) {
    console.error('Error computing autocorrelation:', error)
  }
}

// Draw the correlation graph
function drawCorrelation() {
  const ctx = canvas.value?.getContext('2d')
  if (!ctx) return

  // Clear canvas
  ctx.fillStyle = '#141414'
  ctx.fillRect(0, 0, props.width, height.value)

  const displaySamples = Math.min(2048, correlationData.value.length)
  const xScale = props.width / displaySamples
  const yScale = height.value * 0.8
  const baseline = height.value - 10

  // Draw grid lines at 64-pixel intervals
  ctx.strokeStyle = '#282828'
  ctx.lineWidth = 1
  for (let x = 64; x < displaySamples; x += 64) {
    const xPos = x * xScale
    ctx.beginPath()
    ctx.moveTo(xPos, 0)
    ctx.lineTo(xPos, height.value)
    ctx.stroke()

    // Label major widths (every 256 pixels)
    if (x % 256 === 0) {
      ctx.fillStyle = '#808080'
      ctx.font = '10px monospace'
      ctx.fillText(x.toString(), xPos - 10, height.value - 2)
    }
  }

  // Draw correlation curve
  ctx.strokeStyle = '#00ff80'
  ctx.lineWidth = 1.5
  ctx.beginPath()
  ctx.moveTo(0, baseline)

  for (let i = 0; i < displaySamples; i++) {
    const x = i * xScale
    const y = baseline - correlationData.value[i] * yScale
    ctx.lineTo(x, y)
  }
  ctx.stroke()

  // Draw peaks
  ctx.strokeStyle = '#ffff00'
  ctx.lineWidth = 2
  peaks.value.forEach(peak => {
    const x = peak * xScale
    ctx.globalAlpha = 0.5
    ctx.beginPath()
    ctx.moveTo(x, 0)
    ctx.lineTo(x, height.value)
    ctx.stroke()

    // Label the peak
    ctx.globalAlpha = 1
    ctx.fillStyle = '#ffff00'
    ctx.font = '10px monospace'
    ctx.fillText(peak.toString(), x + 2, 12)
  })

  // Draw label
  ctx.fillStyle = '#c8c8c8'
  ctx.font = '11px sans-serif'
  ctx.fillText('Autocorrelation (Width Detection)', 5, 15)
}

// Mouse interaction
function onMouseMove(event: MouseEvent) {
  if (!canvas.value) return

  const rect = canvas.value.getBoundingClientRect()
  const x = event.clientX - rect.left
  const y = event.clientY - rect.top

  // Calculate which offset this corresponds to
  const offset = Math.floor(x / props.width * 2048)
  if (offset >= 0 && offset < correlationData.value.length) {
    hoveredOffset.value = offset
    hoveredValue.value = correlationData.value[offset]
    tooltipX.value = x + 10
    tooltipY.value = y - 30
  }
}

function onMouseLeave() {
  hoveredOffset.value = null
  hoveredValue.value = null
}

// Watch for changes
watch(() => props.memoryData, computeCorrelation)
watch(() => props.enabled, computeCorrelation)
watch(() => props.width, () => {
  if (props.enabled) {
    drawCorrelation()
  }
})

onMounted(() => {
  if (props.enabled) {
    computeCorrelation()
  }
})
</script>

<style scoped>
.autocorrelator {
  user-select: none;
  cursor: crosshair;
}

canvas {
  display: block;
  image-rendering: pixelated;
}

.tooltip {
  position: absolute;
  background: rgba(0, 0, 0, 0.9);
  color: #fff;
  padding: 4px 8px;
  border-radius: 3px;
  font-size: 11px;
  font-family: monospace;
  pointer-events: none;
  white-space: nowrap;
  z-index: 1000;
}
</style>