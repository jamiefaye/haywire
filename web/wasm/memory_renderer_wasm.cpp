// WebAssembly wrapper for memory_renderer.cpp
// IMPORTANT: This wrapper only exposes the existing renderer functions
// DO NOT reimplement any rendering logic here - it will break!

#include "memory_renderer.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <emscripten/emscripten.h>

using namespace Haywire;

extern "C" {

// Main rendering function using the static RenderMemory method
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
    // Create RenderConfig
    RenderConfig config;
    config.displayWidth = displayWidth;
    config.displayHeight = displayHeight;
    config.stride = stride;

    // Set PixelFormat with the type
    config.format.type = static_cast<PixelFormat::Type>(format);

    config.splitComponents = splitComponents;
    config.columnMode = columnMode;
    config.columnWidth = columnWidth;
    config.columnGap = columnGap;

    // Adjust data pointer for source offset
    const uint8_t* offsetData = memoryData + sourceOffset;
    size_t adjustedSize = memorySize - sourceOffset;

    // Call the static rendering method
    std::vector<uint32_t> rendered = MemoryRenderer::RenderMemory(
        offsetData,
        adjustedSize,
        config
    );

    // Copy rendered data to canvas buffer
    size_t pixelsToCopy = std::min(
        rendered.size(),
        static_cast<size_t>(canvasWidth * canvasHeight)
    );

    if (pixelsToCopy > 0) {
        memcpy(canvasBuffer, rendered.data(), pixelsToCopy * sizeof(uint32_t));
    }
}

// Get format information
EMSCRIPTEN_KEEPALIVE
int getFormatBytesPerPixel(int format) {
    // Create a PixelFormat with the Type enum value
    PixelFormat pixelFormat;
    pixelFormat.type = static_cast<PixelFormat::Type>(format);
    return RenderConfig::GetBytesPerPixel(pixelFormat);
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
    *bytesPerElement = desc.bytesIn;
    *pixelsPerElementX = desc.pixelsOutX;
    *pixelsPerElementY = desc.pixelsOutY;
}

// Simple pixel to memory coordinate conversion
// Note: The actual implementation may not have this method,
// so we provide a basic implementation
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
    // Basic coordinate mapping
    if (columnMode) {
        // Column mode calculation
        int totalColumnWidth = columnWidth + columnGap;
        int columnIndex = pixelX / totalColumnWidth;
        int xInColumn = pixelX % totalColumnWidth;

        if (xInColumn >= columnWidth) {
            // Click in gap
            *memoryX = -1;
            *memoryY = -1;
            return;
        }

        *memoryX = xInColumn;
        *memoryY = columnIndex * displayHeight + pixelY;
    } else {
        // Simple linear mode
        *memoryX = pixelX;
        *memoryY = pixelY;
    }
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