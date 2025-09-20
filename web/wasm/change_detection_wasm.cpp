// SIMD-optimized change detection functions for WebAssembly
// Uses 128-bit SIMD instructions for fast zero detection and checksums

#include <emscripten/emscripten.h>
#include <wasm_simd128.h>
#include <cstdint>
#include <cstring>

extern "C" {

// ============================================================================
// SIMD-Optimized Change Detection Functions
// ============================================================================

// Test if a memory chunk is all zeros using SIMD (16 bytes at a time)
EMSCRIPTEN_KEEPALIVE
bool testChunkZeroSIMD(uint8_t* data, int size) {
    // Handle alignment
    if (reinterpret_cast<uintptr_t>(data) % 16 != 0) {
        // Fall back to byte-by-byte for unaligned start
        int alignOffset = 16 - (reinterpret_cast<uintptr_t>(data) % 16);
        for (int i = 0; i < alignOffset && i < size; i++) {
            if (data[i] != 0) return false;
        }
        data += alignOffset;
        size -= alignOffset;
    }

    // Process 16-byte chunks with SIMD
    v128_t* ptr = reinterpret_cast<v128_t*>(data);
    int simdCount = size / 16;

    // Use OR accumulation for better performance
    v128_t accumulator = wasm_i32x4_const(0, 0, 0, 0);

    // Process in blocks of 8 for better pipelining
    int blockCount = simdCount / 8;
    for (int i = 0; i < blockCount; i++) {
        v128_t block = ptr[i * 8];
        block = wasm_v128_or(block, ptr[i * 8 + 1]);
        block = wasm_v128_or(block, ptr[i * 8 + 2]);
        block = wasm_v128_or(block, ptr[i * 8 + 3]);
        block = wasm_v128_or(block, ptr[i * 8 + 4]);
        block = wasm_v128_or(block, ptr[i * 8 + 5]);
        block = wasm_v128_or(block, ptr[i * 8 + 6]);
        block = wasm_v128_or(block, ptr[i * 8 + 7]);
        accumulator = wasm_v128_or(accumulator, block);
    }

    // Handle remaining SIMD chunks
    for (int i = blockCount * 8; i < simdCount; i++) {
        accumulator = wasm_v128_or(accumulator, ptr[i]);
    }

    // Check if accumulator is still zero
    uint64_t* result = reinterpret_cast<uint64_t*>(&accumulator);
    if (result[0] != 0 || result[1] != 0) return false;

    // Handle remaining bytes
    int remaining = size % 16;
    if (remaining > 0) {
        uint8_t* tail = reinterpret_cast<uint8_t*>(ptr + simdCount);
        for (int i = 0; i < remaining; i++) {
            if (tail[i] != 0) return false;
        }
    }

    return true;
}

// Calculate a fast checksum for a memory chunk using SIMD with rotation
EMSCRIPTEN_KEEPALIVE
uint32_t calculateChunkChecksumSIMD(uint8_t* data, int size) {
    // Initialize checksum components with prime constants
    v128_t checksum1 = wasm_i32x4_const(0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5);
    v128_t checksum2 = wasm_i32x4_const(0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5);

    // Handle alignment
    int offset = 0;
    if (reinterpret_cast<uintptr_t>(data) % 16 != 0) {
        int alignOffset = 16 - (reinterpret_cast<uintptr_t>(data) % 16);
        // Process unaligned bytes
        uint32_t unalignedHash = 0x9e3779b9; // Golden ratio constant
        for (int i = 0; i < alignOffset && i < size; i++) {
            unalignedHash = (unalignedHash << 5) | (unalignedHash >> 27); // Rotate left 5
            unalignedHash ^= data[i];
        }
        checksum1 = wasm_i32x4_replace_lane(checksum1, 0, unalignedHash);
        offset = alignOffset;
    }

    // Process aligned data with SIMD
    v128_t* ptr = reinterpret_cast<v128_t*>(data + offset);
    int simdCount = (size - offset) / 16;

    for (int i = 0; i < simdCount; i++) {
        v128_t chunk = ptr[i];

        // Mix with XOR
        checksum1 = wasm_v128_xor(checksum1, chunk);

        // Rotate checksum1 left by 5 bits for better mixing
        // WASM SIMD doesn't have rotate, so we use shift + or
        v128_t rotated = wasm_v128_or(
            wasm_i32x4_shl(checksum1, 5),
            wasm_u32x4_shr(checksum1, 27)
        );
        checksum1 = rotated;

        // Mix checksum2 with addition and rotation
        checksum2 = wasm_i32x4_add(checksum2, chunk);
        checksum2 = wasm_v128_xor(checksum2, checksum1);

        // Rotate checksum2 right by 13 bits
        checksum2 = wasm_v128_or(
            wasm_u32x4_shr(checksum2, 13),
            wasm_i32x4_shl(checksum2, 19)
        );
    }

    // Reduce to single 32-bit value
    uint32_t* lanes1 = reinterpret_cast<uint32_t*>(&checksum1);
    uint32_t* lanes2 = reinterpret_cast<uint32_t*>(&checksum2);

    uint32_t result = lanes1[0] ^ lanes1[1] ^ lanes1[2] ^ lanes1[3];
    result ^= lanes2[0] ^ lanes2[1] ^ lanes2[2] ^ lanes2[3];

    // Mix in any remaining bytes
    int remaining = (size - offset) % 16;
    if (remaining > 0) {
        uint8_t* tail = reinterpret_cast<uint8_t*>(ptr + simdCount);
        for (int i = 0; i < remaining; i++) {
            result = (result << 7) | (result >> 25); // Rotate
            result ^= tail[i];
        }
    }

    // Final mixing for avalanche effect
    result ^= result >> 16;
    result *= 0x85ebca6b;
    result ^= result >> 13;
    result *= 0xc2b2ae35;
    result ^= result >> 16;

    return result;
}

// Test a single 4KB page for zero
EMSCRIPTEN_KEEPALIVE
bool testPageZero(uint8_t* data) {
    return testChunkZeroSIMD(data, 4096);
}

// Calculate checksum for a single 4KB page
EMSCRIPTEN_KEEPALIVE
uint32_t calculatePageChecksum(uint8_t* data) {
    return calculateChunkChecksumSIMD(data, 4096);
}

// Test a 64KB chunk for zero
EMSCRIPTEN_KEEPALIVE
bool testChunk64KZero(uint8_t* data) {
    return testChunkZeroSIMD(data, 65536);
}

// Calculate checksum for a 64KB chunk
EMSCRIPTEN_KEEPALIVE
uint32_t calculateChunk64KChecksum(uint8_t* data) {
    return calculateChunkChecksumSIMD(data, 65536);
}

// Test a 1MB chunk for zero
EMSCRIPTEN_KEEPALIVE
bool testChunk1MBZero(uint8_t* data) {
    return testChunkZeroSIMD(data, 1048576);
}

// Calculate checksum for a 1MB chunk
EMSCRIPTEN_KEEPALIVE
uint32_t calculateChunk1MBChecksum(uint8_t* data) {
    return calculateChunkChecksumSIMD(data, 1048576);
}

// Memory allocation helpers
EMSCRIPTEN_KEEPALIVE
uint8_t* allocateBuffer(size_t size) {
    return new uint8_t[size];
}

EMSCRIPTEN_KEEPALIVE
void freeBuffer(uint8_t* ptr) {
    delete[] ptr;
}

} // extern "C"