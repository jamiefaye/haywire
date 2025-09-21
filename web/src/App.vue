<template>
  <div id="app">
    <!-- Memory File Manager (Electron) -->
    <MemoryFileManager
      v-if="isElectron"
      @file-opened="handleElectronFileOpened"
      @file-refreshed="handleElectronFileRefreshed"
      @file-closed="handleElectronFileClosed"
    />

    <!-- Control Bar -->
    <div class="control-bar">
      <!-- Legacy file open button for non-Electron mode -->
      <div v-if="!isElectron" class="control-group">
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

      </div>

      <div class="control-group">
        <label>
          <input type="checkbox" v-model="columnMode">
          Column Mode
        </label>
        <label>
          <input type="checkbox" v-model="changeDetectionEnabled">
          Change Detection
        </label>
        <label>
          <input type="checkbox" v-model="showCorrelation">
          Auto-Correlation
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
      <div v-if="qmpAvailable" class="control-group status">
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

    <!-- Main Content Area -->
    <div class="main-content">
      <!-- Overview Pane -->
      <div v-if="isFileOpen && changeDetectionEnabled" class="overview-container">
        <!-- Show progress bar during scan -->
        <div v-if="scanProgress.scanning" class="scan-progress">
          <div class="progress-bar">
            <div class="progress-fill" :style="{ width: scanProgressPercent + '%' }"></div>
          </div>
          <div class="progress-text">Scanning... {{ scanProgressPercent }}%</div>
        </div>
        <MemoryOverviewPane
          v-if="changeDetectionState"
          :width="128"
          :state="changeDetectionState"
          :current-offset="currentOffset"
          :view-size="canvasWidth * canvasHeight * bytesPerPixel"
          @jump-to-offset="jumpToOffset"
        />
      </div>

      <!-- Left Sidebar with Address Slider -->
      <div v-if="isFileOpen" class="address-slider-sidebar">
        <button
          @mousedown="startAutoRepeat(pageUp)"
          @mouseup="stopAutoRepeat"
          @mouseleave="stopAutoRepeat"
          @touchstart="startAutoRepeat(pageUp)"
          @touchend="stopAutoRepeat"
          class="nav-button"
          title="Page up (↑) - Hold to repeat"
        >
          ⬆
        </button>

        <div class="slider-container">
          <input
            type="range"
            :value="invertedSliderPosition"
            @input="onInvertedSliderChange"
            :min="0"
            :max="maxSliderPosition"
            :step="1"
            class="address-slider-vertical"
            orient="vertical"
          >
          <div class="slider-track"></div>
        </div>

        <button
          @mousedown="startAutoRepeat(pageDown)"
          @mouseup="stopAutoRepeat"
          @mouseleave="stopAutoRepeat"
          @touchstart="startAutoRepeat(pageDown)"
          @touchend="stopAutoRepeat"
          class="nav-button"
          title="Page down (↓) - Hold to repeat"
        >
          ⬇
        </button>

        <div class="slider-address">
          <div>{{ formatHex(currentOffset) }}</div>
          <div class="separator">━</div>
          <div>{{ formatHex(fileSize) }}</div>
        </div>
      </div>

      <!-- Memory Canvas and Correlator Container -->
    <div class="canvas-and-correlator">
      <div
        class="canvas-container"
        @contextmenu.prevent="showContextMenu"
        @dragover.prevent="onDragOver"
        @drop.prevent="onDrop"
        @dragleave="onDragLeave"
        @mousedown="startCanvasDrag"
        :class="{ 'drag-over': isDraggingFile, 'dragging': isCanvasDragging }"
      >
        <!-- FFT Sample Point Indicator -->
        <svg
          v-if="showCorrelation && fftSampleOffset > 0 && fftIndicatorPosition"
          class="fft-indicator-overlay"
          :style="{
            position: 'absolute',
            top: 0,
            left: 0,
            width: '100%',
            height: '100%',
            pointerEvents: 'none',
            zIndex: 10
          }"
        >
          <circle
            :cx="fftIndicatorPosition.x"
            :cy="fftIndicatorPosition.y"
            r="8"
            fill="none"
            stroke="#ffaa00"
            stroke-width="2"
            opacity="0.8"
          />
          <circle
            :cx="fftIndicatorPosition.x"
            :cy="fftIndicatorPosition.y"
            r="3"
            fill="#ffaa00"
            opacity="0.8"
          />
        </svg>

        <MemoryCanvas
          v-if="memoryData"
          ref="memoryCanvasRef"
          :key="`canvas-${selectedFormat}`"
          :memory-data="memoryData"
          :width="canvasWidth"
          :height="canvasHeight"
          :format="selectedFormat"
          :source-offset="0"
          :stride="displayWidth"
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

      <!-- Auto-Correlator Pane -->
      <div v-if="memoryData && showCorrelation" class="correlator-wrapper">
        <AutoCorrelator
          :memory-data="memoryData"
          :width="canvasWidth"
          :height="100"
          :enabled="showCorrelation"
          :sample-offset="fftSampleOffset"
          @peak-detected="handleCorrelationPeak"
        />
      </div>
    </div>
    </div> <!-- End of main-content -->

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
      <div class="context-menu-item" @click="openMagnifyingGlass">
        🔎 Open Magnifying Glass
      </div>
      <div class="context-menu-separator"></div>
      <div class="context-menu-item" @click="setFFTSamplePoint">
        📊 Set FFT Sample Point Here
      </div>
      <div class="context-menu-item" @click="copyAddress">
        📋 Copy Address
      </div>
    </div>

    <!-- Mini Bitmap Viewers -->
    <MiniBitmapViewer
      v-for="viewer in miniViewers"
      :key="viewer.id"
      :id="viewer.id"
      :memory-data="viewer.memoryData || memoryData"
      :offset="0"
      :initial-width="viewer.width"
      :initial-height="viewer.height"
      :initial-format="viewer.format"
      :initial-split-components="viewer.splitComponents"
      :initial-column-mode="viewer.columnMode"
      :initial-column-width="viewer.columnWidth"
      :initial-column-gap="viewer.columnGap"
      :title="viewer.title"
      :anchor-point="viewer.anchorPoint"
      :show-leader="true"
      @close="closeMiniViewer(viewer.id)"
      @update-config="(config) => updateMiniViewer(viewer.id, config)"
      @drag-state-changed="(dragging) => viewer.isDragging = dragging"
      @anchor-drag="(pos) => handleAnchorDrag(viewer.id, pos)"
      @anchor-mode-changed="(mode, relPos) => handleAnchorModeChange(viewer.id, mode, relPos)"
      :initial-anchor-mode="viewer.anchorMode || 'address'"
      :relative-anchor-pos="viewer.relativeAnchorPos"
    />

    <!-- Magnifying Glass -->
    <MagnifyingGlass
      v-if="magnifyingGlassVisible"
      :source-canvas="mainCanvas"
      :center-x="magnifyingGlassCenterX"
      :center-y="magnifyingGlassCenterY"
      :visible="magnifyingGlassVisible"
      :initial-locked="magnifyingGlassLocked"
      @close="closeMagnifyingGlass"
      @update="updateMagnifyingGlassPosition"
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
import { ref, computed, watch, onMounted, onUnmounted, nextTick } from 'vue'
import MemoryCanvas from './components/MemoryCanvas.vue'
import MiniBitmapViewer from './components/MiniBitmapViewer.vue'
import MagnifyingGlass from './components/MagnifyingGlass.vue'
import MemoryOverviewPane from './components/MemoryOverviewPane.vue'
import AutoCorrelator from './components/AutoCorrelator.vue'
import MemoryFileManager from './components/MemoryFileManager.vue'
import { useFileSystemAPI } from './composables/useFileSystemAPI'
import { useQmpBridge } from './composables/useQmpBridge'
import { PixelFormat } from './composables/useWasmRenderer'
import { useChangeDetection } from './composables/useChangeDetection'
import { qmpAvailable } from './utils/electronDetect'

