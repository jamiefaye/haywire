#include "memory_renderer.h"
#include "font5x7u.h"
#include "font_data.h"
#include <cstring>
#include <algorithm>

namespace Haywire {

std::vector<uint32_t> MemoryRenderer::RenderMemory(
    const uint8_t* data,
    size_t dataSize,
    const RenderConfig& config)
{
    // Handle split components mode
    if (config.splitComponents && 
        (config.format.type == PixelFormat::RGB888 || 
         config.format.type == PixelFormat::RGBA8888 ||
         config.format.type == PixelFormat::BGR888 ||
         config.format.type == PixelFormat::BGRA8888 ||
         config.format.type == PixelFormat::ARGB8888 ||
         config.format.type == PixelFormat::ABGR8888 ||
         config.format.type == PixelFormat::RGB565)) {
        return RenderSplitComponents(data, dataSize, config);
    }
    
    // Handle special display formats
    switch (config.format.type) {
        case PixelFormat::HEX_PIXEL:
            return RenderHexPixels(data, dataSize, config);
        case PixelFormat::CHAR_8BIT:
            return RenderCharPixels(data, dataSize, config);
        case PixelFormat::BINARY:
            return RenderBinaryPixels(data, dataSize, config);
        default:
            return RenderStandard(data, dataSize, config);
    }
}

std::vector<uint32_t> MemoryRenderer::RenderStandard(
    const uint8_t* data,
    size_t dataSize,
    const RenderConfig& config)
{
    std::vector<uint32_t> pixels(config.displayWidth * config.displayHeight, 0xFF000000);
    
    if (!data || dataSize == 0) {
        return pixels;
    }
    
    int bytesPerPixel = GetBytesPerPixel(config.format);
    
    if (config.columnMode) {
        // Column mode rendering
        for (int y = 0; y < config.displayHeight; y++) {
            for (int x = 0; x < config.displayWidth; x++) {
                // Calculate memory offset for this display position
                size_t offset = config.ColumnDisplayToMemory(x, y, 1);  // 1 pixel per element for standard formats
                
                if (offset == size_t(-1)) {
                    // We're in a gap between columns - leave as black
                    continue;
                }
                
                if (offset + bytesPerPixel <= dataSize) {
                    uint32_t pixel = ExtractPixel(data, offset, dataSize, config.format);
                    pixels[y * config.displayWidth + x] = pixel;
                }
            }
        }
    } else {
        // Linear mode rendering (existing code)
        for (int y = 0; y < config.height && y < config.displayHeight; y++) {
            for (int x = 0; x < config.width && x < config.displayWidth; x++) {
                size_t offset = y * config.stride + x * bytesPerPixel;
                
                if (offset + bytesPerPixel <= dataSize) {
                    uint32_t pixel = ExtractPixel(data, offset, dataSize, config.format);
                    pixels[y * config.displayWidth + x] = pixel;
                }
            }
        }
    }
    
    return pixels;
}

std::vector<uint32_t> MemoryRenderer::RenderHexPixels(
    const uint8_t* data,
    size_t dataSize,
    const RenderConfig& config)
{
    // Each hex cell is 33x7 pixels (32 for content + 1 separator, 6 for digits + 1 separator)
    const int CELL_WIDTH = 33;
    const int CELL_HEIGHT = 7;
    
    std::vector<uint32_t> pixels(config.displayWidth * config.displayHeight, 0xFF000000);
    
    if (!data || dataSize == 0) {
        return pixels;
    }
    
    // Calculate how many 32-bit values fit in one row
    size_t valuesPerRow = config.width / CELL_WIDTH;
    if (valuesPerRow == 0) valuesPerRow = 1;
    
    // Calculate how many rows we need
    size_t numValues = dataSize / 4;  // 4 bytes per value
    size_t numRows = (numValues + valuesPerRow - 1) / valuesPerRow;
    
    // Process each 32-bit value
    for (size_t valueIdx = 0; valueIdx < numValues; ++valueIdx) {
        // Calculate position
        size_t row = valueIdx / valuesPerRow;
        size_t col = valueIdx % valuesPerRow;
        
        if (row * CELL_HEIGHT >= config.displayHeight) break;  // Out of viewport
        
        // Read 32-bit value from memory (little-endian to match memory order)
        size_t memIdx = valueIdx * 4;
        if (memIdx + 3 >= dataSize) break;
        
        uint32_t value = (data[memIdx] << 0) |
                        (data[memIdx + 1] << 8) |
                        (data[memIdx + 2] << 16) |
                        (data[memIdx + 3] << 24);
        
        // Calculate colors from the actual memory bytes
        uint32_t bgColor = PackRGBA(
            data[memIdx + 2],
            data[memIdx + 1],
            data[memIdx],
            255
        );
        uint32_t fgColor = CalcHiContrastOpposite(bgColor);
        
        // Draw 8 hex nibbles (2 per byte)
        for (int nibbleIdx = 7; nibbleIdx >= 0; --nibbleIdx) {
            uint8_t nibble = (value >> (nibbleIdx * 4)) & 0xF;
            uint16_t glyph = GetGlyph3x5Hex(nibble);
            
            // Position of this nibble (4 pixels wide each)
            size_t nibbleX = col * CELL_WIDTH + (7 - nibbleIdx) * 4;
            size_t nibbleY = row * CELL_HEIGHT;
            
            // Fill first column with background (left border)
            for (int y = 0; y < 6; ++y) {
                size_t pixX = nibbleX;
                size_t pixY = nibbleY + y;
                if (pixX < config.displayWidth && pixY < config.displayHeight) {
                    size_t pixIdx = pixY * config.displayWidth + pixX;
                    pixels[pixIdx] = bgColor;
                }
            }
            
            // Fill first row with background (top border)
            for (int x = 0; x < 4; ++x) {
                size_t pixX = nibbleX + x;
                size_t pixY = nibbleY;
                if (pixX < config.displayWidth && pixY < config.displayHeight) {
                    size_t pixIdx = pixY * config.displayWidth + pixX;
                    pixels[pixIdx] = bgColor;
                }
            }
            
            // Draw the 3x5 glyph shifted by 1,1 (now in a 4x6 box with borders)
            for (int y = 0; y < 5; ++y) {
                for (int x = 0; x < 3; ++x) {
                    // Extract bit from glyph - flip horizontally (mirror X-axis)
                    int bitPos = (4 - y) * 3 + (2 - x);  // Mirror X: use (2-x) instead of x
                    bool bit = (glyph >> bitPos) & 1;
                    
                    size_t pixX = nibbleX + x + 1;  // Shift right by 1 for left border
                    size_t pixY = nibbleY + y + 1;  // Shift down by 1 for top border
                    
                    if (pixX < config.displayWidth && pixY < config.displayHeight) {
                        size_t pixIdx = pixY * config.displayWidth + pixX;
                        pixels[pixIdx] = bit ? fgColor : bgColor;
                    }
                }
            }
        }
        
        // Fill the rightmost column (33rd pixel) with background for this value
        for (int y = 0; y < CELL_HEIGHT; ++y) {
            size_t pixX = col * CELL_WIDTH + 32;  // The 33rd pixel (index 32)
            size_t pixY = row * CELL_HEIGHT + y;
            if (pixX < config.displayWidth && pixY < config.displayHeight) {
                size_t pixIdx = pixY * config.displayWidth + pixX;
                pixels[pixIdx] = bgColor;
            }
        }
        
        // Fill the bottom row (7th row) with background for this value
        for (int x = 0; x < 32; ++x) {  // Don't include the rightmost column (already filled)
            size_t pixX = col * CELL_WIDTH + x;
            size_t pixY = row * CELL_HEIGHT + 6;  // The 7th row (index 6)
            if (pixX < config.displayWidth && pixY < config.displayHeight) {
                size_t pixIdx = pixY * config.displayWidth + pixX;
                pixels[pixIdx] = bgColor;
            }
        }
    }
    
    return pixels;
}

std::vector<uint32_t> MemoryRenderer::RenderCharPixels(
    const uint8_t* data,
    size_t dataSize,
    const RenderConfig& config)
{
    const int CHAR_WIDTH = 6;
    const int CHAR_HEIGHT = 8;
    
    // Calculate how many characters fit in one row
    size_t charsPerRow = config.width / CHAR_WIDTH;
    if (charsPerRow == 0) charsPerRow = 1;
    
    // Calculate how many rows we need
    size_t numChars = dataSize;
    size_t numRows = (numChars + charsPerRow - 1) / charsPerRow;
    
    // Create expanded pixel buffer
    std::vector<uint32_t> pixels(config.displayWidth * config.displayHeight, 0xFF000000);
    
    if (!data || dataSize == 0) {
        return pixels;
    }
    
    // Process each byte as a character
    for (size_t charIdx = 0; charIdx < numChars; ++charIdx) {
        // Calculate position
        size_t row = charIdx / charsPerRow;
        size_t col = charIdx % charsPerRow;
        
        if (row * CHAR_HEIGHT >= config.displayHeight) break;  // Out of viewport
        
        uint8_t charCode = data[charIdx];
        
        // Find the glyph in Font5x7u array by searching for the character code
        uint64_t glyph = 0;
        
        // Display null characters as blank
        if (charCode != 0) {
            for (size_t i = 0; i < Font5x7u_count; ++i) {
                uint64_t entry = Font5x7u[i];
                uint16_t glyphCode = (entry >> 48) & 0xFFFF;  // Character code in top 16 bits
                if (glyphCode == charCode) {
                    glyph = entry;
                    break;
                }
            }
        }
        
        // Simple color scheme: always white text on black background
        uint32_t fgColor = 0xFFFFFFFF;  // White
        uint32_t bgColor = 0xFF000000;  // Black
        
        // Position of this character
        size_t charX = col * CHAR_WIDTH;
        size_t charY = row * CHAR_HEIGHT;
        
        // Draw the 5x7 glyph in a 6x8 box
        // Using the pattern from the reference code:
        // Start at bit 47 and read sequentially downward
        uint64_t rotatingBit = 0x0000800000000000ULL;  // bit 47
        
        for (int y = 0; y < CHAR_HEIGHT; ++y) {
            for (int x = 0; x < CHAR_WIDTH; ++x) {
                size_t pixX = charX + x;
                size_t pixY = charY + y;
                
                if (pixX < config.displayWidth && pixY < config.displayHeight) {
                    size_t pixIdx = pixY * config.displayWidth + pixX;
                    // Check if bit is set
                    bool bit = (glyph & rotatingBit) != 0;
                    pixels[pixIdx] = bit ? fgColor : bgColor;
                }
                
                rotatingBit >>= 1;  // Move to next bit
            }
        }
    }
    
    return pixels;
}

std::vector<uint32_t> MemoryRenderer::RenderSplitComponents(
    const uint8_t* data,
    size_t dataSize,
    const RenderConfig& config)
{
    std::vector<uint32_t> pixels(config.displayWidth * config.displayHeight, 0xFF000000);

    if (!data || dataSize == 0) {
        return pixels;
    }

    int bytesPerPixel = GetBytesPerPixel(config.format);
    int componentsPerPixel = (config.format.type == PixelFormat::RGB565) ? 3 : bytesPerPixel;

    // Each source pixel expands to componentsPerPixel destination pixels horizontally
    for (int y = 0; y < config.height && y < config.displayHeight; y++) {
        for (int x = 0; x < config.width; x++) {
            size_t offset = y * config.stride + x * bytesPerPixel;

            if (offset + bytesPerPixel <= dataSize) {
                // Extract components based on format
                // Order: [0]=Alpha (if present), [1]=Red, [2]=Green, [3]=Blue
                uint8_t components[4] = {255, 0, 0, 0};  // Default alpha to 255
                bool hasAlpha = false;

                switch (config.format.type) {
                    case PixelFormat::RGB888:
                        components[1] = data[offset];      // R
                        components[2] = data[offset + 1];  // G
                        components[3] = data[offset + 2];  // B
                        break;
                    case PixelFormat::RGBA8888:
                        components[1] = data[offset];      // R
                        components[2] = data[offset + 1];  // G
                        components[3] = data[offset + 2];  // B
                        components[0] = data[offset + 3];  // A
                        hasAlpha = true;
                        break;
                    case PixelFormat::BGR888:
                        components[3] = data[offset];      // B
                        components[2] = data[offset + 1];  // G
                        components[1] = data[offset + 2];  // R
                        break;
                    case PixelFormat::BGRA8888:
                        components[3] = data[offset];      // B
                        components[2] = data[offset + 1];  // G
                        components[1] = data[offset + 2];  // R
                        components[0] = data[offset + 3];  // A
                        hasAlpha = true;
                        break;
                    case PixelFormat::ARGB8888:
                        components[0] = data[offset];      // A
                        components[1] = data[offset + 1];  // R
                        components[2] = data[offset + 2];  // G
                        components[3] = data[offset + 3];  // B
                        hasAlpha = true;
                        break;
                    case PixelFormat::ABGR8888:
                        components[0] = data[offset];      // A
                        components[3] = data[offset + 1];  // B
                        components[2] = data[offset + 2];  // G
                        components[1] = data[offset + 3];  // R
                        hasAlpha = true;
                        break;
                    case PixelFormat::RGB565: {
                        uint16_t pixel = *(uint16_t*)(data + offset);
                        components[1] = ((pixel >> 11) & 0x1F) << 3;  // R
                        components[2] = ((pixel >> 5) & 0x3F) << 2;   // G
                        components[3] = (pixel & 0x1F) << 3;          // B
                        break;
                    }
                }

                // Draw the expanded pixels in order: A R G B (or R G B if no alpha)
                int startComp = hasAlpha ? 0 : 1;  // Skip alpha slot if format has no alpha
                int outputIdx = 0;

                for (int comp = startComp; comp < 4; comp++) {
                    int destX = x * componentsPerPixel + outputIdx;
                    int destY = y;

                    if (destX < config.displayWidth && destY < config.displayHeight) {
                        uint8_t value = components[comp];
                        uint32_t color = 0xFF000000;  // Full alpha

                        if (comp == 0) {
                            // Alpha - show as grayscale
                            color |= (value << 16) | (value << 8) | value;
                        } else if (comp == 1) {
                            // Red - show in red channel only
                            color |= (value << 16);
                        } else if (comp == 2) {
                            // Green - show in green channel only
                            color |= (value << 8);
                        } else if (comp == 3) {
                            // Blue - show in blue channel only
                            color |= value;
                        }

                        pixels[destY * config.displayWidth + destX] = color;
                    }
                    outputIdx++;
                }
            }
        }
    }

    return pixels;
}

std::vector<uint32_t> MemoryRenderer::RenderBinaryPixels(
    const uint8_t* data,
    size_t dataSize,
    const RenderConfig& config)
{
    std::vector<uint32_t> pixels(config.displayWidth * config.displayHeight, 0xFF000000);
    
    if (!data || dataSize == 0) {
        return pixels;
    }
    
    // Each byte becomes 8 pixels horizontally
    int bytesPerRow = config.width / 8;
    if (bytesPerRow == 0) bytesPerRow = 1;
    
    for (int y = 0; y < config.height && y < config.displayHeight; y++) {
        for (int byteX = 0; byteX < bytesPerRow; byteX++) {
            size_t byteIndex = y * bytesPerRow + byteX;
            if (byteIndex >= dataSize) break;
            
            uint8_t byte = data[byteIndex];
            
            for (int bit = 0; bit < 8; bit++) {
                int pixelX = byteX * 8 + bit;
                if (pixelX < config.displayWidth && y < config.displayHeight) {
                    uint32_t color = (byte & (0x80 >> bit)) ? 0xFFFFFFFF : 0xFF000000;
                    pixels[y * config.displayWidth + pixelX] = color;
                }
            }
        }
    }
    
    return pixels;
}

uint32_t MemoryRenderer::ExtractPixel(
    const uint8_t* data,
    size_t offset,
    size_t dataSize,
    PixelFormat format)
{
    if (offset >= dataSize) {
        return 0xFF000000;  // Black for out-of-bounds
    }
    
    uint32_t pixel = 0xFF000000;  // Default alpha
    
    switch (format.type) {
        case PixelFormat::GRAYSCALE: {
            uint8_t gray = data[offset];
            pixel = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
            break;
        }
        case PixelFormat::RGB565: {
            if (offset + 1 < dataSize) {
                uint16_t rgb = *(uint16_t*)(data + offset);
                uint8_t r = ((rgb >> 11) & 0x1F) << 3;
                uint8_t g = ((rgb >> 5) & 0x3F) << 2;
                uint8_t b = (rgb & 0x1F) << 3;
                pixel = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
            break;
        }
        case PixelFormat::RGB888: {
            if (offset + 2 < dataSize) {
                pixel = 0xFF000000 | (data[offset] << 16) | (data[offset+1] << 8) | data[offset+2];
            }
            break;
        }
        case PixelFormat::RGBA8888: {
            if (offset + 3 < dataSize) {
                pixel = (data[offset+3] << 24) | (data[offset] << 16) | (data[offset+1] << 8) | data[offset+2];
            }
            break;
        }
        case PixelFormat::BGR888: {
            if (offset + 2 < dataSize) {
                pixel = 0xFF000000 | (data[offset+2] << 16) | (data[offset+1] << 8) | data[offset];
            }
            break;
        }
        case PixelFormat::BGRA8888: {
            if (offset + 3 < dataSize) {
                pixel = (data[offset+3] << 24) | (data[offset+2] << 16) | (data[offset+1] << 8) | data[offset];
            }
            break;
        }
        case PixelFormat::ARGB8888: {
            if (offset + 3 < dataSize) {
                pixel = (data[offset] << 24) | (data[offset+1] << 16) | (data[offset+2] << 8) | data[offset+3];
            }
            break;
        }
        case PixelFormat::ABGR8888: {
            if (offset + 3 < dataSize) {
                pixel = (data[offset] << 24) | (data[offset+3] << 16) | (data[offset+2] << 8) | data[offset+1];
            }
            break;
        }
    }
    
    return pixel;
}

int MemoryRenderer::GetBytesPerPixel(PixelFormat format) {
    switch (format.type) {
        case PixelFormat::GRAYSCALE:
        case PixelFormat::CHAR_8BIT:
        case PixelFormat::BINARY:
            return 1;
        case PixelFormat::RGB565:
            return 2;
        case PixelFormat::RGB888:
        case PixelFormat::BGR888:
            return 3;
        case PixelFormat::RGBA8888:
        case PixelFormat::BGRA8888:
        case PixelFormat::ARGB8888:
        case PixelFormat::ABGR8888:
        case PixelFormat::HEX_PIXEL:
            return 4;
        default:
            return 1;
    }
}

int MemoryRenderer::GetPixelWidth(PixelFormat format) {
    switch (format.type) {
        case PixelFormat::HEX_PIXEL:
            return 33;  // 8 hex digits * 4 pixels + separators
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