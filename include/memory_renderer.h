#pragma once

#include "common.h"
#include <vector>
#include <cstdint>

namespace Haywire {

// Unified memory rendering configuration
struct RenderConfig {
    // Display dimensions (in pixels)
    int displayWidth;
    int displayHeight;
    
    // Memory layout
    int stride;           // Bytes per row in memory
    int width;            // Logical width (elements, not pixels)
    int height;           // Logical height (elements, not pixels)
    
    // Format
    PixelFormat format;
    bool splitComponents;  // Split RGB/RGBA into separate channels
    
    // Column mode settings
    bool columnMode = false;
    int columnWidth = 256;    // Width of each column in pixels (same units as main width)
    int columnGap = 8;        // Gap between columns in pixels
    
    // Calculate memory offset for a display position in column mode
    size_t ColumnDisplayToMemory(int x, int y, int pixelWidth) const {
        if (!columnMode) return size_t(-1);
        
        // columnWidth is now in pixels, like the main width
        int bytesPerPixel = GetBytesPerPixel(format);
        int totalColumnWidth = columnWidth + columnGap;
        
        // Which column are we in?
        int col = x / totalColumnWidth;
        int xInColumn = x % totalColumnWidth;
        
        // Are we in the gap?
        if (xInColumn >= columnWidth) {
            return size_t(-1); // In gap between columns
        }
        
        // Calculate memory offset
        // Each column contains (displayHeight * columnWidth * bytesPerPixel) bytes
        size_t bytesPerColumn = displayHeight * columnWidth * bytesPerPixel;
        size_t columnStart = col * bytesPerColumn;
        
        // Position within the column
        size_t byteInRow = xInColumn * bytesPerPixel;
        size_t rowOffset = y * columnWidth * bytesPerPixel;  // columnWidth in pixels * bytesPerPixel = stride in bytes
        
        return columnStart + rowOffset + byteInRow;
    }
    
    static int GetBytesPerPixel(PixelFormat format) {
        switch (format.type) {
            case PixelFormat::GRAYSCALE: return 1;
            case PixelFormat::RGB565: return 2;
            case PixelFormat::RGB888:
            case PixelFormat::BGR888: return 3;
            case PixelFormat::RGBA8888:
            case PixelFormat::BGRA8888:
            case PixelFormat::ARGB8888:
            case PixelFormat::ABGR8888: return 4;
            case PixelFormat::HEX_PIXEL: return 4;
            case PixelFormat::CHAR_8BIT: return 1;
            case PixelFormat::BINARY: return 1;
            default: return 1;
        }
    }
};

// Unified memory renderer for both main and mini viewers
// Format descriptor for the rendering table
struct FormatDescriptor {
    int bytesIn;        // Number of source bytes consumed
    int pixelsOutX;     // Output width in pixels
    int pixelsOutY;     // Output height in pixels

    // Constructor for convenience
    FormatDescriptor(int bytes, int width, int height)
        : bytesIn(bytes), pixelsOutX(width), pixelsOutY(height) {}
};

// Extended format type that combines base format with modifiers like split
enum class ExtendedFormat {
    // Standard formats
    GRAYSCALE,
    RGB565,
    RGB888,
    RGBA8888,
    BGR888,
    BGRA8888,
    ARGB8888,
    ABGR8888,

    // Split component versions
    RGB565_SPLIT,
    RGB888_SPLIT,
    RGBA8888_SPLIT,
    BGR888_SPLIT,
    BGRA8888_SPLIT,
    ARGB8888_SPLIT,
    ABGR8888_SPLIT,

    // Special display formats
    BINARY,
    HEX_PIXEL,
    CHAR_8BIT
};

class MemoryRenderer {
public:
    // Main rendering function
    static std::vector<uint32_t> RenderMemory(
        const uint8_t* data,
        size_t dataSize,
        const RenderConfig& config
    );

    // Get format descriptor for a given format
    static FormatDescriptor GetFormatDescriptor(ExtendedFormat format);

    // Convert from PixelFormat::Type and split flag to ExtendedFormat
    static ExtendedFormat GetExtendedFormat(PixelFormat::Type format, bool splitComponents);

private:
    // Inner element renderers - render a single element at destination
    static void RenderHexElement(
        const uint8_t* src,      // 4 bytes input
        uint32_t* dest,          // Destination buffer
        int destStride           // Stride in pixels
    );

    static void RenderCharElement(
        const uint8_t* src,      // 1 byte input
        uint32_t* dest,          // Destination buffer
        int destStride           // Stride in pixels
    );

    static void RenderBinaryElement(
        const uint8_t* src,      // 1 byte input
        uint32_t* dest           // Destination buffer (8 pixels horizontal)
    );

    static void RenderPixelElement(
        const uint8_t* src,      // N bytes input (depends on format)
        uint32_t* dest,          // Destination buffer
        ExtendedFormat format    // Which pixel format
    );

    static void RenderSplitElement(
        const uint8_t* src,      // N bytes input (depends on format)
        uint32_t* dest,          // Destination buffer
        ExtendedFormat format    // Which split format
    );

    // Layout manager - handles columns and calls element renderers
    static std::vector<uint32_t> RenderWithLayout(
        const uint8_t* data,
        size_t dataSize,
        const RenderConfig& config,
        ExtendedFormat format
    );
    
    // Helper functions
    static uint32_t ExtractPixel(
        const uint8_t* data,
        size_t offset,
        size_t dataSize,
        PixelFormat format
    );
    
    static int GetBytesPerPixel(PixelFormat format);
    static int GetPixelWidth(PixelFormat format);
    static int GetPixelHeight(PixelFormat format);
};

}  // namespace Haywire