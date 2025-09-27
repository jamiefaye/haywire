// Singleton for WASM module to prevent multiple loads
import { ref } from 'vue'
import { PixelFormat } from './useWasmRenderer'

interface MemoryRendererModule {
  _renderMemoryToCanvas(
    memoryData: number,  // Pointer to memory data
    memorySize: number,
    canvasBuffer: number,  // Pointer to canvas pixel buffer
    canvasWidth: number,
    canvasHeight: number,
    sourceOffset: number,
    displayWidth: number,
    displayHeight: number,
    stride: number,
    format: number,
    splitComponents: boolean,
    columnMode: boolean,
    columnWidth: number,
    columnGap: number
  ): void
  _getFormatBytesPerPixel(format: number): number
  _getExtendedFormat(format: number, splitComponents: boolean): number
  _getFormatDescriptor(
    extendedFormat: number,
    bytesPerElement: number,
    pixelsPerElementX: number,
    pixelsPerElementY: number
  ): void
  _pixelToMemoryCoordinate(
    pixelX: number, pixelY: number,
    displayWidth: number, displayHeight: number,
    stride: number,
    format: number,
    splitComponents: boolean,
    columnMode: boolean,
    columnWidth: number,
    columnGap: number,
    memoryX: number, memoryY: number
  ): void
  _allocateMemory(size: number): number
  _allocatePixelBuffer(pixelCount: number): number
  _freeMemory(ptr: number): void

  HEAPU8: Uint8Array
  HEAPU32: Uint32Array
  ccall: Function
  cwrap: Function
}

// Shared state
export const wasmModule = ref<MemoryRendererModule | null>(null)
export const wasmIsLoading = ref(true)
export const wasmError = ref<string | null>(null)

// Shared memory pointers (one set for all components)
export const sharedMemoryState = {
  memoryPtr: 0,
  canvasPtr: 0,
  allocatedMemorySize: 0,
  allocatedCanvasSize: 0
}

// Load function that only runs once
let loadPromise: Promise<void> | null = null

export async function loadWasmModule() {
  // If already loading, return the existing promise
  if (loadPromise) {
    return loadPromise
  }

  // If already loaded, return immediately
  if (wasmModule.value) {
    return
  }

  loadPromise = (async () => {
    try {
      wasmIsLoading.value = true

      // Check if script is already loaded
      if (typeof (window as any).MemoryRendererModule === 'undefined') {
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
      wasmModule.value = await (window as any).MemoryRendererModule() as MemoryRendererModule
      wasmError.value = null
      wasmIsLoading.value = false
    } catch (err) {
      wasmError.value = `WASM module not found. Run "npm run build-wasm" to build it.`
      console.error('Failed to load WASM module:', err)
      wasmIsLoading.value = false
      throw err
    }
  })()

  return loadPromise
}

// Cleanup function
export function cleanupWasmModule() {
  if (wasmModule.value) {
    if (sharedMemoryState.memoryPtr) {
      wasmModule.value._freeMemory(sharedMemoryState.memoryPtr)
      sharedMemoryState.memoryPtr = 0
      sharedMemoryState.allocatedMemorySize = 0
    }
    if (sharedMemoryState.canvasPtr) {
      wasmModule.value._freeMemory(sharedMemoryState.canvasPtr)
      sharedMemoryState.canvasPtr = 0
      sharedMemoryState.allocatedCanvasSize = 0
    }
  }
}

// Auto-load on first import
loadWasmModule().catch(console.error)