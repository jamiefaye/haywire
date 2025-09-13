#pragma once

#include <cstdint>
#include "font5x7u.h"

namespace Haywire {

// 3x5 font for hex digits (0-9, A-F)
// Each character is encoded in 16 bits (octal format from the example)
// Layout: 3 pixels wide, 5 pixels tall
static const uint16_t Font3x5Hex[] = {
    025552, // 0 (octal)
    026222, // 1 (octal)
    071347, // 2 (octal)
    071717, // 3 (octal)
    055711, // 4 (octal)
    074716, // 5 (octal)
    024757, // 6 (octal)
    071244, // 7 (octal)
    075757, // 8 (octal)
    075711, // 9 (octal)
    025755, // A (octal)
    065656, // B (octal)
    034443, // C (octal)
    065556, // D (octal)
    074647, // E (octal)
    074744  // F (octal)
};

// 5x7 font glyphs packed in 64-bit values
// Format: glyph is 5x7 pixels in a 6x8 box
// Bits 40-47 contain 6 pixels of row 0, bits 34-39 contain row 1, etc.
#define GLYPH57(b0,b1,b2,b3,b4,b5,b6) \
    ((uint64_t(b0) << 40) | (uint64_t(b1) << 34) | (uint64_t(b2) << 28) | \
     (uint64_t(b3) << 22) | (uint64_t(b4) << 16) | (uint64_t(b5) << 10) | \
     (uint64_t(b6) << 4))




// Helper function to get a 3x5 hex font glyph
inline uint16_t GetGlyph3x5Hex(unsigned char nibble) {
    if (nibble < 16) {
        return Font3x5Hex[nibble];
    }
    return 0;
}

// Calculate high contrast opposite color
inline uint32_t CalcHiContrastOpposite(uint32_t color) {
    uint8_t r = (color >> 0) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = (color >> 16) & 0xFF;
    
    r = (r < 127) ? 255 : 0;
    g = (g < 127) ? 255 : 0;
    b = (b < 127) ? 255 : 0;
    
    return 0xFF000000 | (b << 16) | (g << 8) | r;
}

} // namespace Haywire