// File System API
const {
  fileHandle,
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

// Change detection
const {
  isLoading: isLoadingChangeDetection,
  error: changeDetectionError,
  state: changeDetectionState,
  loadModule: loadChangeDetectionModule,
  scanMemory,
  getChunkAtOffset
} = useChangeDetection()

// Memory data
const memoryData = ref<Uint8Array | null>(null)
const isLoadingFile = ref(false)
const memoryCanvasRef = ref<InstanceType<typeof MemoryCanvas>>()
const lastScanTime = ref<number>(0)

// Check if running in Electron
const isElectron = ref(window.electronAPI && window.electronAPI.isElectron)
// Incremental scanning state
const scanProgress = ref({
  currentChunk: 0,
  totalChunks: 0,
  scanning: false,
  pendingData: null as Uint8Array | null,
  chunkSize: 65536, // 64KB per chunk
  chunksPerFrame: 100, // Process 100 chunks (6.4MB) per frame
  autoRefresh: false, // Auto-repeat scans (off by default due to permission issues)
  refreshDelay: 2000 // Wait 2 seconds between scans
})

// Mini viewers
interface MiniViewer {
  id: number
  offset: number
  width: number
  height: number
  format: PixelFormat
  splitComponents: boolean
  columnMode: boolean
  columnWidth: number
  columnGap: number
  title: string
  anchorPoint: { x: number, y: number } | null
  isDragging?: boolean
  memoryData?: Uint8Array
  anchorMode?: 'address' | 'position'
  relativeAnchorPos?: { x: number, y: number }
}

const miniViewers = ref<MiniViewer[]>([])
let nextViewerId = 1

// Magnifying glass
const magnifyingGlassVisible = ref(false)
const magnifyingGlassCenterX = ref(0)
const magnifyingGlassCenterY = ref(0)
const magnifyingGlassLocked = ref(false)
const mainCanvas = ref<HTMLCanvasElement | null>(null)

// Context menu
const contextMenuVisible = ref(false)
const contextMenuPosition = ref({ x: 0, y: 0 })
const contextMenuOffset = ref(0)

// Drag and drop
const isDraggingFile = ref(false)

// Canvas dragging for scrolling
const isCanvasDragging = ref(false)
const canvasDragStart = ref({ x: 0, y: 0, offset: 0 })

// Auto-repeat for navigation buttons
let autoRepeatInterval: number | null = null
let autoRepeatTimeout: number | null = null

// Display settings
const displayWidth = ref(1024)
// Height will be computed based on canvas container
const canvasContainerHeight = ref(768)
const selectedFormat = ref(PixelFormat.BGR888) // Start with BGR888 since user said it was working
const splitComponents = ref(false)
const columnMode = ref(false)
const changeDetectionEnabled = ref(false)  // Disabled by default - opt-in feature
const showCorrelation = ref(false)  // Autocorrelation display
const fftSampleOffset = ref<number>(0)  // Offset for FFT sampling (relative to current view)
const fftRelativePosition = ref<{ x: number, y: number } | null>(null)  // Relative position (0-1)
const fftIndicatorPosition = ref<{ x: number, y: number } | null>(null)  // Absolute pixel position

// Watch for change detection toggle
watch(changeDetectionEnabled, (enabled) => {
  if (!enabled) {
    // Stop any ongoing scan
    scanProgress.value.scanning = false
    scanProgress.value.pendingData = null
    // Note: We keep the file handle open - it stays valid
    console.log('Change detection disabled, file remains open')
  } else if (enabled && memoryData.value) {
    // Re-enable scanning with current data
    console.log('Change detection enabled, starting scan...')
    performChangeDetectionScan(memoryData.value)
  }
})
const columnWidth = ref(256)
const columnGap = ref(8)

// Display height is based on actual canvas container size
const displayHeight = computed(() => {
  // Use the container height, but reasonable default
  return canvasContainerHeight.value
})

// Canvas dimensions (may differ from display dimensions)
const canvasWidth = computed(() => {
  // In column mode, canvas width should still be the full display width
  // The renderer will handle column layout internally
  return displayWidth.value
})

const canvasHeight = computed(() => {
  return displayHeight.value
})

const bytesPerPixel = computed(() => {
  return getBytesPerPixel(selectedFormat.value)
})

const formatName = computed(() => {
  const formats = {
    [PixelFormat.GRAYSCALE]: 'GRAY8',
    [PixelFormat.RGB565]: 'RGB565',
    [PixelFormat.RGB888]: 'RGB888',
    [PixelFormat.RGBA8888]: 'RGBA8888',
    [PixelFormat.BGR888]: 'BGR888',
    [PixelFormat.BGRA8888]: 'BGRA8888',
    [PixelFormat.ARGB8888]: 'ARGB8888',
    [PixelFormat.ABGR8888]: 'ABGR8888',
    [PixelFormat.BINARY]: 'BINARY',
    [PixelFormat.HEX_PIXEL]: 'HEX_PIXEL',
    [PixelFormat.CHAR_8BIT]: 'CHAR_8BIT'
  }
  return formats[selectedFormat.value] || 'GRAY8'
})

// Offset handling
const currentOffset = ref(0)
const offsetInput = ref('0x0')
const sliderPosition = ref(0)

// Slider computed values
const pageSize = computed(() => {
  const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
  return displayWidth.value * displayHeight.value * bytesPerPixel
})

// Computed scan progress percentage
const scanProgressPercent = computed(() => {
  if (!scanProgress.value.scanning || scanProgress.value.totalChunks === 0) return 0
  return Math.round((scanProgress.value.currentChunk / scanProgress.value.totalChunks) * 100)
})

const maxSliderPosition = computed(() => {
  // Simply use the full file size
  return fileSize.value || 0
})

// Inverted slider position (top = 0, bottom = max)
const invertedSliderPosition = computed(() => {
  return maxSliderPosition.value - sliderPosition.value
})

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
  const memorySize = displayWidth.value * displayHeight.value * bytesPerPixel

  // Ensure offset is within bounds (0 to file size)
  if (currentOffset.value < 0) {
    currentOffset.value = 0
  }
  if (currentOffset.value >= fileSize.value) {
    // Allow viewing at the end of file, but clamp to last valid position
    currentOffset.value = Math.max(0, fileSize.value - 1)
  }

  // Check if we have a dropped file
  if ((window as any).__droppedFileData) {
    const fullData = (window as any).__droppedFileData as Uint8Array
    const endOffset = Math.min(currentOffset.value + memorySize, fullData.length)

    // If we're at the end and don't have enough data, pad with zeros
    let data = fullData.slice(currentOffset.value, endOffset)
    if (data.length < memorySize) {
      const padded = new Uint8Array(memorySize)
      padded.set(data)
      data = padded
    }
    memoryData.value = data
    performChangeDetectionScan(data)
  } else {
    const data = await readMemoryChunk(currentOffset.value, memorySize)
    if (data) {
      // Ensure we have the full size requested, pad if necessary
      if (data.length < memorySize) {
        const padded = new Uint8Array(memorySize)
        padded.set(data)
        memoryData.value = padded
        performChangeDetectionScan(padded)
      } else {
        memoryData.value = data
        performChangeDetectionScan(data)
      }
    }
  }
}

