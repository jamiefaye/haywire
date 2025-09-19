// Vue composable for WebAssembly memory renderer
import { ref, onUnmounted } from 'vue'

// Pixel format enum matching C++ PixelFormat::Type from common.h
export enum PixelFormat {
  RGB888 = 0,     // R G B order
  RGBA8888 = 1,   // R G B A order
  BGR888 = 2,     // B G R order (Windows BMP)
  BGRA8888 = 3,   // B G R A order (Windows native)
  ARGB8888 = 4,   // A R G B order (Mac native)
  ABGR8888 = 5,   // A B G R order
  RGB565 = 6,
  GRAYSCALE = 7,
  BINARY = 8,
  HEX_PIXEL = 9,  // 32-bit value as 8 hex digits
  CHAR_8BIT = 10, // 8-bit byte as character
  CUSTOM = 11
}

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

export function useWasmRenderer() {
  const module = ref<MemoryRendererModule | null>(null)
  const isLoading = ref(true)
  const error = ref<string | null>(null)

  // Pointers for allocated memory
  let memoryPtr: number = 0
  let canvasPtr: number = 0
  let allocatedMemorySize = 0
  let allocatedCanvasSize = 0

  // Load the WASM module
  async function loadModule() {
    try {
      console.log('Starting WASM module load...')
      // Load the WASM module from public directory using script injection
      // This will be created by running: npm run build-wasm
      await loadWasmScript()
      console.log('Script loaded, checking for MemoryRendererModule...')

      // The script should have created a global MemoryRendererModule
      if (typeof (window as any).MemoryRendererModule === 'undefined') {
        throw new Error('MemoryRendererModule not found on window')
      }

      console.log('Found MemoryRendererModule, initializing...')
      module.value = await (window as any).MemoryRendererModule() as MemoryRendererModule
      console.log('WASM module initialized successfully:', module.value)

      // No need to initialize - removed from wrapper

      isLoading.value = false
    } catch (err) {
      error.value = `WASM module not found. Run "npm run build-wasm" to build it.`
      console.error('Failed to load WASM module:', err)
      isLoading.value = false
    }
  }

  // Helper to load WASM script dynamically
  function loadWasmScript(): Promise<void> {
    return new Promise((resolve, reject) => {
      // Check if already loaded
      if ((window as any).MemoryRendererModule) {
        console.log('MemoryRendererModule already loaded')
        resolve()
        return
      }

      const script = document.createElement('script')
      script.src = '/wasm/memory_renderer.js'
      console.log('Loading WASM script from:', script.src)
      script.onload = () => {
        console.log('WASM script loaded successfully')
        resolve()
      }
      script.onerror = (err) => {
        console.error('Failed to load WASM script:', err)
        reject(new Error('Failed to load WASM script'))
      }
      document.head.appendChild(script)
    })
  }

  // Render memory to canvas
  function renderMemory(
    memoryData: Uint8Array,
    canvas: HTMLCanvasElement,
    params: {
      sourceOffset?: number
      displayWidth?: number
      displayHeight?: number
      stride?: number
      format?: PixelFormat
      splitComponents?: boolean
      columnMode?: boolean
      columnWidth?: number
      columnGap?: number
    } = {}
  ) {
    if (!module.value) {
      console.warn('WASM module not loaded yet')
      return
    }

    if (!canvas) {
      console.warn('Canvas element not provided')
      return
    }

    const ctx = canvas.getContext('2d')
    if (!ctx) {
      console.error('Could not get canvas context')
      return
    }

    // Check if this is the placeholder module
    if (!module.value.HEAPU8 || module.value.HEAPU8.length === 0) {
      // Draw a placeholder pattern for testing
      const imageData = ctx.createImageData(canvas.width, canvas.height)
      const data = imageData.data

      // Create a simple gradient pattern
      for (let y = 0; y < canvas.height; y++) {
        for (let x = 0; x < canvas.width; x++) {
          const idx = (y * canvas.width + x) * 4
          // Use first bytes of memory data if available
          if (memoryData && idx / 4 < memoryData.length) {
            const memIdx = idx / 4
            data[idx] = memoryData[memIdx] || 0     // R
            data[idx + 1] = memoryData[memIdx + 1] || 0 // G
            data[idx + 2] = memoryData[memIdx + 2] || 0 // B
            data[idx + 3] = 255                     // A
          } else {
            // Placeholder gradient
            data[idx] = (x / canvas.width) * 255     // R
            data[idx + 1] = (y / canvas.height) * 255 // G
            data[idx + 2] = 128                      // B
            data[idx + 3] = 255                      // A
          }
        }
      }

      ctx.putImageData(imageData, 0, 0)
      return
    }

    const canvasWidth = canvas.width
    const canvasHeight = canvas.height

    // Set default parameters
    const {
      sourceOffset = 0,
      displayWidth = canvasWidth,
      displayHeight = canvasHeight,
      stride = displayWidth,
      format = PixelFormat.RGB888,
      splitComponents = false,
      columnMode = false,
      columnWidth = displayWidth,
      columnGap = 0
    } = params

    // Ensure memory is allocated with correct size
    if (memoryData.length > allocatedMemorySize) {
      if (memoryPtr) {
        module.value._freeMemory(memoryPtr)
      }
      memoryPtr = module.value._allocateMemory(memoryData.length)
      allocatedMemorySize = memoryData.length
    }

    // Ensure canvas buffer is allocated
    const requiredCanvasSize = canvasWidth * canvasHeight
    if (requiredCanvasSize > allocatedCanvasSize) {
      if (canvasPtr) {
        module.value._freeMemory(canvasPtr)
      }
      canvasPtr = module.value._allocatePixelBuffer(requiredCanvasSize)
      allocatedCanvasSize = requiredCanvasSize
    }

    // Copy memory data to WASM heap
    module.value.HEAPU8.set(memoryData, memoryPtr)

    // Call the renderer
    module.value._renderMemoryToCanvas(
      memoryPtr,
      memoryData.length,
      canvasPtr,
      canvasWidth,
      canvasHeight,
      sourceOffset,
      displayWidth,
      displayHeight,
      stride,
      format,
      splitComponents,
      columnMode,
      columnWidth,
      columnGap
    )

    // Copy rendered pixels back to canvas
    const imageData = ctx.createImageData(canvasWidth, canvasHeight)
    const pixels = new Uint32Array(
      module.value.HEAPU32.buffer,
      canvasPtr,
      canvasWidth * canvasHeight
    )

    // Convert RGBA32 to ImageData format
    const data32 = new Uint32Array(imageData.data.buffer)
    data32.set(pixels)

    ctx.putImageData(imageData, 0, 0)
  }

  // Convert pixel coordinates to memory coordinates
  function pixelToMemoryCoordinate(
    pixelX: number,
    pixelY: number,
    displayWidth: number,
    displayHeight: number,
    stride: number,
    format: PixelFormat,
    splitComponents: boolean,
    columnMode: boolean,
    columnWidth: number,
    columnGap: number
  ): { x: number, y: number } {
    if (!module.value) {
      // Return default values if module not loaded
      return { x: 0, y: 0 }
    }

    // For the placeholder module, just return simple calculation
    if (!module.value.HEAPU8.buffer || module.value.HEAPU8.length === 0) {
      // Simple coordinate mapping for placeholder
      return { x: pixelX, y: pixelY }
    }

    // Allocate space for output values
    const memXPtr = module.value._allocateMemory(4)
    const memYPtr = module.value._allocateMemory(4)

    module.value._pixelToMemoryCoordinate(
      pixelX, pixelY,
      displayWidth, displayHeight,
      stride,
      format,
      splitComponents,
      columnMode,
      columnWidth,
      columnGap,
      memXPtr, memYPtr
    )

    // Read the results
    const memX = new Int32Array(module.value.HEAPU8.buffer, memXPtr, 1)[0]
    const memY = new Int32Array(module.value.HEAPU8.buffer, memYPtr, 1)[0]

    // Free temporary allocations
    module.value._freeMemory(memXPtr)
    module.value._freeMemory(memYPtr)

    return { x: memX, y: memY }
  }

  // Get bytes per pixel for a format
  function getBytesPerPixel(format: PixelFormat): number {
    if (!module.value) {
      throw new Error('WASM module not loaded')
    }
    return module.value._getFormatBytesPerPixel(format)
  }

  // Cleanup on unmount
  onUnmounted(() => {
    if (module.value) {
      if (memoryPtr) {
        module.value._freeMemory(memoryPtr)
      }
      if (canvasPtr) {
        module.value._freeMemory(canvasPtr)
      }
    }
  })

  // Load module on creation
  loadModule()

  return {
    isLoading,
    error,
    renderMemory,
    pixelToMemoryCoordinate,
    getBytesPerPixel
  }
}