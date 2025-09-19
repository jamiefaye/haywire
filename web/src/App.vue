<template>
  <div id="app">
    <!-- Control Bar -->
    <div class="control-bar">
      <div class="control-group">
        <button @click="openFile" :disabled="isLoadingFile">
          {{ isFileOpen ? `📂 ${fileName}` : '📁 Open Memory File' }}
        </button>
        <span v-if="fileSize" class="file-size">
          {{ formatBytes(fileSize) }}
        </span>
      </div>

      <div class="control-group">
        <label>
          Format:
          <select v-model.number="selectedFormat">
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
        </label>

        <label>
          <input type="checkbox" v-model="splitComponents">
          Split
        </label>
      </div>

      <div class="control-group">
        <label>
          Width:
          <input
            type="number"
            v-model.number="displayWidth"
            min="1"
            max="4096"
            class="number-input"
          >
        </label>

        <label>
          Height:
          <input
            type="number"
            v-model.number="displayHeight"
            min="1"
            max="4096"
            class="number-input"
          >
        </label>

        <label>
          Stride:
          <input
            type="number"
            v-model.number="stride"
            min="0"
            max="8192"
            class="number-input"
          >
        </label>
      </div>

      <div class="control-group">
        <label>
          <input type="checkbox" v-model="columnMode">
          Column Mode
        </label>

        <template v-if="columnMode">
          <label>
            Col Width:
            <input
              type="number"
              v-model.number="columnWidth"
              min="1"
              max="1024"
              class="number-input"
            >
          </label>

          <label>
            Gap:
            <input
              type="number"
              v-model.number="columnGap"
              min="0"
              max="100"
              class="number-input"
            >
          </label>
        </template>
      </div>

      <div class="control-group">
        <label>
          Offset:
          <input
            type="text"
            v-model="offsetInput"
            @change="updateOffset"
            placeholder="0x0"
            class="offset-input"
          >
        </label>
        <button @click="refreshMemory" :disabled="!isFileOpen">
          🔄 Refresh
        </button>
      </div>

      <div class="control-group status">
        <span v-if="qmpConnected" class="status-indicator connected">
          ● QMP Connected
        </span>
        <span v-else class="status-indicator disconnected">
          ● QMP Disconnected
        </span>
        <button @click="toggleQmpConnection">
          {{ qmpConnected ? 'Disconnect' : 'Connect QMP' }}
        </button>
      </div>
    </div>

    <!-- Memory Canvas -->
    <div class="canvas-container" @contextmenu.prevent="showContextMenu">
      <MemoryCanvas
        v-if="memoryData"
        ref="memoryCanvasRef"
        :key="`canvas-${currentOffset}-${selectedFormat}`"
        :memory-data="memoryData"
        :width="canvasWidth"
        :height="canvasHeight"
        :format="selectedFormat"
        :source-offset="currentOffset"
        :stride="stride || displayWidth"
        :split-components="splitComponents"
        :column-mode="columnMode"
        :column-width="columnWidth"
        :column-gap="columnGap"
        @memory-click="handleMemoryClick"
        @memory-hover="handleMemoryHover"
      />
      <div v-else-if="isFileOpen" class="placeholder">
        Loading memory data...
      </div>
      <div v-else class="placeholder">
        Open a memory file to begin
      </div>
    </div>

    <!-- Context Menu -->
    <div
      v-if="contextMenuVisible"
      class="context-menu"
      :style="{
        left: `${contextMenuPosition.x}px`,
        top: `${contextMenuPosition.y}px`
      }"
      @click="hideContextMenu"
    >
      <div class="context-menu-item" @click="createMiniViewer">
        🔍 Create Mini Viewer Here
      </div>
      <div class="context-menu-separator"></div>
      <div class="context-menu-item" @click="copyAddress">
        📋 Copy Address
      </div>
    </div>

    <!-- Mini Bitmap Viewers -->
    <MiniBitmapViewer
      v-for="viewer in miniViewers"
      :key="viewer.id"
      :id="viewer.id"
      :memory-data="memoryData"
      :offset="viewer.offset"
      :initial-width="viewer.width"
      :initial-height="viewer.height"
      :initial-format="viewer.format"
      :initial-split-components="viewer.splitComponents"
      :title="viewer.title"
      :anchor-point="viewer.anchorPoint"
      :show-leader="true"
      @close="closeMiniViewer(viewer.id)"
      @update-config="(config) => updateMiniViewer(viewer.id, config)"
    />

    <!-- Status Bar -->
    <div class="status-bar">
      <span v-if="hoveredOffset !== null">
        Hover: 0x{{ hoveredOffset.toString(16).toUpperCase().padStart(8, '0') }}
      </span>
      <span v-if="clickedOffset !== null">
        Click: 0x{{ clickedOffset.toString(16).toUpperCase().padStart(8, '0') }}
      </span>
      <span v-if="error" class="error">
        {{ error }}
      </span>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted } from 'vue'