// Perform change detection scan incrementally
function performChangeDetectionScan(data: Uint8Array) {
  // Skip if disabled
  if (!changeDetectionEnabled.value) return

  // If already scanning this data, skip
  if (scanProgress.value.scanning && scanProgress.value.pendingData === data) return

  // Store the data and start incremental scan
  scanProgress.value.pendingData = data
  scanProgress.value.totalChunks = Math.ceil(data.length / scanProgress.value.chunkSize)
  scanProgress.value.currentChunk = 0
  scanProgress.value.scanning = true

  console.log(`Starting incremental scan of ${data.length} bytes (${scanProgress.value.totalChunks} chunks)`)

  // Start the incremental scan process
  processNextScanBatch()
}

// Process a batch of chunks incrementally
function processNextScanBatch() {
  if (!scanProgress.value.scanning || !scanProgress.value.pendingData) return
  if (!changeDetectionEnabled.value) {
    scanProgress.value.scanning = false
    return
  }

  const data = scanProgress.value.pendingData
  const startChunk = scanProgress.value.currentChunk
  const endChunk = Math.min(
    startChunk + scanProgress.value.chunksPerFrame,
    scanProgress.value.totalChunks
  )

  // Get or create the state
  if (!changeDetectionState.value) {
    changeDetectionState.value = {
      chunks: [],
      pageSize: 4096,
      chunkSize: scanProgress.value.chunkSize,
      totalSize: data.length,
      lastCheckTime: Date.now()
    }
  }

  // Process this batch of chunks
  const state = changeDetectionState.value
  const chunkSize = scanProgress.value.chunkSize

  for (let i = startChunk; i < endChunk; i++) {
    const offset = i * chunkSize
    const size = Math.min(chunkSize, data.length - offset)

    // Get the data slice for this chunk
    const chunkData = data.subarray(offset, offset + size)

    // For now, use simple JS implementation
    // TODO: Hook up to WASM functions through the composable
    const isZero = chunkData.every(b => b === 0)
    const checksum = isZero ? 0 :
      chunkData.reduce((sum, byte) => ((sum << 5) | (sum >>> 27)) ^ byte, 0)

    // Check if changed from previous scan
    const prevChunk = state.chunks[i]
    const hasChanged = prevChunk ?
      (prevChunk.checksum !== checksum || prevChunk.isZero !== isZero) : false

    // Update or add chunk info
    state.chunks[i] = {
      offset,
      size,
      checksum,
      isZero,
      hasChanged
    }
  }

  // Update progress
  scanProgress.value.currentChunk = endChunk
  const percentComplete = Math.round((endChunk / scanProgress.value.totalChunks) * 100)

  if (endChunk < scanProgress.value.totalChunks) {
    // More chunks to process - schedule next batch
    console.log(`Scan progress: ${percentComplete}% (${endChunk}/${scanProgress.value.totalChunks} chunks)`)
    requestAnimationFrame(processNextScanBatch)
  } else {
    // Scan complete
    scanProgress.value.scanning = false
    state.lastCheckTime = Date.now()
    console.log('Change detection scan complete:', state)

    // Schedule next scan if auto-refresh is enabled
    if (scanProgress.value.autoRefresh && changeDetectionEnabled.value && scanProgress.value.pendingData) {
      setTimeout(async () => {
        if (changeDetectionEnabled.value && fileHandle.value) {
          console.log('Auto-refresh: re-reading file data...')
          // Re-read the file data (file stays open)
          try {
            // Just get the file data again - the handle remains valid
            const file = await fileHandle.value.getFile()
            const arrayBuffer = await file.arrayBuffer()
            const freshData = new Uint8Array(arrayBuffer)

            // Update the main memory data too so display refreshes
            memoryData.value = freshData

            // Start a new scan with fresh data from disk
            scanProgress.value.pendingData = freshData
            scanProgress.value.currentChunk = 0
            scanProgress.value.scanning = true
            console.log('Starting auto-refresh scan with fresh data...')
            processNextScanBatch()
          } catch (error) {
            console.warn('Failed to re-read file:', error)
            // If we can't read, just continue with existing data
            // Don't stop auto-refresh, just skip this cycle
            scanProgress.value.currentChunk = 0
            scanProgress.value.scanning = true
            console.log('Re-read failed, scanning existing data...')
            processNextScanBatch()
          }
        }
      }, scanProgress.value.refreshDelay)
    }
  }
}

