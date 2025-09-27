// Vue composable for WebAssembly memory renderer
import { wasmModule, wasmIsLoading, wasmError, sharedMemoryState } from './wasmSingleton'

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
  CUSTOM = 11,
  // Split component formats for formats with alpha
  RGBA8888_SPLIT = 12,  // R G B A displayed as separate components
  BGRA8888_SPLIT = 13,  // B G R A displayed as separate components
  ARGB8888_SPLIT = 14,  // A R G B displayed as separate components
  ABGR8888_SPLIT = 15   // A B G R displayed as separate components
}

export function useWasmRenderer() {
  // Use singleton module and state
  const module = wasmModule
  const isLoading = wasmIsLoading
  const error = wasmError

  // Use shared memory pointers
  const memoryPtr = () => sharedMemoryState.memoryPtr
  const canvasPtr = () => sharedMemoryState.canvasPtr

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
    try {
      if (!canvas) {
        console.warn('Canvas element not provided')
        return
      }

      const ctx = canvas.getContext('2d')
      if (!ctx) {
        console.error('Could not get canvas context')
        return
      }

      // If module isn't loaded or has issues, show placeholder
      if (!module.value || !module.value.HEAPU8) {
        // Don't set error here - module might still be loading
        // Draw a simple placeholder pattern
        const imageData = ctx.createImageData(canvas.width, canvas.height)
        const data = imageData.data
        for (let i = 0; i < data.length; i += 4) {
          data[i] = 64     // R
          data[i + 1] = 64 // G
          data[i + 2] = 64 // B
          data[i + 3] = 255 // A
        }
        ctx.putImageData(imageData, 0, 0)
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

    // Validate memory data
    if (!memoryData || memoryData.length === 0) {
      console.warn('No memory data to render')
      // Clear canvas
      ctx.clearRect(0, 0, canvasWidth, canvasHeight)
      return
    }

    // Only reallocate if size changed
    if (memoryData.length > sharedMemoryState.allocatedMemorySize || sharedMemoryState.allocatedMemorySize === 0) {
      if (sharedMemoryState.memoryPtr) {
        module.value._freeMemory(sharedMemoryState.memoryPtr)
        sharedMemoryState.memoryPtr = 0
      }

      // Allocate a bit extra to avoid frequent reallocations
      const allocSize = Math.max(memoryData.length, 1024 * 1024) // At least 1MB
      sharedMemoryState.memoryPtr = module.value._allocateMemory(allocSize)
      sharedMemoryState.allocatedMemorySize = allocSize

      if (!sharedMemoryState.memoryPtr) {
        console.error('Failed to allocate memory for data')
        return
      }
    }

    // Only reallocate canvas buffer if size changed
    const requiredCanvasSize = canvasWidth * canvasHeight
    if (requiredCanvasSize > sharedMemoryState.allocatedCanvasSize || sharedMemoryState.allocatedCanvasSize === 0) {
      if (sharedMemoryState.canvasPtr) {
        module.value._freeMemory(sharedMemoryState.canvasPtr)
        sharedMemoryState.canvasPtr = 0
      }

      // Allocate exact size for canvas
      sharedMemoryState.canvasPtr = module.value._allocatePixelBuffer(requiredCanvasSize)
      sharedMemoryState.allocatedCanvasSize = requiredCanvasSize

      if (!sharedMemoryState.canvasPtr) {
        console.error('Failed to allocate canvas buffer')
        return
      }
    }

    // Copy memory data to WASM heap
    if (memoryData.length > 0) {
      module.value.HEAPU8.set(memoryData, sharedMemoryState.memoryPtr)
    }

    // Call the renderer
    module.value._renderMemoryToCanvas(
      sharedMemoryState.memoryPtr,
      memoryData.length,
      sharedMemoryState.canvasPtr,
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
      sharedMemoryState.canvasPtr,
      canvasWidth * canvasHeight
    )

    // Convert RGBA32 to ImageData format
    const data32 = new Uint32Array(imageData.data.buffer)
    data32.set(pixels)

    ctx.putImageData(imageData, 0, 0)
    } catch (err) {
      console.warn('Error rendering memory:', err)
      // Don't set error.value here - this is a rendering issue, not a module loading issue
      // Just clear the canvas
      const ctx = canvas.getContext('2d')
      if (ctx) {
        ctx.clearRect(0, 0, canvas.width, canvas.height)
      }
    }
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

    // Allocate space for output values (use temporary allocations)
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

  // No cleanup needed - shared memory is managed by singleton
  // Load is handled by singleton on first import

  return {
    isLoading,
    error,
    renderMemory,
    pixelToMemoryCoordinate,
    getBytesPerPixel
  }
}