import MemoryCanvas from './components/MemoryCanvas.vue'
import MiniBitmapViewer from './components/MiniBitmapViewer.vue'
import { useFileSystemAPI } from './composables/useFileSystemAPI'
import { useQmpBridge } from './composables/useQmpBridge'
import { PixelFormat } from './composables/useWasmRenderer'

// File System API
const {
  isFileOpen,
  fileName,
  fileSize,
  error: fileError,
  openMemoryFile,
  readMemoryChunk,
  watchFile
} = useFileSystemAPI()

// QMP Bridge
const {
  isConnected: qmpConnected,
  connect: connectQmp,
  disconnect: disconnectQmp
} = useQmpBridge()

// Memory data
const memoryData = ref<Uint8Array | null>(null)
const isLoadingFile = ref(false)
const memoryCanvasRef = ref<InstanceType<typeof MemoryCanvas>>()

// Mini viewers
interface MiniViewer {
  id: number
  offset: number
  width: number
  height: number
  format: PixelFormat
  splitComponents: boolean
  title: string
  anchorPoint: { x: number, y: number } | null
}

const miniViewers = ref<MiniViewer[]>([])
let nextViewerId = 1

// Context menu
const contextMenuVisible = ref(false)
const contextMenuPosition = ref({ x: 0, y: 0 })
const contextMenuOffset = ref(0)

// Display settings
const displayWidth = ref(1024)
const displayHeight = ref(768)
const stride = ref(0)
const selectedFormat = ref(PixelFormat.BGR888) // Start with BGR888 since user said it was working
const splitComponents = ref(false)
const columnMode = ref(false)
const columnWidth = ref(256)
const columnGap = ref(8)

// Canvas dimensions (may differ from display dimensions)
const canvasWidth = computed(() => {
  if (columnMode.value) {
    const numColumns = Math.ceil(displayHeight.value / canvasHeight.value)
    return numColumns * (columnWidth.value + columnGap.value) - columnGap.value
  }
  return displayWidth.value
})

const canvasHeight = computed(() => {
  return displayHeight.value
})

// Offset handling
const currentOffset = ref(0)
const offsetInput = ref('0x0')

// Interaction state
const hoveredOffset = ref<number | null>(null)
const clickedOffset = ref<number | null>(null)

// Combined error state
const error = computed(() => fileError.value)

// Open file
async function openFile() {
  isLoadingFile.value = true
  const success = await openMemoryFile()

  if (success) {
    // Load initial chunk
    await refreshMemory()

    // Watch for file changes
    watchFile(() => {
      refreshMemory()
    }, 1000)
  }

  isLoadingFile.value = false
}

// Refresh memory display
async function refreshMemory() {
  if (!isFileOpen.value) return

  // Calculate how much memory to read based on display settings
  const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
  const effectiveStride = stride.value || displayWidth.value
  const memorySize = effectiveStride * displayHeight.value * bytesPerPixel

  const data = await readMemoryChunk(currentOffset.value, memorySize)
  if (data) {
    memoryData.value = data
  }
}

// Update offset from input
function updateOffset() {
  let value = offsetInput.value.trim()

  // Parse hex or decimal
  if (value.startsWith('0x') || value.startsWith('0X')) {
    currentOffset.value = parseInt(value.slice(2), 16) || 0
  } else {
    currentOffset.value = parseInt(value, 10) || 0
  }

  refreshMemory()
}

// Handle memory click
function handleMemoryClick(offset: number) {
  clickedOffset.value = offset
  console.log('Memory clicked at:', `0x${offset.toString(16).toUpperCase()}`)
}

// Handle memory hover
function handleMemoryHover(offset: number) {
  hoveredOffset.value = offset
}

// Toggle QMP connection
async function toggleQmpConnection() {
  if (qmpConnected.value) {
    disconnectQmp()
  } else {
    await connectQmp()
  }
}

// Context menu functions
function showContextMenu(event: MouseEvent) {
  if (!memoryData.value) return

  const rect = (event.currentTarget as HTMLElement).getBoundingClientRect()
  const x = event.clientX - rect.left
  const y = event.clientY - rect.top

  // Calculate memory offset at click position
  const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
  const row = Math.floor(y / canvasHeight.value * displayHeight.value)
  const col = Math.floor(x / canvasWidth.value * displayWidth.value)
  const clickOffset = row * (stride.value || displayWidth.value) * bytesPerPixel + col * bytesPerPixel

  contextMenuOffset.value = currentOffset.value + clickOffset
  contextMenuPosition.value = { x: event.clientX, y: event.clientY }
  contextMenuVisible.value = true
}