// Jump to a specific offset from overview pane
function jumpToOffset(offset: number) {
  currentOffset.value = offset
  sliderPosition.value = offset
  offsetInput.value = `0x${offset.toString(16)}`
  refreshMemory()
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

  // Update slider position
  sliderPosition.value = currentOffset.value

  refreshMemory()
}

// Slider control functions
function onSliderChange() {
  const newOffset = Math.floor(sliderPosition.value)
  if (newOffset !== currentOffset.value) {
    currentOffset.value = newOffset
    offsetInput.value = `0x${newOffset.toString(16).toUpperCase()}`
    refreshMemory()
  }
}

function onInvertedSliderChange(event: Event) {
  // Get the inverted value from the slider
  const invertedValue = parseInt((event.target as HTMLInputElement).value)
  // Convert inverted position back to normal position
  const newOffset = Math.floor(maxSliderPosition.value - invertedValue)
  if (newOffset !== currentOffset.value) {
    currentOffset.value = newOffset
    sliderPosition.value = newOffset
    offsetInput.value = `0x${newOffset.toString(16).toUpperCase()}`
    refreshMemory()
  }
}

function jumpToStart() {
  currentOffset.value = 0
  sliderPosition.value = 0
  offsetInput.value = '0x0'
  refreshMemory()
}

function jumpToEnd() {
  // Jump to the last page that shows data
  const lastPageOffset = Math.max(0, fileSize.value - pageSize.value)
  currentOffset.value = lastPageOffset
  sliderPosition.value = lastPageOffset
  offsetInput.value = `0x${lastPageOffset.toString(16).toUpperCase()}`
  refreshMemory()
}

function pageUp() {
  const newOffset = Math.max(0, currentOffset.value - pageSize.value)
  currentOffset.value = newOffset
  sliderPosition.value = newOffset
  offsetInput.value = `0x${newOffset.toString(16).toUpperCase()}`
  refreshMemory()
}

function pageDown() {
  const newOffset = Math.min(fileSize.value - 1, currentOffset.value + pageSize.value)
  currentOffset.value = newOffset
  sliderPosition.value = newOffset
  offsetInput.value = `0x${newOffset.toString(16).toUpperCase()}`
  refreshMemory()
}

// Auto-repeat navigation functions
function startAutoRepeat(action: () => void) {
  // Clear any existing intervals
  stopAutoRepeat()

  // Execute the action immediately
  action()

  // Start auto-repeat after initial delay
  autoRepeatTimeout = window.setTimeout(() => {
    autoRepeatInterval = window.setInterval(() => {
      action()
    }, 50) // Repeat every 50ms (20Hz)
  }, 300) // Initial delay of 300ms
}

