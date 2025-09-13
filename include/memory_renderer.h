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
    
    // Column mode settings (future)
    bool columnMode = false;
    int columnWidth = 0;   // Width of each column in bytes
    int columnGap = 0;     // Gap between columns in bytes
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