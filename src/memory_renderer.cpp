#include "memory_renderer.h"
#include "font5x7u.h"
#include "font_data.h"
#include <cstring>
#include <algorithm>

namespace Haywire {

// Format descriptor table
FormatDescriptor MemoryRenderer::GetFormatDescriptor(ExtendedFormat format) {
    switch (format) {
        // Standard single-pixel formats
        case ExtendedFormat::GRAYSCALE:    return FormatDescriptor(1, 1, 1);
        case ExtendedFormat::RGB565:       return FormatDescriptor(2, 1, 1);
        case ExtendedFormat::RGB888:       return FormatDescriptor(3, 1, 1);
        case ExtendedFormat::RGBA8888:     return FormatDescriptor(4, 1, 1);
        case ExtendedFormat::BGR888:       return FormatDescriptor(3, 1, 1);
        case ExtendedFormat::BGRA8888:     return FormatDescriptor(4, 1, 1);
        case ExtendedFormat::ARGB8888:     return FormatDescriptor(4, 1, 1);
        case ExtendedFormat::ABGR8888:     return FormatDescriptor(4, 1, 1);

        // Split component formats (horizontal expansion)
        case ExtendedFormat::RGB565_SPLIT:   return FormatDescriptor(2, 3, 1);  // R, G, B
        case ExtendedFormat::RGB888_SPLIT:   return FormatDescriptor(3, 3, 1);  // R, G, B
        case ExtendedFormat::RGBA8888_SPLIT: return FormatDescriptor(4, 4, 1);  // R, G, B, A
        case ExtendedFormat::BGR888_SPLIT:   return FormatDescriptor(3, 3, 1);  // B, G, R
        case ExtendedFormat::BGRA8888_SPLIT: return FormatDescriptor(4, 4, 1);  // B, G, R, A
        case ExtendedFormat::ARGB8888_SPLIT: return FormatDescriptor(4, 4, 1);  // A, R, G, B
        case ExtendedFormat::ABGR8888_SPLIT: return FormatDescriptor(4, 4, 1);  // A, B, G, R

        // Special display formats
        case ExtendedFormat::BINARY:      return FormatDescriptor(1, 8, 1);   // 8 bits as pixels
        case ExtendedFormat::HEX_PIXEL:   return FormatDescriptor(4, 33, 7);  // Hex display
        case ExtendedFormat::CHAR_8BIT:   return FormatDescriptor(1, 6, 8);   // Character glyph

        default:
            return FormatDescriptor(1, 1, 1);  // Default fallback
    }
}

// Convert from PixelFormat::Type and split flag to ExtendedFormat
ExtendedFormat MemoryRenderer::GetExtendedFormat(PixelFormat::Type format, bool splitComponents) {
    if (splitComponents) {
        switch (format) {
            case PixelFormat::RGB565:   return ExtendedFormat::RGB565_SPLIT;
            case PixelFormat::RGB888:   return ExtendedFormat::RGB888_SPLIT;
            case PixelFormat::RGBA8888: return ExtendedFormat::RGBA8888_SPLIT;
            case PixelFormat::BGR888:   return ExtendedFormat::BGR888_SPLIT;
            case PixelFormat::BGRA8888: return ExtendedFormat::BGRA8888_SPLIT;
            case PixelFormat::ARGB8888: return ExtendedFormat::ARGB8888_SPLIT;
            case PixelFormat::ABGR8888: return ExtendedFormat::ABGR8888_SPLIT;
            default:                    return ExtendedFormat::GRAYSCALE;  // Fallback
        }
    }

    // Non-split formats
    switch (format) {
        case PixelFormat::GRAYSCALE: return ExtendedFormat::GRAYSCALE;
        case PixelFormat::RGB565:    return ExtendedFormat::RGB565;
        case PixelFormat::RGB888:    return ExtendedFormat::RGB888;
        case PixelFormat::RGBA8888:  return ExtendedFormat::RGBA8888;
        case PixelFormat::BGR888:    return ExtendedFormat::BGR888;
        case PixelFormat::BGRA8888:  return ExtendedFormat::BGRA8888;
        case PixelFormat::ARGB8888:  return ExtendedFormat::ARGB8888;
        case PixelFormat::ABGR8888:  return ExtendedFormat::ABGR8888;
        case PixelFormat::BINARY:    return ExtendedFormat::BINARY;
        case PixelFormat::HEX_PIXEL: return ExtendedFormat::HEX_PIXEL;
        case PixelFormat::CHAR_8BIT: return ExtendedFormat::CHAR_8BIT;
        default:                     return ExtendedFormat::GRAYSCALE;  // Default
    }
}

// =============================================================================
// Single-element renderer functions
// =============================================================================

// Render a single hex element (4 bytes -> 33x7 pixels)
void MemoryRenderer::RenderHexElement(
    const uint8_t* src,      // 4 bytes input
    uint32_t* dest,          // Destination buffer
    int destStride)          // Stride in pixels
{
    // Read 32-bit value (little-endian)
    uint32_t value = (src[0] << 0) |
                     (src[1] << 8) |
                     (src[2] << 16) |
                     (src[3] << 24);

    // Calculate colors from the actual memory bytes
    uint32_t bgColor = PackRGBA(src[2], src[1], src[0], 255);
    uint32_t fgColor = CalcHiContrastOpposite(bgColor);

    // Draw 8 hex nibbles (2 per byte) across 32 pixels + 1 separator
    for (int nibbleIdx = 7; nibbleIdx >= 0; --nibbleIdx) {
        uint8_t nibble = (value >> (nibbleIdx * 4)) & 0xF;
        uint16_t glyph = GetGlyph3x5Hex(nibble);

        // Position of this nibble (4 pixels wide each)
        int nibbleX = (7 - nibbleIdx) * 4;

        // Fill first column with background (left border)
        for (int y = 0; y < 6; ++y) {
            dest[y * destStride + nibbleX] = bgColor;
        }

        // Fill first row with background (top border)
        for (int x = 0; x < 4; ++x) {
            dest[nibbleX + x] = bgColor;
        }

        // Draw the 3x5 glyph shifted by 1,1
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 3; ++x) {
                // Extract bit from glyph - flip horizontally
                int bitPos = (4 - y) * 3 + (2 - x);
                bool bit = (glyph >> bitPos) & 1;

                dest[(y + 1) * destStride + (nibbleX + x + 1)] = bit ? fgColor : bgColor;
            }
        }
    }

    // Fill the rightmost column (33rd pixel) with background
    for (int y = 0; y < 7; ++y) {
        dest[y * destStride + 32] = bgColor;
    }

    // Fill the bottom row (7th row) with background
    for (int x = 0; x < 33; ++x) {
        dest[6 * destStride + x] = bgColor;
    }
}