function stopAutoRepeat() {
  if (autoRepeatTimeout) {
    window.clearTimeout(autoRepeatTimeout)
    autoRepeatTimeout = null
  }
  if (autoRepeatInterval) {
    window.clearInterval(autoRepeatInterval)
    autoRepeatInterval = null
  }
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

// Handle correlation peak detection
function handleCorrelationPeak(offset: number) {
  console.log('Correlation peak detected at offset:', offset)
  // Could automatically set width based on detected peak
  // displayWidth.value = offset
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

  // Get the actual canvas element, not the container
  const canvas = document.querySelector('.memory-canvas') as HTMLCanvasElement
  if (!canvas) return

  const canvasRect = canvas.getBoundingClientRect()
  const x = event.clientX - canvasRect.left
  const y = event.clientY - canvasRect.top

  // Check if click is actually on the canvas
  if (x < 0 || y < 0 || x > canvasRect.width || y > canvasRect.height) {
    return // Click was outside canvas
  }

  // Calculate memory offset at click position, accounting for column mode
  const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
  let clickOffset = 0

  if (columnMode.value) {
    // Column mode calculation
    const pixelX = Math.floor((x / canvasRect.width) * canvasWidth.value)
    const pixelY = Math.floor((y / canvasRect.height) * canvasHeight.value)

    const totalColumnWidth = columnWidth.value + columnGap.value
    const columnIndex = Math.floor(pixelX / totalColumnWidth)
    const xInColumn = pixelX % totalColumnWidth

    if (xInColumn >= columnWidth.value) {
      // Click in gap - ignore
      return
    }

    const memoryX = xInColumn
    const memoryY = columnIndex * canvasHeight.value + pixelY
    clickOffset = memoryY * displayWidth.value * bytesPerPixel + memoryX * bytesPerPixel
  } else {
    // Simple linear mode
    const row = Math.floor((y / canvasRect.height) * displayHeight.value)
    const col = Math.floor((x / canvasRect.width) * displayWidth.value)
    clickOffset = row * displayWidth.value * bytesPerPixel + col * bytesPerPixel
  }

  contextMenuOffset.value = currentOffset.value + clickOffset
  contextMenuPosition.value = { x: event.clientX, y: event.clientY }
  contextMenuVisible.value = true
}

function hideContextMenu() {
  contextMenuVisible.value = false
}

// Calculate canvas position from memory offset
function getCanvasPositionFromOffset(offset: number): { x: number, y: number } | null {
  // Check if offset is visible in current view
  const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
  const bytesPerRow = displayWidth.value * bytesPerPixel
  const viewSize = displayWidth.value * displayHeight.value * bytesPerPixel

  if (offset < currentOffset.value || offset >= currentOffset.value + viewSize) {
    return null // Offset not visible in current view
  }

  // Calculate position relative to current view
  const relativeOffset = offset - currentOffset.value
  const row = Math.floor(relativeOffset / bytesPerRow)
  const col = Math.floor((relativeOffset % bytesPerRow) / bytesPerPixel)

  // Get canvas element position
  const canvas = document.querySelector('.memory-canvas') as HTMLCanvasElement
  if (!canvas) return null

  const rect = canvas.getBoundingClientRect()
  const x = rect.left + (col / displayWidth.value) * rect.width
  const y = rect.top + (row / displayHeight.value) * rect.height

  return { x, y }
}

// Mini viewer functions
async function createMiniViewer() {
  const initialAnchor = getCanvasPositionFromOffset(contextMenuOffset.value) || contextMenuPosition.value

  const viewer: MiniViewer = {
    id: nextViewerId++,
    offset: contextMenuOffset.value,
    width: 256,
    height: 256,
    format: selectedFormat.value,
    splitComponents: splitComponents.value,
    columnMode: false,
    columnWidth: 64,
    columnGap: 4,
    title: `Viewer @ 0x${contextMenuOffset.value.toString(16).toUpperCase()}`,
    anchorPoint: initialAnchor,
    anchorMode: 'address',
    relativeAnchorPos: { x: 0.5, y: 0.5 }
  }

  // Load initial data for the viewer
  await loadMiniViewerData(viewer)

  miniViewers.value.push(viewer)
}

function closeMiniViewer(id: number) {
  miniViewers.value = miniViewers.value.filter(v => v.id !== id)
}

// Magnifying glass functions
function openMagnifyingGlass() {
  if (!memoryData.value) return

  // Get the main canvas from the MemoryCanvas component
  const canvas = memoryCanvasRef.value?.canvas
  if (!canvas) return
  mainCanvas.value = canvas

  // Calculate pixel position from context menu offset relative to current view
  const relativeOffset = contextMenuOffset.value - currentOffset.value
  const bytesPerRow = displayWidth.value * bytesPerPixel.value

  const pixelY = Math.floor(relativeOffset / bytesPerRow)
  const pixelX = Math.floor((relativeOffset % bytesPerRow) / bytesPerPixel.value)

  magnifyingGlassCenterX.value = pixelX
  magnifyingGlassCenterY.value = pixelY
  magnifyingGlassLocked.value = false // Context menu opens without lock
  magnifyingGlassVisible.value = true
}

function closeMagnifyingGlass() {
  magnifyingGlassVisible.value = false
}

function updateMagnifyingGlassPosition(x: number, y: number) {
  // Convert screen coordinates to canvas coordinates
  const canvas = memoryCanvasRef.value?.canvas
  if (!canvas) return

  const rect = canvas.getBoundingClientRect()
  const canvasX = x - rect.left
  const canvasY = y - rect.top

  // Check bounds
  if (canvasX < 0 || canvasY < 0 || canvasX > rect.width || canvasY > rect.height) {
    return
  }

  // Convert canvas position to pixel coordinates
  // Note: Since we're rendering pixels, canvas coordinates are already pixel coordinates
  magnifyingGlassCenterX.value = Math.floor(canvasX)
  magnifyingGlassCenterY.value = Math.floor(canvasY)
}

function updateMiniViewer(id: number, config: any) {
  const viewer = miniViewers.value.find(v => v.id === id)
  if (viewer) {
    viewer.width = config.width
    viewer.height = config.height
    viewer.format = config.format
    viewer.splitComponents = config.splitComponents
    viewer.columnMode = config.columnMode
    viewer.columnWidth = config.columnWidth
    viewer.columnGap = config.columnGap
  }
}

async function handleAnchorDrag(id: number, mousePos: { x: number, y: number }) {
  const viewer = miniViewers.value.find(v => v.id === id)
  if (!viewer || !memoryData.value) return

  // Convert mouse position to memory offset
  const canvas = document.querySelector('.memory-canvas') as HTMLCanvasElement
  if (!canvas) return

  const rect = canvas.getBoundingClientRect()
  const relX = mousePos.x - rect.left
  const relY = mousePos.y - rect.top

  // Clamp to canvas bounds
  const clampedX = Math.max(0, Math.min(rect.width - 1, relX))
  const clampedY = Math.max(0, Math.min(rect.height - 1, relY))

  // Calculate which memory offset this corresponds to
  const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
  const row = Math.floor((clampedY / rect.height) * displayHeight.value)
  const col = Math.floor((clampedX / rect.width) * displayWidth.value)
  const newOffset = currentOffset.value + row * displayWidth.value * bytesPerPixel + col * bytesPerPixel

  // Update the viewer's offset
  viewer.offset = newOffset
  viewer.title = `Viewer @ 0x${newOffset.toString(16).toUpperCase()}`

  // Load new data for this offset
  await loadMiniViewerData(viewer)

  // Update anchor position
  viewer.anchorPoint = { x: mousePos.x, y: mousePos.y }
}

// Load data for a mini viewer at its offset
async function loadMiniViewerData(viewer: MiniViewer) {
  if (!isFileOpen.value) return

  const bytesPerPixel = getBytesPerPixel(viewer.format)
  const memorySize = viewer.width * viewer.height * bytesPerPixel

  // Check if we have a dropped file
  if ((window as any).__droppedFileData) {
    const fullData = (window as any).__droppedFileData as Uint8Array
    const endOffset = Math.min(viewer.offset + memorySize, fullData.length)
    let data = fullData.slice(viewer.offset, endOffset)
    if (data.length < memorySize) {
      const padded = new Uint8Array(memorySize)
      padded.set(data)
      data = padded
    }
    viewer.memoryData = data
  } else {
    const data = await readMemoryChunk(viewer.offset, memorySize)
    if (data) {
      if (data.length < memorySize) {
        const padded = new Uint8Array(memorySize)
        padded.set(data)
        viewer.memoryData = padded
      } else {
        viewer.memoryData = data
      }
    }
  }
}

function copyAddress() {
  const address = `0x${contextMenuOffset.value.toString(16).toUpperCase()}`
  navigator.clipboard.writeText(address)
  console.log('Copied address:', address)
}

function setFFTSamplePoint() {
  // The offset should be relative to the current view, not absolute
  fftSampleOffset.value = contextMenuOffset.value - currentOffset.value
  console.log('Set FFT sample point at:', `0x${contextMenuOffset.value.toString(16).toUpperCase()}`)

  // Store relative position for the indicator
  const canvasEl = document.querySelector('.canvas-container')
  if (canvasEl && contextMenuPosition.value) {
    const rect = canvasEl.getBoundingClientRect()
    fftRelativePosition.value = {
      x: (contextMenuPosition.value.x - rect.left) / rect.width,
      y: (contextMenuPosition.value.y - rect.top) / rect.height
    }
    updateFFTIndicatorPosition()
  }

  // Enable correlation if not already enabled
  if (!showCorrelation.value) {
    showCorrelation.value = true
  }
}

// Update FFT indicator position and sample offset based on relative position
function updateFFTIndicatorPosition() {
  if (!fftRelativePosition.value) return

  const canvasEl = document.querySelector('.canvas-container')
  if (canvasEl) {
    const rect = canvasEl.getBoundingClientRect()
    fftIndicatorPosition.value = {
      x: fftRelativePosition.value.x * rect.width,
      y: fftRelativePosition.value.y * rect.height
    }

    // Recalculate the sample offset based on the relative position
    const x = fftRelativePosition.value.x * rect.width
    const y = fftRelativePosition.value.y * rect.height

    const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
    let clickOffset = 0

    if (columnMode.value) {
      // Column mode calculation
      const pixelX = Math.floor((x / rect.width) * canvasWidth.value)
      const pixelY = Math.floor((y / rect.height) * canvasHeight.value)

      const totalColumnWidth = columnWidth.value + columnGap.value
      const columnIndex = Math.floor(pixelX / totalColumnWidth)
      const xInColumn = pixelX % totalColumnWidth

      if (xInColumn < columnWidth.value) {
        const memoryX = xInColumn
        const memoryY = columnIndex * canvasHeight.value + pixelY
        clickOffset = memoryY * displayWidth.value * bytesPerPixel + memoryX * bytesPerPixel
      }
    } else {
      // Simple linear mode
      const row = Math.floor((y / rect.height) * displayHeight.value)
      const col = Math.floor((x / rect.width) * displayWidth.value)
      clickOffset = row * displayWidth.value * bytesPerPixel + col * bytesPerPixel
    }

    // Update the FFT sample offset relative to the loaded memory data
    // Don't include currentOffset since memoryData starts at currentOffset
    fftSampleOffset.value = clickOffset
  }
}

// Clear FFT sample point
function clearFFTSamplePoint() {
  fftSampleOffset.value = 0
  fftRelativePosition.value = null
  fftIndicatorPosition.value = null
  console.log('Cleared FFT sample point')
}

// Drag and drop handlers
function onDragOver(event: DragEvent) {
  isDraggingFile.value = true
}

function onDragLeave() {
  isDraggingFile.value = false
}

async function onDrop(event: DragEvent) {
  isDraggingFile.value = false

  const files = event.dataTransfer?.files
  if (!files || files.length === 0) return

  const file = files[0]
  console.log('Dropped file:', file.name, 'Size:', file.size)

  // Create a synthetic file handle from the dropped file
  isLoadingFile.value = true
  fileName.value = file.name
  fileSize.value = file.size

  // Read the file content directly
  const arrayBuffer = await file.arrayBuffer()
  const data = new Uint8Array(arrayBuffer)

  // Store for memory reading
  (window as any).__droppedFileData = data

  isFileOpen.value = true
  await refreshMemory()
  isLoadingFile.value = false
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

// Helper: Format hex address
function formatHex(value: number): string {
  return '0x' + value.toString(16).toUpperCase().padStart(8, '0')
}

// Canvas drag scrolling
function startCanvasDrag(event: MouseEvent) {
  // Only start drag with left mouse button and if not clicking on controls
  if (event.button !== 0) return

  // Check if the click originated from within a mini viewer
  const target = event.target as HTMLElement
  if (target.closest('.mini-bitmap-viewer')) return // Don't drag if clicking on mini viewer

  // Check if the target is the main memory canvas
  if (target.tagName !== 'CANVAS') return
  if (!target.classList.contains('memory-canvas')) return // Only drag on main canvas

  // Don't start canvas drag if any mini viewer is being dragged
  if (miniViewers.value.some(v => v.isDragging)) return

  isCanvasDragging.value = true
  canvasDragStart.value = {
    x: event.clientX,
    y: event.clientY,
    offset: currentOffset.value
  }
  event.preventDefault()
}

function handleCanvasDrag(event: MouseEvent) {
  if (!isCanvasDragging.value) return

  // Stop dragging if any mini viewer starts being dragged
  if (miniViewers.value.some(v => v.isDragging)) {
    stopCanvasDrag()
    return
  }

  const deltaX = event.clientX - canvasDragStart.value.x
  const deltaY = event.clientY - canvasDragStart.value.y

  const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
  const bytesPerRow = displayWidth.value * bytesPerPixel

  // Vertical: move by full width (in bytes) per pixel of drag
  // Horizontal: move by the mouse motion amount in bytes
  const verticalDelta = -deltaY * bytesPerRow
  const horizontalDelta = -deltaX * bytesPerPixel  // Negative so dragging right decreases offset

  // Combine both deltas
  const offsetDelta = verticalDelta + horizontalDelta

  const newOffset = Math.max(0, Math.min(fileSize.value - 1,
    Math.floor(canvasDragStart.value.offset + offsetDelta)))

  if (newOffset !== currentOffset.value) {
    currentOffset.value = newOffset
    sliderPosition.value = newOffset
    offsetInput.value = formatHex(newOffset)
    refreshMemory()
    // Update anchors while dragging
    updateMiniViewerAnchors()
  }
}

function stopCanvasDrag() {
  isCanvasDragging.value = false
  // Update anchor points after drag completes
  updateMiniViewerAnchors()
}

// Watch for display changes
watch([displayWidth, selectedFormat, splitComponents, columnMode, columnWidth, columnGap], () => {
  if (isFileOpen.value) {
    refreshMemory()
  }
})

// Update mini viewer anchor points
async function updateMiniViewerAnchors() {
  // Always update anchors, even during canvas drag
  for (const viewer of miniViewers.value) {
    if (viewer.anchorMode === 'position' && viewer.relativeAnchorPos) {
      // In position mode, calculate anchor based on relative position
      const canvas = document.querySelector('.memory-canvas') as HTMLCanvasElement
      if (canvas) {
        const rect = canvas.getBoundingClientRect()
        viewer.anchorPoint = {
          x: rect.left + viewer.relativeAnchorPos.x * rect.width,
          y: rect.top + viewer.relativeAnchorPos.y * rect.height
        }

        // Also update the offset to show what's at this position
        const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
        const relY = viewer.relativeAnchorPos.y
        const relX = viewer.relativeAnchorPos.x
        const row = Math.floor(relY * displayHeight.value)
        const col = Math.floor(relX * displayWidth.value)
        const newOffset = currentOffset.value + row * displayWidth.value * bytesPerPixel + col * bytesPerPixel

        if (viewer.offset !== newOffset) {
          viewer.offset = newOffset
          viewer.title = `Viewer @ 0x${newOffset.toString(16).toUpperCase()}`
          // Reload data for the new offset
          await loadMiniViewerData(viewer)
        }
      }
    } else {
      // In address mode, anchor follows the memory address
      const newAnchor = getCanvasPositionFromOffset(viewer.offset)
      viewer.anchorPoint = newAnchor // Set to null if not visible
    }
  }
}

// Handle anchor mode changes
async function handleAnchorModeChange(id: number, mode: 'address' | 'position', relativePos?: { x: number, y: number }) {
  const viewer = miniViewers.value.find(v => v.id === id)
  if (viewer) {
    viewer.anchorMode = mode
    if (mode === 'position' && relativePos) {
      viewer.relativeAnchorPos = relativePos
    }
    // Update anchor immediately
    await updateMiniViewerAnchors()
  }
}

// Update anchors when offset changes
watch(currentOffset, () => {
  updateMiniViewerAnchors()
  updateFFTIndicatorPosition()
})

// Export for template
const PixelFormatExport = PixelFormat

// Keyboard shortcuts
function handleKeyboard(event: KeyboardEvent) {
  if (!isFileOpen.value) return

  switch(event.key) {
    case 'ArrowUp':
      if (event.metaKey || event.ctrlKey) {
        jumpToStart()
      } else {
        pageUp()
      }
      event.preventDefault()
      break
    case 'ArrowDown':
      if (event.metaKey || event.ctrlKey) {
        jumpToEnd()
      } else {
        pageDown()
      }
      event.preventDefault()
      break
    case 'PageUp':
      pageUp()
      event.preventDefault()
      break
    case 'PageDown':
      pageDown()
      event.preventDefault()
      break
    case 'Home':
      jumpToStart()
      event.preventDefault()
      break
    case 'End':
      jumpToEnd()
      event.preventDefault()
      break
    case 'm':
      // Toggle magnifying glass with lock
      if (magnifyingGlassVisible.value) {
        closeMagnifyingGlass()
      } else {
        // Get mouse position from last known position or center of viewport
        const canvas = memoryCanvasRef.value?.canvas
        if (canvas) {
          mainCanvas.value = canvas
          const rect = canvas.getBoundingClientRect()
          magnifyingGlassCenterX.value = Math.floor(rect.width / 2)
          magnifyingGlassCenterY.value = Math.floor(rect.height / 2)
          magnifyingGlassLocked.value = true // Lock when using lowercase 'm'
          magnifyingGlassVisible.value = true
        }
      }
      event.preventDefault()
      break
    case 'M':
      // Toggle magnifying glass without lock
      if (magnifyingGlassVisible.value) {
        closeMagnifyingGlass()
      } else {
        // Get mouse position from last known position or center of viewport
        const canvas = memoryCanvasRef.value?.canvas
        if (canvas) {
          mainCanvas.value = canvas
          const rect = canvas.getBoundingClientRect()
          magnifyingGlassCenterX.value = Math.floor(rect.width / 2)
          magnifyingGlassCenterY.value = Math.floor(rect.height / 2)
          magnifyingGlassLocked.value = false // Don't lock when using uppercase 'M'
          magnifyingGlassVisible.value = true
        }
      }
      event.preventDefault()
      break
  }
}

// Setup on mount
onMounted(async () => {
  // Only auto-connect to QMP if running under Electron
  // if (qmpAvailable.value) {
  //   connectQmp().catch(console.error)
  // }

  // Initialize change detection module
  loadChangeDetectionModule().catch(console.error)

  // Update FFT indicator on window resize
  window.addEventListener('resize', updateFFTIndicatorPosition)

  // Add event listeners
  window.addEventListener('keydown', handleKeyboard)
  window.addEventListener('mousemove', handleCanvasDrag)
  window.addEventListener('mouseup', stopCanvasDrag)

  // Wait for DOM to be ready
  await nextTick()

  // Observe canvas container size
  const canvasContainer = document.querySelector('.canvas-container')
  if (canvasContainer) {
    // Get initial size
    const rect = canvasContainer.getBoundingClientRect()
    const initialHeight = Math.floor(rect.height - 40) // Subtract padding
    if (initialHeight > 0) {
      canvasContainerHeight.value = initialHeight

      // Refresh if file is already open
      if (isFileOpen.value) {
        await refreshMemory()
      }
    }

    const resizeObserver = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const newHeight = Math.floor(entry.contentRect.height - 40) // Subtract padding
        if (newHeight > 0 && newHeight !== canvasContainerHeight.value) {
          canvasContainerHeight.value = newHeight
          if (isFileOpen.value) {
            refreshMemory()
          }
        }
      }
    })
    if (canvasContainer instanceof Element) {
      resizeObserver.observe(canvasContainer)
    }

    // Store observer for cleanup
    (window as any).__canvasResizeObserver = resizeObserver
  }
})

