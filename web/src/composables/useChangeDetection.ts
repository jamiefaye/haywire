import { ref, shallowRef } from 'vue'

// Types for change detection
export interface ChunkInfo {
  offset: number
  size: number
  checksum: number
  isZero: boolean
  hasChanged: boolean
}

export interface ChangeDetectionState {
  chunks: ChunkInfo[]
  pageSize: number
  chunkSize: number
  totalSize: number
  lastCheckTime: number
}

// WASM module instance
let wasmModule: any = null
let wasmInstance: any = null

// Shared buffer for operations
let sharedBuffer: number = 0
let sharedBufferSize: number = 0

export function useChangeDetection() {
  const isLoading = ref(false)
  const error = ref<string | null>(null)
  const state = shallowRef<ChangeDetectionState | null>(null)

  // Load WASM module
  async function loadModule() {
    if (wasmModule) return

    try {
      isLoading.value = true
      error.value = null

      // Check if script is already loaded
      // @ts-ignore
      if (typeof window.MemoryRendererModule === 'undefined') {
        // Load the script
        await new Promise<void>((resolve, reject) => {
          const script = document.createElement('script')
          script.src = '/wasm/memory_renderer.js'
          script.onload = () => resolve()
          script.onerror = () => reject(new Error('Failed to load WASM script'))
          document.head.appendChild(script)
        })
      }

      // Initialize the module
      // @ts-ignore
      wasmInstance = await window.MemoryRendererModule()
      wasmModule = wasmInstance

      // Ensure the module is fully initialized
      if (!wasmModule._allocateMemory) {
        throw new Error('WASM module functions not available')
      }

      // Allocate shared buffer (1MB for processing chunks)
      sharedBufferSize = 1048576
      sharedBuffer = wasmModule._allocateMemory(sharedBufferSize)
    } catch (err) {
      error.value = `Failed to load change detection module: ${err}`
      console.error('Change detection load error:', err)
    } finally {
      isLoading.value = false
    }
  }

  // Test if a chunk is all zeros
  function testChunkZero(data: Uint8Array, offset: number, size: number): boolean {
    if (!wasmModule) throw new Error('WASM module not loaded')
    if (size > sharedBufferSize) throw new Error('Chunk too large for buffer')

    // Copy data to WASM heap
    wasmModule.HEAPU8.set(data.subarray(offset, offset + size), sharedBuffer)

    // Call appropriate function based on size
    if (size === 4096) {
      return wasmModule._testPageZero(sharedBuffer)
    } else if (size === 65536) {
      return wasmModule._testChunk64KZero(sharedBuffer)
    } else if (size === 1048576) {
      return wasmModule._testChunk1MBZero(sharedBuffer)
    } else {
      return wasmModule._testChunkZeroSIMD(sharedBuffer, size)
    }
  }

  // Calculate checksum for a chunk
  function calculateChecksum(data: Uint8Array, offset: number, size: number): number {
    if (!wasmModule) throw new Error('WASM module not loaded')
    if (size > sharedBufferSize) throw new Error('Chunk too large for buffer')

    // Copy data to WASM heap
    wasmModule.HEAPU8.set(data.subarray(offset, offset + size), sharedBuffer)

    // Call appropriate function based on size
    if (size === 4096) {
      return wasmModule._calculatePageChecksum(sharedBuffer)
    } else if (size === 65536) {
      return wasmModule._calculateChunk64KChecksum(sharedBuffer)
    } else if (size === 1048576) {
      return wasmModule._calculateChunk1MBChecksum(sharedBuffer)
    } else {
      return wasmModule._calculateChunkChecksumSIMD(sharedBuffer, size)
    }
  }

  // Scan memory and detect changes
  function scanMemory(
    data: Uint8Array,
    chunkSize: number = 65536,
    previousState?: ChangeDetectionState
  ): ChangeDetectionState {
    if (!wasmModule) throw new Error('WASM module not loaded')

    const chunks: ChunkInfo[] = []
    const totalSize = data.length
    const numChunks = Math.ceil(totalSize / chunkSize)

    // Process each chunk
    for (let i = 0; i < numChunks; i++) {
      const offset = i * chunkSize
      const size = Math.min(chunkSize, totalSize - offset)

      // Test for zero
      const isZero = testChunkZero(data, offset, size)

      // Calculate checksum (skip for zero chunks for performance)
      const checksum = isZero ? 0 : calculateChecksum(data, offset, size)

      // Check if changed from previous state
      let hasChanged = false
      if (previousState && previousState.chunks[i]) {
        const prevChunk = previousState.chunks[i]
        hasChanged = (prevChunk.checksum !== checksum) || (prevChunk.isZero !== isZero)
      }

      chunks.push({
        offset,
        size,
        checksum,
        isZero,
        hasChanged
      })
    }

    return {
      chunks,
      pageSize: 4096,
      chunkSize,
      totalSize,
      lastCheckTime: Date.now()
    }
  }

  // Get chunk at memory offset
  function getChunkAtOffset(state: ChangeDetectionState, offset: number): ChunkInfo | null {
    if (!state) return null
    const chunkIndex = Math.floor(offset / state.chunkSize)
    return state.chunks[chunkIndex] || null
  }

  // Get visualization data for overview pane
  function getVisualizationData(state: ChangeDetectionState) {
    if (!state) return null

    // Calculate dimensions for visualization
    const numChunks = state.chunks.length
    const cols = 16 // 16 chunks per row
    const rows = Math.ceil(numChunks / cols)

    // Create pixel data (one pixel per chunk)
    const pixels = new Uint8Array(numChunks * 4) // RGBA

    state.chunks.forEach((chunk, i) => {
      const pixelOffset = i * 4

      if (chunk.isZero) {
        // Dark gray for zero chunks
        pixels[pixelOffset] = 32
        pixels[pixelOffset + 1] = 32
        pixels[pixelOffset + 2] = 32
        pixels[pixelOffset + 3] = 255
      } else if (chunk.hasChanged) {
        // Bright green for changed chunks
        pixels[pixelOffset] = 0
        pixels[pixelOffset + 1] = 255
        pixels[pixelOffset + 2] = 0
        pixels[pixelOffset + 3] = 255
      } else {
        // Blue gradient based on checksum (for visual variety)
        const intensity = (chunk.checksum % 128) + 128
        pixels[pixelOffset] = 0
        pixels[pixelOffset + 1] = intensity / 2
        pixels[pixelOffset + 2] = intensity
        pixels[pixelOffset + 3] = 255
      }
    })

    return {
      width: cols,
      height: rows,
      pixels
    }
  }

  // Cleanup
  function cleanup() {
    if (wasmModule && sharedBuffer) {
      wasmModule._freeMemory(sharedBuffer)
      sharedBuffer = 0
    }
  }

  return {
    isLoading,
    error,
    state,
    loadModule,
    scanMemory,
    getChunkAtOffset,
    getVisualizationData,
    cleanup
  }
}