function hideContextMenu() {
  contextMenuVisible.value = false
}

// Mini viewer functions
function createMiniViewer() {
  const viewer: MiniViewer = {
    id: nextViewerId++,
    offset: contextMenuOffset.value,
    width: 256,
    height: 256,
    format: selectedFormat.value,
    splitComponents: splitComponents.value,
    title: `Viewer @ 0x${contextMenuOffset.value.toString(16).toUpperCase()}`,
    anchorPoint: { x: contextMenuPosition.value.x, y: contextMenuPosition.value.y }
  }
  miniViewers.value.push(viewer)
}

function closeMiniViewer(id: number) {
  miniViewers.value = miniViewers.value.filter(v => v.id !== id)
}

function updateMiniViewer(id: number, config: any) {
  const viewer = miniViewers.value.find(v => v.id === id)
  if (viewer) {
    viewer.width = config.width
    viewer.height = config.height
    viewer.format = config.format
    viewer.splitComponents = config.splitComponents
  }
}

function copyAddress() {
  const address = `0x${contextMenuOffset.value.toString(16).toUpperCase()}`
  navigator.clipboard.writeText(address)
  console.log('Copied address:', address)
}

// Helper: Get bytes per pixel for format
function getBytesPerPixel(format: PixelFormat): number {
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
      return 4
    default:
      return 1
  }
}

// Helper: Format bytes
function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`
}

// Watch for display changes
watch([displayWidth, displayHeight, stride, selectedFormat, splitComponents, columnMode, columnWidth, columnGap], () => {
  if (isFileOpen.value) {
    refreshMemory()
  }
})

// Export for template
const PixelFormatExport = PixelFormat

// Auto-connect to QMP on mount
onMounted(() => {
  connectQmp().catch(console.error)
})
</script>

<style>
* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
  background: #1e1e1e;
  color: #e0e0e0;
}

#app {
  display: flex;
  flex-direction: column;
  height: 100vh;
}

.control-bar {
  background: #2d2d30;
  border-bottom: 1px solid #3e3e42;
  padding: 10px;
  display: flex;
  flex-wrap: wrap;
  gap: 20px;
  align-items: center;
}

.control-group {
  display: flex;
  align-items: center;
  gap: 10px;
}

.control-group.status {
  margin-left: auto;
}

label {
  display: flex;
  align-items: center;
  gap: 5px;
  font-size: 14px;
}

input[type="checkbox"] {
  cursor: pointer;
}

select, input {
  background: #3c3c3c;
  color: #e0e0e0;
  border: 1px solid #555;
  padding: 4px 8px;
  border-radius: 3px;
  font-size: 14px;
}

select {
  cursor: pointer;
}

.number-input {
  width: 80px;
}

.offset-input {
  width: 120px;
  font-family: 'Courier New', monospace;
}

button {
  background: #0e639c;
  color: white;
  border: none;
  padding: 6px 12px;
  border-radius: 3px;
  cursor: pointer;
  font-size: 14px;
  transition: background 0.2s;
}

button:hover:not(:disabled) {
  background: #1177bb;
}

button:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.file-size {
  font-size: 12px;
  color: #999;
}

.status-indicator {
  font-size: 12px;
  display: flex;
  align-items: center;
  gap: 4px;
}

.status-indicator.connected {
  color: #4ec9b0;
}

.status-indicator.disconnected {
  color: #f48771;
}

.canvas-container {
  flex: 1;
  overflow: auto;
  background: #1e1e1e;
  display: flex;
  justify-content: center;
  align-items: center;
  padding: 20px;
}

.placeholder {
  color: #666;
  font-size: 18px;
  text-align: center;
}

.status-bar {
  background: #007acc;
  color: white;
  padding: 6px 12px;
  font-size: 13px;
  font-family: 'Courier New', monospace;
  display: flex;
  gap: 20px;
  min-height: 28px;
}

.status-bar .error {
  color: #ff6b6b;
  margin-left: auto;
}

/* Context Menu */
.context-menu {
  position: fixed;
  background: #2d2d30;
  border: 1px solid #555;
  border-radius: 4px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.5);
  z-index: 10000;
  padding: 4px 0;
  min-width: 180px;
}

.context-menu-item {
  padding: 8px 16px;
  color: #e0e0e0;
  cursor: pointer;
  font-size: 13px;
  display: flex;
  align-items: center;
  gap: 8px;
}

.context-menu-item:hover {
  background: #0e639c;
}

.context-menu-separator {
  height: 1px;
  background: #555;
  margin: 4px 0;
}
</style>