// Render a single character element (1 byte -> 6x8 pixels)
void MemoryRenderer::RenderCharElement(
    const uint8_t* src,      // 1 byte input
    uint32_t* dest,          // Destination buffer
    int destStride)          // Stride in pixels
{
    uint8_t ch = *src;

    // Get colors based on character value
    uint32_t bgColor = PackRGBA((ch & 0xE0) >> 5 << 5,
                                 (ch & 0x1C) >> 2 << 5,
                                 (ch & 0x03) << 6,
                                 255);
    uint32_t fgColor = CalcHiContrastOpposite(bgColor);

    // Find the font glyph
    uint64_t glyph = 0;
    for (size_t i = 0; i < Font5x7u_count; ++i) {
        uint64_t entry = Font5x7u[i];
        uint16_t glyphCode = (entry >> 48) & 0xFFFF;
        if (glyphCode == ch) {
            glyph = entry;
            break;
        }
    }

    // Draw the 5x7 character in a 6x8 box
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 6; ++x) {
            uint32_t color = bgColor;

            if (y < 7 && x < 5 && glyph) {
                // Extract the row data from packed glyph
                int rowShift = 40 - (y * 6);  // Each row uses 6 bits
                uint8_t row = (glyph >> rowShift) & 0x3F;
                bool bit = (row >> (4 - x)) & 1;
                if (bit) color = fgColor;
            }

            dest[y * destStride + x] = color;
        }
    }
}

