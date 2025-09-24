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
            <option :value="PixelFormat.RGB888">RGB</option>
            <option :value="PixelFormat.RGBA8888">RGBA</option>
            <option :value="PixelFormat.BGR888">BGR</option>
            <option :value="PixelFormat.BGRA8888">BGRA</option>
            <option :value="PixelFormat.ARGB8888">ARGB</option>
            <option :value="PixelFormat.ABGR8888">ABGR</option>
            <option :value="PixelFormat.BINARY">Binary</option>
            <option :value="PixelFormat.HEX_PIXEL">Hex Pixel</option>
            <option :value="PixelFormat.CHAR_8BIT">Char 8-bit</option>
            <option :value="PixelFormat.RGBA8888_SPLIT">R|G|B|A</option>
            <option :value="PixelFormat.BGRA8888_SPLIT">B|G|R|A</option>
            <option :value="PixelFormat.ARGB8888_SPLIT">A|R|G|B</option>
            <option :value="PixelFormat.ABGR8888_SPLIT">A|B|G|R</option>
          </select>
        </label>
      </div>

      <div class="control-group">
        <label>
          Address:
          <input
            type="text"
            v-model="offsetInput"
            @change="updateOffset"
            @keyup.enter="updateOffset"
            placeholder="0x0 or hex address"
            class="offset-input"
            title="Enter address in hex (with or without 0x prefix)"
          >
        </label>

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
        <button @click="refreshMemory" :disabled="!isFileOpen">
          🔄 Refresh
        </button>
        <button @click="runKernelDiscovery" :disabled="!isFileOpen || kernelDiscoveryRunning">
          {{ kernelDiscoveryRunning ? '⏳ Discovering...' : '🔍 Kernel Discovery' }}
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
          @memory-hover="(offset, event) => handleMemoryHover(offset, event)"
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

    <!-- Kernel Page Tooltip -->
    <KernelPageTooltip
      v-if="tooltipPageInfo"
      :page-info="tooltipPageInfo"
      :x="tooltipPosition.x"
      :y="tooltipPosition.y"
      :visible="tooltipVisible"
      @close="closeTooltip"
    />

    <!-- Status Bar -->
    <div class="status-bar">
      <span v-if="hoveredOffset !== null">
        Hover: 0x{{ hoveredOffset.toString(16).toUpperCase().padStart(8, '0') }}
        <span v-if="kernelPageDB.hasData()" class="db-indicator">
          [DB Ready]
        </span>
        <span v-if="pageCoverage" class="coverage-indicator">
          [Coverage: {{ pageCoverage }}%]
        </span>
      </span>
      <span v-if="clickedOffset !== null">
        Click: 0x{{ clickedOffset.toString(16).toUpperCase().padStart(8, '0') }}
      </span>
      <span v-if="error" class="error">
        {{ error }}
      </span>
    </div>

    <!-- Kernel Discovery Modal -->
    <div v-if="kernelDiscoveryModal" class="modal-overlay" @click.self="closeKernelDiscovery">
      <div class="modal-content kernel-discovery-modal">
        <div class="modal-header">
          <h2>🔍 Kernel Memory Discovery Report</h2>
          <button class="modal-close" @click="closeKernelDiscovery">✕</button>
        </div>

        <div v-if="kernelDiscoveryRunning" class="discovery-progress">
          <div class="spinner"></div>
          <p>{{ kernelDiscoveryStatus }}</p>
        </div>

        <div v-else-if="kernelDiscoveryResults" class="discovery-results">
          <!-- Summary Statistics -->
          <div class="stats-grid">
            <div class="stat-card">
              <div class="stat-value">{{ kernelDiscoveryResults.processes?.length || 0 }}</div>
              <div class="stat-label">Processes Found</div>
            </div>
            <div class="stat-card">
              <div class="stat-value">{{ kernelDiscoveryResults.kernelPTEs?.length || 0 }}</div>
              <div class="stat-label">Kernel PTEs</div>
            </div>
            <div class="stat-card">
              <div class="stat-value">{{ formatHex(kernelDiscoveryResults.swapperPgDir || 0) }}</div>
              <div class="stat-label">swapper_pg_dir</div>
            </div>
          </div>

          <!-- Process List -->
          <div class="section">
            <h3>Discovered Processes</h3>
            <div class="process-table-container">
              <table class="process-table">
                <thead>
                  <tr>
                    <th @click="sortProcesses('pid')">PID ↕</th>
                    <th @click="sortProcesses('name')">Name ↕</th>
                    <th @click="sortProcesses('ptes')">PTEs ↕</th>
                    <th>Actions</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="proc in sortedProcesses" :key="proc.pid">
                    <td>{{ proc.pid }}</td>
                    <td>{{ proc.name || proc.comm || 'unknown' }}</td>
                    <td>{{ proc.ptes?.length || proc.pteCount || 0 }}</td>
                    <td>
                      <button class="small-button" @click="viewProcessPTEs(proc)">View PTEs</button>
                    </td>
                  </tr>
                </tbody>
              </table>
            </div>
          </div>

          <!-- Export Options -->
          <div class="export-buttons">
            <button @click="exportJSON">📄 Export JSON</button>
            <button @click="exportHTML">📊 Export HTML Report</button>
            <button @click="copyToClipboard">📋 Copy Summary</button>
          </div>
        </div>

        <div v-else class="no-results">
          <p>No discovery results yet</p>
          <button @click="startDiscovery" class="primary-button">🔍 Run Discovery</button>
        </div>
      </div>
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
import KernelPageTooltip from './components/KernelPageTooltip.vue'
import { useFileSystemAPI } from './composables/useFileSystemAPI'
import { useQmpBridge } from './composables/useQmpBridge'
import { PixelFormat } from './composables/useWasmRenderer'
import { useChangeDetection } from './composables/useChangeDetection'
import { qmpAvailable } from './utils/electronDetect'
import { KernelDiscovery } from './kernel-discovery'
import { kernelPageDB } from './kernel-page-database'
import type { PageInfo } from './kernel-page-database'
import type { PageCollection, PageInfo as NewPageInfo } from './page-info'

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
  getChunkAtOffset,
  testChunkZero
} = useChangeDetection()

