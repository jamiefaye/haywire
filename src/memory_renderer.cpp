#include "memory_renderer.h"
#include "font5x7u.h"
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
    
    for (int y = 0; y < config.height && y < config.displayHeight; y++) {
        for (int x = 0; x < config.width && x < config.displayWidth; x++) {
            size_t offset = y * config.stride + x * bytesPerPixel;
            
            if (offset + bytesPerPixel <= dataSize) {
                uint32_t pixel = ExtractPixel(data, offset, dataSize, config.format);
                pixels[y * config.displayWidth + x] = pixel;
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
    // Each hex cell is 33x7 pixels (32 for digits + 1 for separator, 7 rows)
    const int CELL_WIDTH = 33;
    const int CELL_HEIGHT = 7;
    const int CHAR_WIDTH = 4;
    
    std::vector<uint32_t> pixels(config.displayWidth * config.displayHeight, 0xFF000000);
    
    if (!data || dataSize == 0) {
        return pixels;
    }
    
    // Simple 4x6 hex digit bitmaps
    static const uint16_t hexDigits[16] = {
        0x6996, // 0
        0x2622, // 1
        0x691E, // 2
        0xE11E, // 3
        0x99F1, // 4
        0xE88E, // 5
        0x698E, // 6
        0xE111, // 7
        0x6966, // 8
        0xE996, // 9
        0x9996, // A
        0x699D, // B
        0x7887, // C
        0x9669, // D
        0x788F, // E
        0x788F  // F (with baseline)
    };
    
    // Calculate how many cells fit
    int cellsPerRow = config.width / CELL_WIDTH;
    if (cellsPerRow == 0) cellsPerRow = 1;
    
    int totalCells = (dataSize + 3) / 4;  // Round up
    int cellRows = (totalCells + cellsPerRow - 1) / cellsPerRow;
    
    for (int cellY = 0; cellY < cellRows; cellY++) {
        for (int cellX = 0; cellX < cellsPerRow; cellX++) {
            size_t byteIndex = (cellY * cellsPerRow + cellX) * 4;
            if (byteIndex >= dataSize) break;
            
            // Read up to 4 bytes
            uint32_t value = 0;
            for (int i = 0; i < 4 && byteIndex + i < dataSize; i++) {
                value |= ((uint32_t)data[byteIndex + i]) << (i * 8);
            }
            
            // Draw 8 hex digits
            for (int digit = 0; digit < 8; digit++) {
                int nibble = (value >> ((7 - digit) * 4)) & 0xF;
                uint16_t bitmap = hexDigits[nibble];
                
                int digitX = cellX * CELL_WIDTH + digit * CHAR_WIDTH;
                int digitY = cellY * CELL_HEIGHT;
                
                // Draw the 4x6 bitmap
                for (int py = 0; py < 6; py++) {
                    for (int px = 0; px < 3; px++) {
                        if (bitmap & (1 << (py * 3 + px))) {
                            int finalX = digitX + px;
                            int finalY = digitY + py;
                            if (finalX < config.displayWidth && finalY < config.displayHeight) {
                                pixels[finalY * config.displayWidth + finalX] = 0xFF00FF00;
                            }
                        }
                    }
                }
            }
            
            // Draw separator line
            if (cellX < cellsPerRow - 1) {
                int sepX = (cellX + 1) * CELL_WIDTH - 1;
                for (int py = 0; py < CELL_HEIGHT && cellY * CELL_HEIGHT + py < config.displayHeight; py++) {
                    if (sepX < config.displayWidth) {
                        pixels[(cellY * CELL_HEIGHT + py) * config.displayWidth + sepX] = 0xFF404040;
                    }
                }
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
    
    std::vector<uint32_t> pixels(config.displayWidth * config.displayHeight, 0xFF000000);
    
    if (!data || dataSize == 0) {
        return pixels;
    }
    
    int charsPerRow = config.width / CHAR_WIDTH;
    if (charsPerRow == 0) charsPerRow = 1;
    
    int totalChars = dataSize;
    int charRows = (totalChars + charsPerRow - 1) / charsPerRow;
    
    for (int row = 0; row < charRows; row++) {
        for (int col = 0; col < charsPerRow; col++) {
            size_t charIndex = row * charsPerRow + col;
            if (charIndex >= dataSize) break;
            
            uint8_t charCode = data[charIndex];
            
            // Find the glyph in Font5x7u array
            uint64_t glyph = 0;
            
            // Display null characters as blank
            if (charCode != 0) {
                for (size_t i = 0; i < Font5x7u_count; ++i) {
                    uint64_t entry = Font5x7u[i];
                    uint16_t glyphCode = (entry >> 48) & 0xFFFF;
                    if (glyphCode == charCode) {
                        glyph = entry;
                        break;
                    }
                }
            }
            
            int baseX = col * CHAR_WIDTH;
            int baseY = row * CHAR_HEIGHT;
            
            // Draw the 5x7 glyph in a 6x8 box
            // Start at bit 47 and read sequentially downward
            uint64_t rotatingBit = 0x0000800000000000ULL;  // bit 47
            
            for (int py = 0; py < CHAR_HEIGHT; py++) {
                for (int px = 0; px < CHAR_WIDTH; px++) {
                    int finalX = baseX + px;
                    int finalY = baseY + py;
                    
                    if (finalX < config.displayWidth && finalY < config.displayHeight) {
                        // Check if bit is set
                        bool bit = (glyph & rotatingBit) != 0;
                        
                        // Color based on character type
                        uint32_t color = 0xFF000000;  // Black background
                        if (bit) {
                            if (charCode >= '0' && charCode <= '9') {
                                color = 0xFF00FFFF;  // Cyan for numbers
                            } else if ((charCode >= 'A' && charCode <= 'Z') || 
                                     (charCode >= 'a' && charCode <= 'z')) {
                                color = 0xFF00FF00;  // Green for letters
                            } else if (charCode < 32 || charCode >= 127) {
                                color = 0xFFFF0080;  // Pink for control/extended
                            } else {
                                color = 0xFFFFFFFF;  // White for other printable
                            }
                        }
                        pixels[finalY * config.displayWidth + finalX] = color;
                    }
                    
                    rotatingBit >>= 1;  // Move to next bit
                }
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
    
    // Calculate layout
    int pixelsPerRow = config.width;
    int componentWidth = config.displayWidth / componentsPerPixel;
    
    for (int y = 0; y < config.height && y < config.displayHeight; y++) {
        for (int x = 0; x < pixelsPerRow && x < config.width; x++) {
            size_t offset = y * config.stride + x * bytesPerPixel;
            
            if (offset + bytesPerPixel <= dataSize) {
                // Extract components based on format
                uint8_t components[4] = {0, 0, 0, 255};
                
                switch (config.format.type) {
                    case PixelFormat::RGB888:
                        components[0] = data[offset];
                        components[1] = data[offset + 1];
                        components[2] = data[offset + 2];
                        break;
                    case PixelFormat::RGBA8888:
                        components[0] = data[offset];
                        components[1] = data[offset + 1];
                        components[2] = data[offset + 2];
                        components[3] = data[offset + 3];
                        break;
                    case PixelFormat::BGR888:
                        components[0] = data[offset + 2];
                        components[1] = data[offset + 1];
                        components[2] = data[offset];
                        break;
                    case PixelFormat::BGRA8888:
                        components[0] = data[offset + 2];
                        components[1] = data[offset + 1];
                        components[2] = data[offset];
                        components[3] = data[offset + 3];
                        break;
                    case PixelFormat::ARGB8888:
                        components[0] = data[offset + 1];
                        components[1] = data[offset + 2];
                        components[2] = data[offset + 3];
                        components[3] = data[offset];
                        break;
                    case PixelFormat::ABGR8888:
                        components[0] = data[offset + 3];
                        components[1] = data[offset + 2];
                        components[2] = data[offset + 1];
                        components[3] = data[offset];
                        break;
                    case PixelFormat::RGB565: {
                        uint16_t pixel = *(uint16_t*)(data + offset);
                        components[0] = ((pixel >> 11) & 0x1F) << 3;
                        components[1] = ((pixel >> 5) & 0x3F) << 2;
                        components[2] = (pixel & 0x1F) << 3;
                        break;
                    }
                }
                
                // Draw each component in its section
                for (int comp = 0; comp < componentsPerPixel; comp++) {
                    int destX = comp * componentWidth + (x % componentWidth);
                    int destY = y;
                    
                    if (destX < config.displayWidth && destY < config.displayHeight) {
                        uint32_t color = 0xFF000000;
                        if (comp == 0) color |= (components[0] << 16);  // Red channel
                        else if (comp == 1) color |= (components[1] << 8);  // Green channel
                        else if (comp == 2) color |= components[2];  // Blue channel
                        else if (comp == 3) {
                            // Alpha as grayscale
                            color |= (components[3] << 16) | (components[3] << 8) | components[3];
                        }
                        pixels[destY * config.displayWidth + destX] = color;
                    }
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