// Cleanup
onUnmounted(() => {
  window.removeEventListener('keydown', handleKeyboard)
  window.removeEventListener('mousemove', handleCanvasDrag)
  window.removeEventListener('mouseup', stopCanvasDrag)
  window.removeEventListener('resize', updateFFTIndicatorPosition)
  stopAutoRepeat() // Clean up any active auto-repeat

  // Clean up ResizeObserver
  if ((window as any).__canvasResizeObserver) {
    (window as any).__canvasResizeObserver.disconnect()
    delete (window as any).__canvasResizeObserver
  }
})
// Electron file handlers
async function handleElectronFileOpened({ path, size }) {
  console.log(`Electron: Opened ${path} (${size} bytes)`)
  fileSize.value = size
  fileName.value = path.split('/').pop() || 'memory.bin'
  isFileOpen.value = true
  currentOffset.value = 0

  // Load initial data
  await refreshMemory()

  // Start change detection if enabled
  if (changeDetectionEnabled.value) {
    startChangeDetection()
  }
}

async function handleElectronFileRefreshed({ path, size }) {
  console.log(`Electron: Refreshed ${path} (${size} bytes)`)
  fileSize.value = size

  // Reload current view
  await refreshMemory()
}

function handleElectronFileClosed() {
  console.log('Electron: File closed')
  isFileOpen.value = false
  memoryData.value = null
  fileSize.value = 0
  fileName.value = ''
  stopChangeDetection()
}