// Memory data
const memoryData = ref<Uint8Array | null>(null)
const isLoadingFile = ref(false)
const memoryCanvasRef = ref<InstanceType<typeof MemoryCanvas>>()
const lastScanTime = ref<number>(0)

// Check if running in Electron
const isElectron = ref(window.electronAPI && window.electronAPI.isElectron)
// Incremental scanning state - track position for each scan type
const scanProgress = ref({
  // Global scan state
  totalChunks: 0,
  scanning: false,
  chunkSize: 65536, // 64KB per chunk
  chunksPerFrame: 10000, // Process 10000 chunks (640MB) per frame - 100x faster

  // Separate positions for each scan type
  grandScanPosition: 0,    // Full memory scan position
  mapScanPosition: 0,      // Memory map window scan position
  viewportScanFrame: 0,    // Track frames for viewport (15Hz = every 4 frames at 60fps)

  // Deprecated - will remove
  currentChunk: 0,
  pendingData: null as Uint8Array | null,
  autoRefresh: false,
  refreshDelay: 2000
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

// Kernel Discovery state
const kernelDiscoveryModal = ref(false)
const kernelDiscoveryRunning = ref(false)
const kernelDiscoveryStatus = ref('Initializing...')
const kernelDiscoveryResults = ref<any>(null)
const pageCollection = ref<PageCollection | null>(null)
const processSortKey = ref<'pid' | 'name' | 'ptes'>('pid')
const processSortReverse = ref(false)

// Display settings
const displayWidth = ref(1024)
// Height will be computed based on canvas container
const canvasContainerHeight = ref(768)
const selectedFormat = ref(PixelFormat.BGR888) // Start with BGR888 since user said it was working
const splitComponents = computed(() => {
  return selectedFormat.value === PixelFormat.RGBA8888_SPLIT ||
         selectedFormat.value === PixelFormat.BGRA8888_SPLIT ||
         selectedFormat.value === PixelFormat.ARGB8888_SPLIT ||
         selectedFormat.value === PixelFormat.ABGR8888_SPLIT
})
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
  } else if (enabled) {
    // Re-enable scanning
    if (isElectron.value && window.electronAPI && isFileOpen.value) {
      // For Electron, scan the full file
      startFullFileScan()
    } else if (memoryData.value) {
      // For web version, scan the loaded data
      performChangeDetectionScan(memoryData.value)
    } else {
    }
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
    [PixelFormat.RGB888]: 'RGB',
    [PixelFormat.RGBA8888]: 'RGBA',
    [PixelFormat.BGR888]: 'BGR',
    [PixelFormat.BGRA8888]: 'BGRA',
    [PixelFormat.ARGB8888]: 'ARGB',
    [PixelFormat.ABGR8888]: 'ABGR',
    [PixelFormat.BINARY]: 'BINARY',
    [PixelFormat.HEX_PIXEL]: 'HEX_PIXEL',
    [PixelFormat.CHAR_8BIT]: 'CHAR_8BIT',
    [PixelFormat.RGBA8888_SPLIT]: 'R|G|B|A',
    [PixelFormat.BGRA8888_SPLIT]: 'B|G|R|A',
    [PixelFormat.ARGB8888_SPLIT]: 'A|R|G|B',
    [PixelFormat.ABGR8888_SPLIT]: 'A|B|G|R'
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
  return Math.round((scanProgress.value.grandScanPosition / scanProgress.value.totalChunks) * 100)
})

const maxSliderPosition = computed(() => {
  // Simply use the full file size
  return fileSize.value || 0
})

// Inverted slider position (top = 0, bottom = max)
const invertedSliderPosition = computed(() => {
  return maxSliderPosition.value - sliderPosition.value
})

// Sorted processes for display
const sortedProcesses = computed(() => {
  if (!kernelDiscoveryResults.value?.processes) return []
  const procs = [...kernelDiscoveryResults.value.processes]

  procs.sort((a, b) => {
    let aVal, bVal
    switch (processSortKey.value) {
      case 'pid':
        aVal = a.pid
        bVal = b.pid
        break
      case 'name':
        aVal = a.comm || ''
        bVal = b.comm || ''
        break
      case 'ptes':
        aVal = a.pteCount || 0
        bVal = b.pteCount || 0
        break
      default:
        return 0
    }

    const result = aVal < bVal ? -1 : aVal > bVal ? 1 : 0
    return processSortReverse.value ? -result : result
  })

  return procs
})

// Interaction state
const hoveredOffset = ref<number | null>(null)
const clickedOffset = ref<number | null>(null)

// Tooltip state
const tooltipVisible = ref(false)
const tooltipPageInfo = ref<PageInfo | null>(null)
const tooltipPosition = ref({ x: 0, y: 0 })
const tooltipTimer = ref<number | null>(null)

// Combined error state
const error = computed(() => fileError.value)

