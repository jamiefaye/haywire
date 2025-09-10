#pragma once

#include <cstdint>

namespace Haywire {

// 5x7 Unicode font - adapted from FreeType/Bitstream
// Glyphs are 5x7 in a 6x8 box
// 64-bit packed format with character code in top 16 bits
#define DEFCHAR57(nm,cc,b0,b1,b2,b3,b4,b5,b6) ((uint64_t(cc) << 48)\
| (uint64_t(b0) <<  40) | (uint64_t(b1) << 34) | (uint64_t(b2) << 28) | (uint64_t(b3) << 22) | (uint64_t(b4) << 16)\
| (uint64_t(b5) <<  10) | (uint64_t(b6) << 4))

static const uint64_t Font5x7u[] = {
#include "font5x7u_array.inc"
};

static const size_t Font5x7u_count = sizeof(Font5x7u) / sizeof(Font5x7u[0]);

} // namespace Haywire