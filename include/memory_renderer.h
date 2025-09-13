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
    int columnWidth = 256;    // Width of each column in bytes (inner width)
    int columnHeight = 256;   // Height of each column in rows
    int columnGap = 8;        // Gap between columns in pixels
    
    // Calculate memory offset for a display position in column mode
    size_t ColumnDisplayToMemory(int x, int y, int pixelWidth) const {
        if (!columnMode) return size_t(-1);
        
        // Calculate column pixel width based on format
        int columnPixelWidth = columnWidth * pixelWidth / GetBytesPerPixel(format);
        int totalColumnWidth = columnPixelWidth + columnGap;
        
        // Which column are we in?
        int col = x / totalColumnWidth;
        int xInColumn = x % totalColumnWidth;
        
        // Are we in the gap?
        if (xInColumn >= columnPixelWidth) {
            return size_t(-1); // In gap between columns
        }
        
        // Calculate memory offset
        size_t bytesPerColumn = stride * columnHeight;
        size_t columnStart = col * bytesPerColumn;
        size_t rowInColumn = y % columnHeight;
        
        // Convert pixel position to byte position
        size_t byteInRow = (xInColumn * GetBytesPerPixel(format)) / pixelWidth;
        
        return columnStart + (rowInColumn * stride) + byteInRow;
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
class MemoryRenderer {
public:
    // Main rendering function
    static std::vector<uint32_t> RenderMemory(
        const uint8_t* data,
        size_t dataSize,
        const RenderConfig& config
    );
    
private:
    // Format-specific renderers
    static std::vector<uint32_t> RenderStandard(
        const uint8_t* data,
        size_t dataSize,
        const RenderConfig& config
    );
    
    static std::vector<uint32_t> RenderHexPixels(
        const uint8_t* data,
        size_t dataSize,
        const RenderConfig& config
    );
    
    static std::vector<uint32_t> RenderCharPixels(
        const uint8_t* data,
        size_t dataSize,
        const RenderConfig& config
    );
    
    static std::vector<uint32_t> RenderSplitComponents(
        const uint8_t* data,
        size_t dataSize,
        const RenderConfig& config
    );
    
    static std::vector<uint32_t> RenderBinaryPixels(
        const uint8_t* data,
        size_t dataSize,
        const RenderConfig& config
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