// Override refreshMemory to use Electron API when available
const originalRefreshMemory = refreshMemory
async function refreshMemoryElectron() {
  if (!isFileOpen.value) return

  // Use Electron API if available
  if (isElectron.value && window.electronAPI) {
    const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
    const memorySize = displayWidth.value * displayHeight.value * bytesPerPixel

    try {
      const result = await window.electronAPI.readMemoryChunk(currentOffset.value, memorySize)
      if (result.success) {
        memoryData.value = result.data
        return
      }
    } catch (error) {
      console.error('Electron read failed:', error)
    }
  }

  // Fallback to original method
  return originalRefreshMemory()
}

// Use Electron version if available
if (isElectron.value) {
  refreshMemory = refreshMemoryElectron
}
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

/* Main content layout */
.main-content {
  display: flex;
  flex: 1;
  overflow: hidden;
}

.overview-container {
  display: flex;
  flex-direction: column;
  width: 128px;
  background: #0a0a0a;
  border-right: 1px solid #333;
}

.scan-progress {
  padding: 8px;
  border-bottom: 1px solid #333;
}

.progress-bar {
  width: 100%;
  height: 6px;
  background: #1a1a1a;
  border-radius: 3px;
  overflow: hidden;
  margin-bottom: 4px;
}

.progress-fill {
  height: 100%;
  background: linear-gradient(90deg, #00ff00, #00cc00);
  border-radius: 3px;
  transition: width 0.2s ease;
}

.progress-text {
  font-size: 10px;
  color: #888;
  text-align: center;
}

/* Left sidebar with vertical slider */
.address-slider-sidebar {
  width: 60px;
  background: #2d2d30;
  border-right: 1px solid #3e3e42;
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 15px 0;
  gap: 10px;
}

.nav-button {
  background: #3c3c3c;
  border: 1px solid #555;
  color: #e0e0e0;
  width: 40px;
  height: 32px;
  font-size: 16px;
  cursor: pointer;
  border-radius: 3px;
  display: flex;
  align-items: center;
  justify-content: center;
}

.nav-button:hover {
  background: #4c4c4c;
}

.slider-container {
  flex: 1;
  position: relative;
  width: 100%;
  display: flex;
  justify-content: center;
  align-items: center;
  padding: 10px 0;
}

.slider-track {
  position: absolute;
  width: 4px;
  height: 100%;
  background: #3c3c3c;
  border-radius: 2px;
  pointer-events: none;
}

.address-slider-vertical {
  writing-mode: bt-lr; /* IE */
  -webkit-appearance: slider-vertical; /* WebKit */
  width: 100%;
  height: 100%;
  background: transparent;
  outline: none;
  cursor: pointer;
}

/* Webkit browsers */
.address-slider-vertical::-webkit-slider-track {
  width: 4px;
  background: transparent;
}

.address-slider-vertical::-webkit-slider-thumb {
  -webkit-appearance: none;
  appearance: none;
  width: 20px;
  height: 20px;
  background: #0e639c;
  cursor: pointer;
  border-radius: 50%;
  border: 2px solid #1177bb;
  position: relative;
  z-index: 1;
}

.address-slider-vertical::-webkit-slider-thumb:hover {
  background: #1177bb;
  border-color: #1e8ad6;
}

/* Firefox */
.address-slider-vertical::-moz-range-track {
  width: 4px;
  background: transparent;
}

.address-slider-vertical::-moz-range-thumb {
  width: 20px;
  height: 20px;
  background: #0e639c;
  cursor: pointer;
  border-radius: 50%;
  border: 2px solid #1177bb;
  border: none;
}

.slider-address {
  font-family: 'Courier New', monospace;
  font-size: 10px;
  color: #e0e0e0;
  text-align: center;
  padding: 5px;
}

.slider-address .separator {
  margin: 2px 0;
  color: #666;
  font-size: 8px;
}

.canvas-and-correlator {
  flex: 1;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.correlator-wrapper {
  display: flex;
  justify-content: center;
  align-items: center;
  padding: 0 20px;
  background: #1e1e1e;
}

.canvas-container {
  flex: 1;
  overflow: auto;
  background: #1e1e1e;
  display: flex;
  justify-content: center;
  align-items: center;
  padding: 20px;
  position: relative;
  transition: background-color 0.2s;
}

.canvas-container.drag-over {
  background: #2a4a2a;
  border: 2px dashed #4ec9b0;
}

.canvas-container.drag-over::after {
  content: '📁 Drop memory file here';
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  font-size: 24px;
  color: #4ec9b0;
  pointer-events: none;
  z-index: 1000;
  background: rgba(0, 0, 0, 0.8);
  padding: 20px 40px;
  border-radius: 8px;
}

.canvas-container.dragging {
  cursor: grabbing !important;
}

.canvas-container canvas {
  cursor: grab;
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