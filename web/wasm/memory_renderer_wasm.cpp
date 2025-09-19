// WebAssembly wrapper for memory_renderer.cpp
// IMPORTANT: This wrapper only exposes the existing renderer functions
// DO NOT reimplement any rendering logic here - it will break!

#include "memory_renderer.h"
#include <cstdint>
#include <cstring>
#include <emscripten/emscripten.h>

using namespace Haywire;

// Static renderer instance to avoid recreation
static MemoryRenderer* g_renderer = nullptr;

extern "C" {

// Initialize the global renderer
EMSCRIPTEN_KEEPALIVE
void initRenderer() {
    if (!g_renderer) {
        g_renderer = new MemoryRenderer();
    }
}

// Clean up the global renderer
EMSCRIPTEN_KEEPALIVE
void cleanupRenderer() {
    if (g_renderer) {
        delete g_renderer;
        g_renderer = nullptr;
    }
}

// Direct wrapper around RenderMemoryToCanvas
// This is the main entry point - it handles all the complex rendering logic
EMSCRIPTEN_KEEPALIVE
void renderMemoryToCanvas(
    const uint8_t* memoryData,
    size_t memorySize,
    uint32_t* canvasBuffer,
    int canvasWidth,
    int canvasHeight,
    size_t sourceOffset,
    int displayWidth,
    int displayHeight,
    int stride,
    int format,           // PixelFormat::Type
    bool splitComponents,
    bool columnMode,
    int columnWidth,
    int columnGap
) {
    if (!g_renderer) {
        initRenderer();
    }

    // The original function handles all the complexity
    g_renderer->RenderMemoryToCanvas(
        memoryData,
        memorySize,
        canvasBuffer,
        canvasWidth,
        canvasHeight,
        sourceOffset,
        displayWidth,
        displayHeight,
        stride,
        static_cast<PixelFormat::Type>(format),
        splitComponents,
        columnMode,
        columnWidth,
        columnGap
    );
}

// Get format information without modifying any logic
EMSCRIPTEN_KEEPALIVE
int getFormatBytesPerPixel(int format) {
    return PixelFormat::GetBytesPerPixel(static_cast<PixelFormat::Type>(format));
}

// Get extended format for split components
EMSCRIPTEN_KEEPALIVE
int getExtendedFormat(int format, bool splitComponents) {
    return static_cast<int>(
        MemoryRenderer::GetExtendedFormat(
            static_cast<PixelFormat::Type>(format),
            splitComponents
        )
    );
}

// Get format descriptor details
EMSCRIPTEN_KEEPALIVE
void getFormatDescriptor(
    int extendedFormat,
    int* bytesPerElement,
    int* pixelsPerElementX,
    int* pixelsPerElementY
) {
    FormatDescriptor desc = MemoryRenderer::GetFormatDescriptor(
        static_cast<ExtendedFormat>(extendedFormat)
    );
    *bytesPerElement = desc.bytesPerElement;
    *pixelsPerElementX = desc.pixelsPerElementX;
    *pixelsPerElementY = desc.pixelsPerElementY;
}

// Memory coordinate conversion for column mode
EMSCRIPTEN_KEEPALIVE
void pixelToMemoryCoordinate(
    int pixelX, int pixelY,
    int displayWidth, int displayHeight,
    int stride,
    int format,
    bool splitComponents,
    bool columnMode,
    int columnWidth,
    int columnGap,
    int* memoryX, int* memoryY
) {
    if (!g_renderer) {
        initRenderer();
    }

    g_renderer->PixelToMemoryCoordinate(
        pixelX, pixelY,
        displayWidth, displayHeight,
        stride,
        static_cast<PixelFormat::Type>(format),
        splitComponents,
        columnMode,
        columnWidth,
        columnGap,
        memoryX, memoryY
    );
}

// Memory allocation helpers for JavaScript
EMSCRIPTEN_KEEPALIVE
uint8_t* allocateMemory(size_t size) {
    return (uint8_t*)malloc(size);
}

EMSCRIPTEN_KEEPALIVE
uint32_t* allocatePixelBuffer(size_t pixelCount) {
    return (uint32_t*)malloc(pixelCount * sizeof(uint32_t));
}

EMSCRIPTEN_KEEPALIVE
void freeMemory(void* ptr) {
    free(ptr);
}

} // extern "C"