// Render a single binary element (1 byte -> 8x1 pixels)
void MemoryRenderer::RenderBinaryElement(
    const uint8_t* src,      // 1 byte input
    uint32_t* dest)          // Destination buffer (8 pixels horizontal)
{
    uint8_t byte = *src;

    // Each bit becomes a pixel
    for (int bit = 0; bit < 8; ++bit) {
        bool isSet = (byte >> (7 - bit)) & 1;
        dest[bit] = isSet ? 0xFFFFFFFF : 0xFF000000;
    }
}

// Render a single pixel element (standard formats)
void MemoryRenderer::RenderPixelElement(
    const uint8_t* src,      // N bytes input (depends on format)
    uint32_t* dest,          // Destination buffer
    ExtendedFormat format)   // Which pixel format
{
    PixelFormat pixelFormat;

    // Map extended format back to base format
    switch (format) {
        case ExtendedFormat::GRAYSCALE:
            pixelFormat.type = PixelFormat::GRAYSCALE;
            *dest = ExtractPixel(src, 0, 1, pixelFormat);
            break;
        case ExtendedFormat::RGB565:
            pixelFormat.type = PixelFormat::RGB565;
            *dest = ExtractPixel(src, 0, 2, pixelFormat);
            break;
        case ExtendedFormat::RGB888:
            pixelFormat.type = PixelFormat::RGB888;
            *dest = ExtractPixel(src, 0, 3, pixelFormat);
            break;
        case ExtendedFormat::RGBA8888:
            pixelFormat.type = PixelFormat::RGBA8888;
            *dest = ExtractPixel(src, 0, 4, pixelFormat);
            break;
        case ExtendedFormat::BGR888:
            pixelFormat.type = PixelFormat::BGR888;
            *dest = ExtractPixel(src, 0, 3, pixelFormat);
            break;
        case ExtendedFormat::BGRA8888:
            pixelFormat.type = PixelFormat::BGRA8888;
            *dest = ExtractPixel(src, 0, 4, pixelFormat);
            break;
        case ExtendedFormat::ARGB8888:
            pixelFormat.type = PixelFormat::ARGB8888;
            *dest = ExtractPixel(src, 0, 4, pixelFormat);
            break;
        case ExtendedFormat::ABGR8888:
            pixelFormat.type = PixelFormat::ABGR8888;
            *dest = ExtractPixel(src, 0, 4, pixelFormat);
            break;
        default:
            *dest = 0xFF000000;  // Black for unknown
            break;
    }
}

// Render a split element (RGB/RGBA -> multiple pixels showing components)
void MemoryRenderer::RenderSplitElement(
    const uint8_t* src,      // N bytes input (depends on format)
    uint32_t* dest,          // Destination buffer
    ExtendedFormat format)   // Which split format
{
    switch (format) {
        case ExtendedFormat::RGB565_SPLIT: {
            uint16_t pixel = (src[1] << 8) | src[0];
            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;
            dest[0] = PackRGBA(r, 0, 0, 255);
            dest[1] = PackRGBA(0, g, 0, 255);
            dest[2] = PackRGBA(0, 0, b, 255);
            break;
        }
        case ExtendedFormat::RGB888_SPLIT:
            dest[0] = PackRGBA(src[0], 0, 0, 255);
            dest[1] = PackRGBA(0, src[1], 0, 255);
            dest[2] = PackRGBA(0, 0, src[2], 255);
            break;
        case ExtendedFormat::RGBA8888_SPLIT:
            dest[0] = PackRGBA(src[0], 0, 0, 255);
            dest[1] = PackRGBA(0, src[1], 0, 255);
            dest[2] = PackRGBA(0, 0, src[2], 255);
            dest[3] = PackRGBA(src[3], src[3], src[3], 255);
            break;
        case ExtendedFormat::BGR888_SPLIT:
            dest[0] = PackRGBA(0, 0, src[0], 255);
            dest[1] = PackRGBA(0, src[1], 0, 255);
            dest[2] = PackRGBA(src[2], 0, 0, 255);
            break;
        case ExtendedFormat::BGRA8888_SPLIT:
            dest[0] = PackRGBA(0, 0, src[0], 255);
            dest[1] = PackRGBA(0, src[1], 0, 255);
            dest[2] = PackRGBA(src[2], 0, 0, 255);
            dest[3] = PackRGBA(src[3], src[3], src[3], 255);
            break;
        case ExtendedFormat::ARGB8888_SPLIT:
            dest[0] = PackRGBA(src[0], src[0], src[0], 255);
            dest[1] = PackRGBA(src[1], 0, 0, 255);
            dest[2] = PackRGBA(0, src[2], 0, 255);
            dest[3] = PackRGBA(0, 0, src[3], 255);
            break;
        case ExtendedFormat::ABGR8888_SPLIT:
            dest[0] = PackRGBA(src[0], src[0], src[0], 255);
            dest[1] = PackRGBA(0, 0, src[1], 255);
            dest[2] = PackRGBA(0, src[2], 0, 255);
            dest[3] = PackRGBA(src[3], 0, 0, 255);
            break;
        default:
            break;
    }
}