// Page coverage metric - percentage of memory pages we have info for
const pageCoverage = computed(() => {
  if (!pageCollection.value || !fileSize.value) return null

  // Calculate how many 4KB pages are in the current view
  const pageSize = 4096
  const totalPagesInFile = Math.floor(fileSize.value / pageSize)

  // Get stats from PageCollection
  const stats = pageCollection.value.getStats()
  const mappedPages = stats.totalPages

  // Calculate percentage
  const percentage = (mappedPages / totalPagesInFile) * 100
  return percentage.toFixed(1)
})

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
    // Don't scan just the viewport - that's handled separately
    // performChangeDetectionScan(data)
  } else {
    const data = await readMemoryChunk(currentOffset.value, memorySize)
    if (data) {
      // Ensure we have the full size requested, pad if necessary
      if (data.length < memorySize) {
        const padded = new Uint8Array(memorySize)
        padded.set(data)
        memoryData.value = padded
        // Don't scan just the viewport
        // performChangeDetectionScan(padded)
      } else {
        memoryData.value = data
        // Don't scan just the viewport
        // performChangeDetectionScan(data)
      }
    }
  }
}

// Perform change detection scan incrementally
function performChangeDetectionScan(data: Uint8Array) {
  // Skip if disabled
  if (!changeDetectionEnabled.value) return

  // If already scanning, just update the pending data but don't restart
  if (scanProgress.value.scanning) {
    // Update the data for future scans but don't interrupt current scan
    scanProgress.value.pendingData = data
    return
  }

  // Store the data and start/continue incremental scan
  scanProgress.value.pendingData = data
  scanProgress.value.totalChunks = Math.ceil(data.length / scanProgress.value.chunkSize)

  // Only reset to 0 if this is the first scan or data size changed
  if (!changeDetectionState.value || changeDetectionState.value.totalSize !== data.length) {
    scanProgress.value.currentChunk = 0
    // Start new scan
  } else {
    // Continue from where we left off
  }

  scanProgress.value.scanning = true

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

  // Debug: log every 10% progress
  if (percentComplete % 10 === 0 && percentComplete > 0) {
  }

  if (endChunk < scanProgress.value.totalChunks) {
    // More chunks to process - schedule next batch
    requestAnimationFrame(processNextScanBatch)
  } else {
    // Reached end of memory - wrap around to continue scanning
    state.lastCheckTime = Date.now()

    // Reset to start for continuous scanning
    scanProgress.value.currentChunk = 0

    // Continue scanning if enabled
    if (changeDetectionEnabled.value && scanProgress.value.pendingData) {
      // Small delay to avoid blocking, then continue
      setTimeout(() => {
        if (changeDetectionEnabled.value && scanProgress.value.pendingData) {
          scanProgress.value.scanning = true
          processNextScanBatch()
        }
      }, 10) // Very small delay to yield to UI
    } else {
      scanProgress.value.scanning = false
    }
  }
}

// Generic chunk processor - scans a range of chunks and updates position
async function processChunkRange(startIdx: number, endIdx: number, currentPos: number, chunksToDo: number): Promise<number> {
  if (!window.electronAPI || !changeDetectionState.value) return currentPos

  const state = changeDetectionState.value
  const chunkSize = scanProgress.value.chunkSize
  let processed = 0
  let position = currentPos

  // Pre-populate chunks for the entire range if needed
  // This ensures the memory map can display chunks even if they haven't been scanned yet
  while (state.chunks.length < endIdx) {
    state.chunks.push({
      offset: state.chunks.length * chunkSize,
      size: chunkSize,
      checksum: 0,
      isZero: false,  // Don't assume zero until scanned
      hasChanged: false,
      scanned: false,  // Add flag to track if actually scanned
      lastChangeTime: 0,
      scanCount: 0
    })
  }

  // Process chunks from current position, wrapping around if needed
  while (processed < chunksToDo) {
    // Skip if outside the requested range
    if (position < startIdx || position >= endIdx) {
      position = startIdx // Wrap to beginning of range
      if (position >= endIdx) break // Empty range
    }

    // Read and process the chunk
    const offset = position * chunkSize
    const size = Math.min(chunkSize, fileSize.value - offset)

    try {
      const result = await window.electronAPI.readMemoryChunk(offset, size)
      if (result.success && result.data) {
        const chunkData = result.data

        // Test for zero
        const isZero = testChunkZero(chunkData, 0, size)

        // Calculate checksum
        const checksum = isZero ? 0 :
          chunkData.reduce((sum, byte) => ((sum << 5) | (sum >>> 27)) ^ byte, 0)

        // Check if changed
        const prevChunk = state.chunks[position]
        const isFirstScan = !prevChunk?.scanned
        const hasChanged = !isFirstScan && prevChunk &&
          (prevChunk.checksum !== checksum || prevChunk.isZero !== isZero)

        // Update state using splice for Vue reactivity
        const now = Date.now()
        const existingChunk = state.chunks[position]
        state.chunks.splice(position, 1, {
          offset,
          size,
          checksum,
          isZero,
          hasChanged,
          scanned: true,  // Mark as actually scanned
          lastChangeTime: hasChanged ? now : (existingChunk?.lastChangeTime || 0),  // Track when it changed
          scanCount: (existingChunk?.scanCount || 0) + 1  // Track number of scans
        })
      }
    } catch (error) {
      // Silent failure - chunk remains unchanged
    }

    processed++
    position++

    // Wrap around if we hit the end
    if (position >= endIdx) {
      position = startIdx
    }
  }

  return position // Return new position for next call
}

// Scan the entire file for Electron version
async function startFullFileScan() {
  if (!isElectron.value || !window.electronAPI || !isFileOpen.value) return
  if (scanProgress.value.scanning) return

  // Initialize state FIRST to prevent component recreation
  if (!changeDetectionState.value) {
    changeDetectionState.value = {
      chunks: [],
      pageSize: 4096,
      chunkSize: scanProgress.value.chunkSize,
      totalSize: fileSize.value,
      lastCheckTime: Date.now()
    }
  }

  // Initialize for full file scan
  scanProgress.value.totalChunks = Math.ceil(fileSize.value / scanProgress.value.chunkSize)
  scanProgress.value.grandScanPosition = 0  // Reset grand scan position
  scanProgress.value.mapScanPosition = 0     // Reset map scan position
  scanProgress.value.viewportScanFrame = 0   // Reset viewport frame counter
  scanProgress.value.scanning = true

  processFullFileBatch()
}