// =============================================================================
// Layout manager - handles columns and calls element renderers
// =============================================================================

std::vector<uint32_t> MemoryRenderer::RenderWithLayout(
    const uint8_t* data,
    size_t dataSize,
    const RenderConfig& config,
    ExtendedFormat format)
{
    // Create output buffer
    std::vector<uint32_t> pixels(config.displayWidth * config.displayHeight, 0xFF000000);

    if (!data || dataSize == 0) {
        return pixels;
    }

    // Get format descriptor
    FormatDescriptor desc = GetFormatDescriptor(format);

    // Calculate layout parameters
    int elementWidth = desc.pixelsOutX;
    int elementHeight = desc.pixelsOutY;

    // Set up column parameters
    // Non-column mode is just 1 column with full width and no gap
    int columnWidth = config.columnMode ? config.columnWidth : config.displayWidth;
    int columnGap = config.columnMode ? config.columnGap : 0;

    // Calculate how many elements fit in one column row (horizontally)
    int elementsPerRow = columnWidth / elementWidth;
    if (elementsPerRow == 0) elementsPerRow = 1;

    // Calculate column parameters
    int totalColumnWidth = columnWidth + columnGap;
    int columnsPerScreen = (config.displayWidth + columnGap) / totalColumnWidth;
    if (columnsPerScreen == 0) columnsPerScreen = 1;

    // Calculate rows per column
    int rowsPerColumn = config.displayHeight / elementHeight;
    if (rowsPerColumn == 0) rowsPerColumn = 1;

    // Process data element by element
    size_t srcOffset = 0;
    int currentCol = 0;
    int currentRow = 0;
    int elementInRow = 0;

    while (srcOffset + desc.bytesIn <= dataSize) {
        // Calculate destination position
        int destX = currentCol * totalColumnWidth + elementInRow * elementWidth;
        int destY = currentRow * elementHeight;

        // Check bounds and render
        if (destX + elementWidth <= currentCol * totalColumnWidth + columnWidth &&
            destX + elementWidth <= config.displayWidth &&
            destY + elementHeight <= config.displayHeight) {
            // Render element at this position
            uint32_t* destPtr = pixels.data() + destY * config.displayWidth + destX;

            // Call appropriate renderer based on format
            switch (format) {
                case ExtendedFormat::HEX_PIXEL:
                    RenderHexElement(data + srcOffset, destPtr, config.displayWidth);
                    break;
                case ExtendedFormat::CHAR_8BIT:
                    RenderCharElement(data + srcOffset, destPtr, config.displayWidth);
                    break;
                case ExtendedFormat::BINARY:
                    // Binary is single-line, no stride needed
                    RenderBinaryElement(data + srcOffset, destPtr);
                    break;
                case ExtendedFormat::RGB565_SPLIT:
                case ExtendedFormat::RGB888_SPLIT:
                case ExtendedFormat::RGBA8888_SPLIT:
                case ExtendedFormat::BGR888_SPLIT:
                case ExtendedFormat::BGRA8888_SPLIT:
                case ExtendedFormat::ARGB8888_SPLIT:
                case ExtendedFormat::ABGR8888_SPLIT:
                    RenderSplitElement(data + srcOffset, destPtr, format);
                    break;
                default:
                    // Standard pixel formats
                    RenderPixelElement(data + srcOffset, destPtr, format);
                    break;
            }
        }

        // Advance to next element
        srcOffset += desc.bytesIn;
        elementInRow++;

        // Check if we need to wrap to next row
        if (elementInRow >= elementsPerRow) {
            elementInRow = 0;
            currentRow++;

            // Check if we need to wrap to next column
            if (currentRow >= rowsPerColumn) {
                currentRow = 0;
                currentCol++;

                // Check if we've filled all columns
                if (currentCol >= columnsPerScreen) {
                    break;  // Screen is full
                }
            }
        }
    }

    return pixels;
}

std::vector<uint32_t> MemoryRenderer::RenderMemory(
    const uint8_t* data,
    size_t dataSize,
    const RenderConfig& config)
{
    // Convert to extended format
    ExtendedFormat format = GetExtendedFormat(config.format.type, config.splitComponents);

    // Use the new unified rendering pipeline
    return RenderWithLayout(data, dataSize, config, format);
}

// Helper function to extract a pixel value from memory
uint32_t MemoryRenderer::ExtractPixel(
    const uint8_t* data,
    size_t offset,
    size_t dataSize,
    PixelFormat format)
{
    if (offset >= dataSize) {
        return 0xFF000000;  // Black for out-of-bounds
    }

    switch (format.type) {
        case PixelFormat::GRAYSCALE: {
            if (offset >= dataSize) return 0xFF000000;
            uint8_t gray = data[offset];
            return PackRGBA(gray, gray, gray, 255);
        }
        case PixelFormat::RGB565: {
            if (offset + 1 >= dataSize) return 0xFF000000;
            uint16_t pixel = (data[offset + 1] << 8) | data[offset];
            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;
            return PackRGBA(r, g, b, 255);
        }
        case PixelFormat::RGB888: {
            if (offset + 2 >= dataSize) return 0xFF000000;
            return PackRGBA(data[offset], data[offset + 1], data[offset + 2], 255);
        }
        case PixelFormat::RGBA8888: {
            if (offset + 3 >= dataSize) return 0xFF000000;
            return PackRGBA(data[offset], data[offset + 1], data[offset + 2], data[offset + 3]);
        }
        case PixelFormat::BGR888: {
            if (offset + 2 >= dataSize) return 0xFF000000;
            return PackRGBA(data[offset + 2], data[offset + 1], data[offset], 255);
        }
        case PixelFormat::BGRA8888: {
            if (offset + 3 >= dataSize) return 0xFF000000;
            return PackRGBA(data[offset + 2], data[offset + 1], data[offset], data[offset + 3]);
        }
        case PixelFormat::ARGB8888: {
            if (offset + 3 >= dataSize) return 0xFF000000;
            return PackRGBA(data[offset + 1], data[offset + 2], data[offset + 3], data[offset]);
        }
        case PixelFormat::ABGR8888: {
            if (offset + 3 >= dataSize) return 0xFF000000;
            return PackRGBA(data[offset + 3], data[offset + 2], data[offset + 1], data[offset]);
        }
        default:
            return 0xFF000000;  // Black for unknown formats
    }
}

int MemoryRenderer::GetBytesPerPixel(PixelFormat format) {
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

int MemoryRenderer::GetPixelWidth(PixelFormat format) {
    switch (format.type) {
        case PixelFormat::HEX_PIXEL:
            return 33;  // Hex cell width
        case PixelFormat::CHAR_8BIT:
            return 6;   // Character width
        case PixelFormat::BINARY:
            return 1;   // Each bit is one pixel
        default:
            return 1;   // Standard formats are 1:1
    }
}

int MemoryRenderer::GetPixelHeight(PixelFormat format) {
    switch (format.type) {
        case PixelFormat::HEX_PIXEL:
            return 7;   // Hex cell height
        case PixelFormat::CHAR_8BIT:
            return 8;   // Character height
        default:
            return 1;   // Standard formats are 1:1
    }
}

}  // namespace Haywire