async function processFullFileBatch() {
  if (!scanProgress.value.scanning || !changeDetectionEnabled.value) {
    scanProgress.value.scanning = false
    return
  }

  // Make sure totalChunks is initialized
  if (!scanProgress.value.totalChunks || scanProgress.value.totalChunks === 0) {
    scanProgress.value.totalChunks = Math.ceil(fileSize.value / scanProgress.value.chunkSize)
  }

  const chunkSize = scanProgress.value.chunkSize
  let chunksUsed = 0
  const totalBudget = scanProgress.value.chunksPerFrame // 1000 chunks

  // Calculate zones
  const bytesPerPixel = getBytesPerPixel(selectedFormat.value)
  const viewportSize = displayWidth.value * displayHeight.value * bytesPerPixel
  const viewportStartChunk = Math.floor(currentOffset.value / chunkSize)
  const viewportEndChunk = Math.min(
    Math.ceil((currentOffset.value + viewportSize) / chunkSize),
    scanProgress.value.totalChunks
  )

  // Memory map window - 100 rows × 18 chunks = 1800 chunks visible
  const mapCenterChunk = Math.floor(currentOffset.value / chunkSize)
  const mapHalfWindow = 900 // Half of 1800 visible chunks
  const mapStartChunk = Math.max(0, mapCenterChunk - mapHalfWindow)
  const mapEndChunk = Math.min(scanProgress.value.totalChunks, mapCenterChunk + mapHalfWindow)

  // 1. Viewport scan - scan immediately for responsiveness, then every 15 frames
  const viewportChunks = viewportEndChunk - viewportStartChunk
  if (viewportChunks > 0) {
    // Always scan viewport for immediate feedback when scrolling
    await processChunkRange(viewportStartChunk, viewportEndChunk, viewportStartChunk, viewportChunks)
    chunksUsed += viewportChunks
  }

  // 2. Memory map window scan - prioritize filling the entire window quickly
  const mapChunkBudget = Math.min(3000, totalBudget - chunksUsed) // Increased to 3000 for faster fill
  if (mapChunkBudget > 0 && mapEndChunk > mapStartChunk) {
    // Reset map position if it's outside the current window
    if (scanProgress.value.mapScanPosition < mapStartChunk || scanProgress.value.mapScanPosition >= mapEndChunk) {
      scanProgress.value.mapScanPosition = mapStartChunk
    }

    // Calculate how many chunks we need to scan to fill the window
    const windowSize = mapEndChunk - mapStartChunk
    const chunksToScan = Math.min(mapChunkBudget, windowSize)

    const newMapPosition = await processChunkRange(
      mapStartChunk,
      mapEndChunk,
      scanProgress.value.mapScanPosition,
      chunksToScan
    )
    chunksUsed += chunksToScan
    scanProgress.value.mapScanPosition = newMapPosition
  }

  // 3. Grand scan - use most of remaining budget
  const remainingBudget = totalBudget - chunksUsed
  const grandScanBudget = Math.min(2000, remainingBudget) // Reduced to 2000 chunks for grand scan
  if (grandScanBudget > 0) {
    const newGrandPosition = await processChunkRange(
      0,
      scanProgress.value.totalChunks,
      scanProgress.value.grandScanPosition,
      grandScanBudget
    )
    chunksUsed += grandScanBudget
    scanProgress.value.grandScanPosition = newGrandPosition

  }


  // Force state update to trigger watchers
  if (changeDetectionState.value) {
    changeDetectionState.value.lastCheckTime = Date.now()
  }

  // Always continue scanning
  requestAnimationFrame(processFullFileBatch)
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
  let value = offsetInput.value.trim().toLowerCase()

  // Remove any 0x prefix if present
  if (value.startsWith('0x')) {
    value = value.slice(2)
  }

  // Determine if it's hex or decimal
  let newOffset = 0
  if (value === '') {
    newOffset = 0
  } else if (/[a-f]/.test(value)) {
    // Contains hex characters, must be hex
    newOffset = parseInt(value, 16) || 0
  } else if (value.length > 8) {
    // Very long number, probably hex even without letters
    newOffset = parseInt(value, 16) || 0
  } else {
    // Try as hex first (common for addresses), fall back to decimal
    const hexValue = parseInt(value, 16)
    const decValue = parseInt(value, 10)
    // If they're the same, it doesn't matter
    // If different, prefer hex for addresses
    newOffset = hexValue || decValue || 0
  }

  // Ensure offset is within bounds
  currentOffset.value = Math.max(0, Math.min(newOffset, fileSize.value - 1))

  // Update slider position and formatted input
  sliderPosition.value = currentOffset.value
  offsetInput.value = `0x${currentOffset.value.toString(16).toUpperCase()}`

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
function handleMemoryHover(offset: number, event?: MouseEvent) {
  hoveredOffset.value = offset

  // Show tooltip if we have page data
  const hasData = pageCollection.value || kernelPageDB.hasData();
  if (hasData && event) {
    // If tooltip is already visible, update position immediately
    if (tooltipVisible.value) {
      tooltipPosition.value = { x: event.clientX, y: event.clientY }

      // Also update the page info if hovering over a different page
      const fileOffset = offset + currentOffset.value;
      let physicalAddress;
      if (fileOffset < 0x40000000) {
        physicalAddress = fileOffset;
      } else {
        physicalAddress = fileOffset + 0x40000000;
      }

      // Check if this is a different page than currently shown
      const currentPA = tooltipPageInfo.value?.physicalAddress
      const currentPageBase = currentPA ? Math.floor(currentPA / 4096) * 4096 : null
      const newPageBase = Math.floor(physicalAddress / 4096) * 4096

      if (currentPageBase !== newPageBase) {
        // Different page - update tooltip content
        updateTooltipContent(physicalAddress, event)
      }
      return // Don't set timer if tooltip already visible
    }

    // Clear any existing timer
    if (tooltipTimer.value) {
      clearTimeout(tooltipTimer.value)
      tooltipTimer.value = null
    }

    // Set timer to show tooltip after short delay
    tooltipTimer.value = window.setTimeout(() => {
      const fileOffset = offset + currentOffset.value;
      let physicalAddress;
      if (fileOffset < 0x40000000) {
        physicalAddress = fileOffset;
      } else {
        physicalAddress = fileOffset + 0x40000000;
      }
      updateTooltipContent(physicalAddress, event)
    }, 500) // 500ms delay before showing tooltip
  }
}

// Helper function to update tooltip content
function updateTooltipContent(physicalAddress: number, event: MouseEvent) {
  // Try PageCollection first (newer, better data)
  if (pageCollection.value) {
    console.log(`Looking up PA 0x${physicalAddress.toString(16)} in PageCollection`);
    const pageInfo = pageCollection.value.getPageInfo(physicalAddress);
    console.log('PageInfo result:', pageInfo);
    if (pageInfo && pageInfo.mappings.length > 0) {
      // Generate tooltip text
      const tooltipText = pageCollection.value.getPageTooltip(physicalAddress);
      console.log('Page tooltip:', tooltipText);

      // Convert to old format temporarily
      tooltipPageInfo.value = {
        physicalAddress,
        references: pageInfo.mappings.map(m => ({
          pid: m.pid,
          processName: m.processName,
          type: 'pte' as const,
          virtualAddress: m.va,
          permissions: m.perms,
          sectionType: m.sectionType
        })),
        isKernel: pageInfo.isKernel,
        isShared: pageInfo.mappings.length > 1,
        isZero: false
      };
      tooltipPosition.value = { x: event.clientX, y: event.clientY }
      tooltipVisible.value = true;
      return;
    }
  }

  // Fall back to old database
  const pageInfo = kernelPageDB.getPageInfo(physicalAddress)
  if (pageInfo && pageInfo.references.length > 0) {
    tooltipPageInfo.value = pageInfo
    tooltipPosition.value = { x: event.clientX, y: event.clientY }
    tooltipVisible.value = true
  } else {
    // Show "not found" tooltip
    tooltipPageInfo.value = {
      physicalAddress,
      references: [],
      isKernel: false,
      isShared: false,
      isZero: false,
      notFound: true
    };
    tooltipPosition.value = { x: event.clientX, y: event.clientY }
    tooltipVisible.value = true
  }
}

function closeTooltip() {
  tooltipVisible.value = false
  tooltipPageInfo.value = null
  if (tooltipTimer.value) {
    clearTimeout(tooltipTimer.value)
    tooltipTimer.value = null
  }
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
    case PixelFormat.RGBA8888_SPLIT:
    case PixelFormat.BGRA8888_SPLIT:
    case PixelFormat.ARGB8888_SPLIT:
    case PixelFormat.ABGR8888_SPLIT:
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
watch([displayWidth, selectedFormat, columnMode, columnWidth, columnGap], () => {
  if (isFileOpen.value) {
    refreshMemory()
  }
  // Update magnifying glass canvas reference if it's open
  if (magnifyingGlassVisible.value) {
    nextTick(() => {
      const canvas = memoryCanvasRef.value?.canvas
      if (canvas) {
        mainCanvas.value = canvas
      }
    })
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

// Kernel Discovery Methods
async function runKernelDiscovery() {
  // Check if a file is actually open
  if (!isFileOpen.value) {
    alert('Please open a memory file first using the file browser')
    return
  }

  // Just open the modal and run discovery immediately
  kernelDiscoveryModal.value = true
  await startDiscovery()
}

async function startDiscovery() {
  kernelDiscoveryRunning.value = true
  kernelDiscoveryStatus.value = 'Initializing kernel discovery...'

  try {
    // Get memory data
    let fullMemory: Uint8Array | null = null

    console.log('Starting discovery, checking data sources...')
    console.log('- Dropped data:', !!(window as any).__droppedFileData)
    console.log('- Electron:', isElectron.value)
    console.log('- File handle:', !!fileHandle.value)
    console.log('- Memory data:', !!memoryData.value, memoryData.value?.length)
    console.log('- File size:', fileSize.value)

    // Try different sources in order of preference
    if ((window as any).__droppedFileData) {
      // Use dropped file data if available
      fullMemory = (window as any).__droppedFileData as Uint8Array
      kernelDiscoveryStatus.value = 'Using dropped file data...'
      console.log('Using dropped file data, size:', fullMemory.length)
    } else if (isElectron.value && window.electronAPI) {
      // Read FULL file for Electron - the good stuff is above 500MB!
      // But we need to read in chunks due to Node.js buffer limitations
      const chunkSize = 100 * 1024 * 1024; // 100MB chunks
      const totalChunks = Math.ceil(fileSize.value / chunkSize);

      kernelDiscoveryStatus.value = `Reading ${(fileSize.value / (1024*1024*1024)).toFixed(1)} GB file in ${totalChunks} chunks...`
      console.log(`Reading file in ${totalChunks} chunks of ${chunkSize} bytes each`)

      try {
        const chunks: Uint8Array[] = [];

        for (let i = 0; i < totalChunks; i++) {
          const offset = i * chunkSize;
          const size = Math.min(chunkSize, fileSize.value - offset);

          kernelDiscoveryStatus.value = `Reading chunk ${i + 1}/${totalChunks} (${(offset / (1024*1024)).toFixed(0)}-${((offset + size) / (1024*1024)).toFixed(0)} MB)...`

          const result = await window.electronAPI.readMemoryChunk(offset, size);
          if (result?.success && result.data) {
            chunks.push(result.data);
            // Chunk read successfully (commenting out verbose log)
          } else {
            console.error(`Failed to read chunk ${i + 1}:`, result);
            throw new Error(`Failed to read chunk at offset ${offset}`);
          }

          // Let UI update
          await new Promise(resolve => setTimeout(resolve, 10));
        }

        // Use PagedMemory approach to handle large files without array size limits
        console.log(`Using PagedMemory with ${chunks.length} chunks...`);
        kernelDiscoveryStatus.value = `Loading memory pages...`;

        const totalSize = chunks.reduce((sum, chunk) => sum + chunk.length, 0);

        // Import and use PagedMemory
        const { PagedMemory } = await import('./paged-memory');
        const { PagedKernelDiscovery } = await import('./kernel-discovery-paged');

        // Load chunks into PagedMemory
        const memory = await PagedMemory.fromChunks(chunks, (percent) => {
          kernelDiscoveryStatus.value = `Loading memory pages... ${percent.toFixed(1)}%`;
        });

        console.log(`PagedMemory loaded: ${memory.getMemoryUsage()}`);
        kernelDiscoveryStatus.value = `Analyzing memory (${(totalSize / (1024*1024)).toFixed(0)}MB)...`;

        // Run discovery using PagedKernelDiscovery
        const discovery = new PagedKernelDiscovery(memory);
        const results = await discovery.discover(totalSize);

        kernelDiscoveryResults.value = {
          processes: results.processes || [],
          kernelPTEs: results.kernelPtes || [],
          swapperPgDir: results.swapperPgDir || 0,
          statistics: {
            totalChunksProcessed: chunks.length,
            totalProcesses: results.processes?.length || 0,
            totalPTEs: results.stats?.kernelPTEs || results.kernelPtes?.length || 0,
            memoryAnalyzed: totalSize
          }
        };

        // Store the PageCollection if available
        if (results.pageCollection) {
          pageCollection.value = results.pageCollection;
          console.log('PageCollection ready with', results.pageCollection.getStatistics());
        }

        // Also populate the old kernel page database for compatibility
        kernelDiscoveryStatus.value = 'Building page reference database...';
        kernelPageDB.populate(results);
        const dbStats = kernelPageDB.getStatistics();
        console.log('Kernel page database populated:', dbStats);

        kernelDiscoveryRunning.value = false;
        const pteCount = results.stats?.kernelPTEs || results.kernelPtes?.length || 0;
        kernelDiscoveryStatus.value = `Found ${results.processes?.length || 0} processes and ${pteCount} page tables (DB: ${dbStats.totalPages} pages indexed)`;
        console.log('Paged kernel discovery complete');
        return; // Exit early since we handled everything
      } catch (e) {
        console.error('Electron chunked read exception:', e);
      }
    } else if (fileHandle.value) {
      // Browser path - read file in chunks and use PagedMemory
      try {
        const file = await fileHandle.value.getFile()
        console.log('Got file from handle:', file.name, 'size:', file.size)

        const chunkSize = 100 * 1024 * 1024; // 100MB chunks
        const totalChunks = Math.ceil(file.size / chunkSize);

        kernelDiscoveryStatus.value = `Reading ${(file.size / (1024*1024*1024)).toFixed(1)} GB file in ${totalChunks} chunks...`

        const chunks: Uint8Array[] = [];

        for (let i = 0; i < totalChunks; i++) {
          const offset = i * chunkSize;
          const size = Math.min(chunkSize, file.size - offset);

          kernelDiscoveryStatus.value = `Reading chunk ${i + 1}/${totalChunks} (${(offset / (1024*1024)).toFixed(0)}-${((offset + size) / (1024*1024)).toFixed(0)} MB)...`

          const blob = file.slice(offset, offset + size);
          const arrayBuffer = await blob.arrayBuffer();
          chunks.push(new Uint8Array(arrayBuffer));

          // Chunk read successfully (commenting out verbose log)

          // Let UI update
          await new Promise(resolve => setTimeout(resolve, 10));
        }

        // Use PagedMemory approach
        console.log(`Using PagedMemory with ${chunks.length} chunks...`);
        kernelDiscoveryStatus.value = `Loading memory pages...`;

        const totalSize = chunks.reduce((sum, chunk) => sum + chunk.length, 0);

        // Import and use PagedMemory
        const { PagedMemory } = await import('./paged-memory');
        const { PagedKernelDiscovery } = await import('./kernel-discovery-paged');

        // Load chunks into PagedMemory
        const memory = await PagedMemory.fromChunks(chunks, (percent) => {
          kernelDiscoveryStatus.value = `Loading memory pages... ${percent.toFixed(1)}%`;
        });

        console.log(`PagedMemory loaded: ${memory.getMemoryUsage()}`);
        kernelDiscoveryStatus.value = `Analyzing memory (${(totalSize / (1024*1024)).toFixed(0)}MB)...`;

        // Run discovery using PagedKernelDiscovery
        const discovery = new PagedKernelDiscovery(memory);
        const results = await discovery.discover(totalSize);

        kernelDiscoveryResults.value = {
          processes: results.processes || [],
          kernelPTEs: results.kernelPtes || [],
          swapperPgDir: results.swapperPgDir || 0,
          statistics: {
            totalChunksProcessed: chunks.length,
            totalProcesses: results.processes?.length || 0,
            totalPTEs: results.stats?.kernelPTEs || results.kernelPtes?.length || 0,
            memoryAnalyzed: totalSize
          }
        };

        // Store the PageCollection if available
        if (results.pageCollection) {
          pageCollection.value = results.pageCollection;
          console.log('PageCollection ready with', results.pageCollection.getStatistics());
        }

        // Also populate the old kernel page database for compatibility
        kernelDiscoveryStatus.value = 'Building page reference database...';
        kernelPageDB.populate(results);
        const dbStats = kernelPageDB.getStatistics();
        console.log('Kernel page database populated:', dbStats);

        kernelDiscoveryRunning.value = false;
        const pteCount = results.stats?.kernelPTEs || results.kernelPtes?.length || 0;
        kernelDiscoveryStatus.value = `Found ${results.processes?.length || 0} processes and ${pteCount} page tables (DB: ${dbStats.totalPages} pages indexed)`;
        console.log('Paged kernel discovery complete');
        return;
      } catch (e) {
        console.error('File handle read failed:', e)
        kernelDiscoveryStatus.value = 'File read failed: ' + e.message
        kernelDiscoveryRunning.value = false;
        return;
      }
    } else if (memoryData.value) {
      // Fall back to current view (limited but better than nothing)
      kernelDiscoveryStatus.value = 'Using current view (limited data - only analyzing visible portion)...'
      fullMemory = memoryData.value
      console.log('Using current view, size:', fullMemory.length)

      // Run basic discovery on limited data
      const discovery = new KernelDiscovery(fullMemory)
      kernelDiscoveryStatus.value = 'Finding processes...'
      await new Promise(resolve => setTimeout(resolve, 100))
      const results = await discovery.discover()

      // Populate the kernel page database
      kernelDiscoveryStatus.value = 'Building page reference database...';
      kernelPageDB.populate(results);
      const dbStats = kernelPageDB.getStatistics();

      kernelDiscoveryStatus.value = 'Analysis complete!'
      kernelDiscoveryResults.value = results
      console.log('Kernel discovery complete:', results)
      console.log('Kernel page database populated:', dbStats)

      // Warn if database is empty due to limited view
      if (dbStats.totalPages === 0) {
        kernelDiscoveryStatus.value = 'Warning: Limited memory view - no page tables accessible. Use full file for complete discovery.'
      }
    }
  } catch (error) {
    console.error('Kernel discovery failed:', error)
    kernelDiscoveryStatus.value = `Error: ${error.message || 'Discovery failed'}`
  } finally {
    kernelDiscoveryRunning.value = false
  }
}

function closeKernelDiscovery() {
  kernelDiscoveryModal.value = false
}

function sortProcesses(key: 'pid' | 'name' | 'ptes') {
  if (processSortKey.value === key) {
    processSortReverse.value = !processSortReverse.value
  } else {
    processSortKey.value = key
    processSortReverse.value = false
  }
}

function viewProcessPTEs(proc: any) {
  console.log('View PTEs for process:', proc)
  // Could expand this to show detailed PTE view
  alert(`Process ${proc.pid} (${proc.comm}) has ${proc.pteCount || 0} PTEs`)
}

function exportJSON() {
  if (!kernelDiscoveryResults.value) return

  const json = JSON.stringify(kernelDiscoveryResults.value, null, 2)
  const blob = new Blob([json], { type: 'application/json' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = `kernel-discovery-${Date.now()}.json`
  a.click()
  URL.revokeObjectURL(url)
}

function exportHTML() {
  if (!kernelDiscoveryResults.value) return

  const html = generateHTMLReport(kernelDiscoveryResults.value)
  const blob = new Blob([html], { type: 'text/html' })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = `kernel-discovery-${Date.now()}.html`
  a.click()
  URL.revokeObjectURL(url)
}

function generateHTMLReport(results: any): string {
  const processes = results.processes || []
  const processRows = processes.map((p: any) => `
    <tr>
      <td>${p.pid}</td>
      <td>${p.comm || 'unknown'}</td>
      <td>${p.pteCount || 0}</td>
      <td>${p.sections?.length || 0}</td>
    </tr>
  `).join('')

  return `<!DOCTYPE html>
<html>
<head>
  <title>Kernel Memory Discovery Report</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #1e1e1e; color: #e0e0e0; }
    h1 { color: #4ec9b0; }
    h2 { color: #0e639c; margin-top: 30px; }
    .stats { display: flex; gap: 20px; margin: 20px 0; }
    .stat { background: #2d2d30; padding: 15px; border-radius: 5px; }
    .stat-value { font-size: 24px; font-weight: bold; color: #4ec9b0; }
    .stat-label { color: #999; margin-top: 5px; }
    table { width: 100%; border-collapse: collapse; background: #2d2d30; }
    th { background: #0e639c; padding: 10px; text-align: left; }
    td { padding: 8px; border-bottom: 1px solid #3e3e42; }
    tr:hover { background: #3c3c3c; }
  </style>
</head>
<body>
  <h1>🔍 Kernel Memory Discovery Report</h1>
  <p>Generated: ${new Date().toLocaleString()}</p>

  <div class="stats">
    <div class="stat">
      <div class="stat-value">${processes.length}</div>
      <div class="stat-label">Processes Found</div>
    </div>
    <div class="stat">
      <div class="stat-value">${results.kernelPTEs?.length || 0}</div>
      <div class="stat-label">Kernel PTEs</div>
    </div>
    <div class="stat">
      <div class="stat-value">0x${(results.swapperPgDir || 0).toString(16).toUpperCase()}</div>
      <div class="stat-label">swapper_pg_dir</div>
    </div>
  </div>

  <h2>Process List</h2>
  <table>
    <thead>
      <tr>
        <th>PID</th>
        <th>Name</th>
        <th>PTEs</th>
        <th>Sections</th>
      </tr>
    </thead>
    <tbody>
      ${processRows}
    </tbody>
  </table>

  <h2>Statistics</h2>
  <pre>${JSON.stringify(results.statistics || {}, null, 2)}</pre>
</body>
</html>`
}

function copyToClipboard() {
  if (!kernelDiscoveryResults.value) return

  const summary = `Kernel Discovery Summary
========================
Processes Found: ${kernelDiscoveryResults.value.processes?.length || 0}
Kernel PTEs: ${kernelDiscoveryResults.value.kernelPTEs?.length || 0}
swapper_pg_dir: 0x${(kernelDiscoveryResults.value.swapperPgDir || 0).toString(16).toUpperCase()}

Top Processes:
${(kernelDiscoveryResults.value.processes || []).slice(0, 10).map((p: any) =>
  `  PID ${p.pid}: ${p.comm} (${p.pteCount || 0} PTEs)`
).join('\n')}
`

  navigator.clipboard.writeText(summary).then(() => {
    console.log('Summary copied to clipboard')
  })
}

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

  // Expose functions to window for Electron/testing
  (window as any).runKernelDiscovery = runKernelDiscovery;
  (window as any).startDiscovery = startDiscovery;
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
  fileSize.value = size
  fileName.value = path.split('/').pop() || 'memory.bin'
  isFileOpen.value = true
  currentOffset.value = 0

  // Load initial data
  await refreshMemory()

  // Start change detection if enabled
  if (changeDetectionEnabled.value) {
    // For Electron, start scanning the full file
    if (isElectron.value && window.electronAPI) {
      startFullFileScan()
    }
  }
}

let lastDataRefreshTime = null
let dataRefreshCount = 0

async function handleElectronFileRefreshed({ path, size }) {
  const now = Date.now()
  if (lastDataRefreshTime) {
    const delta = now - lastDataRefreshTime
    dataRefreshCount++
  }
  lastDataRefreshTime = now

  fileSize.value = size

  // Reload current view
  await refreshMemory()
}

function handleElectronFileClosed() {
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
  width: 180px;
  font-family: 'Courier New', monospace;
  padding: 4px 8px;
  font-size: 13px;
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

.status-bar .db-indicator {
  color: #4CAF50;
  font-weight: bold;
  margin-left: 8px;
  background: rgba(76, 175, 80, 0.2);
  padding: 2px 6px;
  border-radius: 3px;
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

/* Modal Overlay */
.modal-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(0, 0, 0, 0.7);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 10001;
}

.modal-content {
  background: #2d2d30;
  border-radius: 8px;
  box-shadow: 0 4px 20px rgba(0, 0, 0, 0.5);
  max-width: 90%;
  max-height: 90vh;
  overflow: hidden;
  display: flex;
  flex-direction: column;
}

.kernel-discovery-modal {
  width: 900px;
}

.modal-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 20px;
  background: #0e639c;
  color: white;
}

.modal-header h2 {
  margin: 0;
  font-size: 20px;
}

.modal-close {
  background: transparent;
  border: none;
  color: white;
  font-size: 24px;
  cursor: pointer;
  padding: 0;
  width: 30px;
  height: 30px;
  display: flex;
  align-items: center;
  justify-content: center;
}

.modal-close:hover {
  background: rgba(255, 255, 255, 0.1);
  border-radius: 4px;
}

/* Discovery Progress */
.discovery-progress {
  padding: 60px;
  text-align: center;
}

.spinner {
  width: 50px;
  height: 50px;
  border: 3px solid #3c3c3c;
  border-top: 3px solid #0e639c;
  border-radius: 50%;
  margin: 0 auto 20px;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  0% { transform: rotate(0deg); }
  100% { transform: rotate(360deg); }
}

/* Discovery Results */
.discovery-results {
  padding: 20px;
  overflow-y: auto;
  flex: 1;
}

.stats-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 15px;
  margin-bottom: 30px;
}

.stat-card {
  background: #3c3c3c;
  padding: 15px;
  border-radius: 5px;
  border: 1px solid #555;
}

.stat-value {
  font-size: 24px;
  font-weight: bold;
  color: #4ec9b0;
  margin-bottom: 5px;
  font-family: 'Courier New', monospace;
}

.stat-label {
  color: #999;
  font-size: 12px;
  text-transform: uppercase;
}

.section {
  margin-bottom: 30px;
}

.section h3 {
  color: #4ec9b0;
  margin-bottom: 15px;
  font-size: 16px;
}

.process-table-container {
  max-height: 400px;
  overflow-y: auto;
  background: #3c3c3c;
  border-radius: 5px;
}

.process-table {
  width: 100%;
  border-collapse: collapse;
}

.process-table th {
  background: #0e639c;
  color: white;
  padding: 10px;
  text-align: left;
  position: sticky;
  top: 0;
  cursor: pointer;
  user-select: none;
}

.process-table th:hover {
  background: #1177bb;
}

.process-table td {
  padding: 8px 10px;
  border-bottom: 1px solid #555;
  color: #e0e0e0;
}

.process-table tr:hover {
  background: #4a4a4a;
}

.small-button {
  padding: 4px 8px;
  font-size: 12px;
  background: #0e639c;
  color: white;
  border: none;
  border-radius: 3px;
  cursor: pointer;
}

.small-button:hover {
  background: #1177bb;
}

.export-buttons {
  display: flex;
  gap: 10px;
  margin-top: 20px;
  padding-top: 20px;
  border-top: 1px solid #555;
}

.no-results {
  padding: 60px;
  text-align: center;
  color: #999;
}

.primary-button {
  background: #0e639c;
  color: white;
  border: none;
  padding: 10px 20px;
  border-radius: 4px;
  font-size: 16px;
  cursor: pointer;
  margin-top: 20px;
}

.primary-button:hover {
  background: #1177bb;
}
</style>