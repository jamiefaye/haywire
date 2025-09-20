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

    // Validate source offset
    if (sourceOffset >= memorySize) {
        // No data to render
        return;
    }

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

// ============================================================================
// Change Detection Functions - SIMD optimized memory scanning
// ============================================================================

#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#endif

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Test if a chunk is all zeros using SIMD
EMSCRIPTEN_KEEPALIVE
int testChunkZeroSIMD(const uint8_t* data, size_t size) {
    if (!data || size == 0) return 1;

    // Handle unaligned start
    size_t offset = 0;
    while ((reinterpret_cast<uintptr_t>(data + offset) & 15) && offset < size) {
        if (data[offset] != 0) return 0;
        offset++;
    }

    // SIMD processing - 16 bytes at a time
    v128_t zero = wasm_i32x4_const(0, 0, 0, 0);
    v128_t accumulator = zero;

    size_t simd_end = offset + ((size - offset) & ~15);
    for (size_t i = offset; i < simd_end; i += 16) {
        v128_t chunk = wasm_v128_load(data + i);
        accumulator = wasm_v128_or(accumulator, chunk);
    }

    // Check if accumulator is still zero
    if (wasm_i32x4_extract_lane(accumulator, 0) != 0 ||
        wasm_i32x4_extract_lane(accumulator, 1) != 0 ||
        wasm_i32x4_extract_lane(accumulator, 2) != 0 ||
        wasm_i32x4_extract_lane(accumulator, 3) != 0) {
        return 0;
    }

    // Handle remaining bytes
    for (size_t i = simd_end; i < size; i++) {
        if (data[i] != 0) return 0;
    }

    return 1;
}

// Calculate checksum using SIMD with rotation for mixing
EMSCRIPTEN_KEEPALIVE
uint32_t calculateChunkChecksumSIMD(const uint8_t* data, size_t size) {
    if (!data || size == 0) return 0;

    // Handle unaligned start
    size_t offset = 0;
    uint32_t scalar_sum = 0x9e3779b9;  // Golden ratio for better mixing

    while ((reinterpret_cast<uintptr_t>(data + offset) & 15) && offset < size) {
        scalar_sum = ((scalar_sum << 5) | (scalar_sum >> 27)) ^ data[offset];
        offset++;
    }

    // SIMD processing - initialize with constants and then update
    v128_t checksum1 = wasm_i32x4_const(0x9e3779b9, 0x517cc1b7, 0x27220a95, 0x2b885d7e);
    v128_t checksum2 = wasm_i32x4_const(0x5b2c5926, 0x7119f859, 0xa4426e90, 0x1edc6f25);

    // Update first lane with scalar_sum if we processed unaligned data
    if (offset > 0) {
        checksum1 = wasm_i32x4_replace_lane(checksum1, 0, scalar_sum);
    }

    size_t simd_end = offset + ((size - offset) & ~15);
    for (size_t i = offset; i < simd_end; i += 16) {
        v128_t chunk = wasm_v128_load(data + i);

        // Rotate checksum1 left by 5
        v128_t rotated = wasm_v128_or(
            wasm_i32x4_shl(checksum1, 5),
            wasm_u32x4_shr(checksum1, 27)
        );

        checksum1 = wasm_v128_xor(rotated, chunk);

        // Mix with checksum2 using different rotation
        checksum2 = wasm_v128_xor(
            wasm_v128_or(
                wasm_i32x4_shl(checksum2, 13),
                wasm_u32x4_shr(checksum2, 19)
            ),
            chunk
        );
    }

    // Combine the checksums
    uint32_t result = wasm_i32x4_extract_lane(checksum1, 0) ^
                      wasm_i32x4_extract_lane(checksum1, 1) ^
                      wasm_i32x4_extract_lane(checksum1, 2) ^
                      wasm_i32x4_extract_lane(checksum1, 3) ^
                      wasm_i32x4_extract_lane(checksum2, 0) ^
                      wasm_i32x4_extract_lane(checksum2, 1) ^
                      wasm_i32x4_extract_lane(checksum2, 2) ^
                      wasm_i32x4_extract_lane(checksum2, 3);

    // Handle remaining bytes
    for (size_t i = simd_end; i < size; i++) {
        result = ((result << 7) | (result >> 25)) ^ data[i];
    }

    // Final mixing
    result ^= result >> 16;
    result *= 0x85ebca6b;
    result ^= result >> 13;
    result *= 0xc2b2ae35;
    result ^= result >> 16;

    return result;
}


// Optimized functions for specific sizes
EMSCRIPTEN_KEEPALIVE
int testPageZero(const uint8_t* data) {
    return testChunkZeroSIMD(data, 4096);
}

EMSCRIPTEN_KEEPALIVE
uint32_t calculatePageChecksum(const uint8_t* data) {
    return calculateChunkChecksumSIMD(data, 4096);
}

EMSCRIPTEN_KEEPALIVE
int testChunk64KZero(const uint8_t* data) {
    return testChunkZeroSIMD(data, 65536);
}

EMSCRIPTEN_KEEPALIVE
uint32_t calculateChunk64KChecksum(const uint8_t* data) {
    return calculateChunkChecksumSIMD(data, 65536);
}

EMSCRIPTEN_KEEPALIVE
int testChunk1MBZero(const uint8_t* data) {
    return testChunkZeroSIMD(data, 1048576);
}

EMSCRIPTEN_KEEPALIVE
uint32_t calculateChunk1MBChecksum(const uint8_t* data) {
    return calculateChunkChecksumSIMD(data, 1048576);
}

// ============================================================================
// FFT Auto-Correlator Functions - Based on C++ autocorrelator
// ============================================================================

// Cooley-Tukey FFT implementation
static void fft_internal(float* real, float* imag, int n, bool inverse) {
    if (n <= 1) return;

    // Bit reversal
    int j = 0;
    for (int i = 1; i < n - 1; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;

        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    // Cooley-Tukey NTT
    for (int len = 2; len <= n; len <<= 1) {
        float angle = 2 * M_PI / len * (inverse ? 1 : -1);
        float wlen_real = cosf(angle);
        float wlen_imag = sinf(angle);

        for (int i = 0; i < n; i += len) {
            float w_real = 1;
            float w_imag = 0;
            for (int j = 0; j < len / 2; j++) {
                float u_real = real[i + j];
                float u_imag = imag[i + j];
                float v_real = real[i + j + len/2] * w_real - imag[i + j + len/2] * w_imag;
                float v_imag = real[i + j + len/2] * w_imag + imag[i + j + len/2] * w_real;

                real[i + j] = u_real + v_real;
                imag[i + j] = u_imag + v_imag;
                real[i + j + len/2] = u_real - v_real;
                imag[i + j + len/2] = u_imag - v_imag;

                float w_temp = w_real;
                w_real = w_real * wlen_real - w_imag * wlen_imag;
                w_imag = w_temp * wlen_imag + w_imag * wlen_real;
            }
        }
    }

    // Scale for inverse transform
    if (inverse) {
        float scale = 1.0f / n;
        for (int i = 0; i < n; i++) {
            real[i] *= scale;
            imag[i] *= scale;
        }
    }
}

// Auto-correlation using FFT
EMSCRIPTEN_KEEPALIVE
void autoCorrelate(const uint8_t* data, int dataSize, float* output, int outputSize) {
    // Find next power of 2 >= 2 * dataSize for zero padding
    int fftSize = 1;
    while (fftSize < dataSize * 2) {
        fftSize <<= 1;
    }

    // Limit FFT size to something reasonable (16384 like the C++ version)
    if (fftSize > 16384) fftSize = 16384;

    // Allocate working arrays
    float* real = (float*)calloc(fftSize, sizeof(float));
    float* imag = (float*)calloc(fftSize, sizeof(float));

    if (!real || !imag) {
        free(real);
        free(imag);
        return;
    }

    // Calculate mean for DC removal
    float sum = 0;
    for (int i = 0; i < dataSize; i++) {
        sum += data[i];
    }
    float mean = sum / dataSize;

    // Copy data with DC removal (mean subtraction)
    int sampleCount = (dataSize < fftSize) ? dataSize : fftSize;
    for (int i = 0; i < sampleCount; i++) {
        real[i] = data[i] - mean;
    }

    // Forward FFT
    fft_internal(real, imag, fftSize, false);

    // Compute power spectrum (magnitude squared)
    for (int i = 0; i < fftSize; i++) {
        float magnitude = sqrtf(real[i] * real[i] + imag[i] * imag[i]);
        real[i] = magnitude / fftSize;
        imag[i] = 0;
    }

    // Inverse FFT to get autocorrelation
    fft_internal(real, imag, fftSize, true);

    // Normalize by the zero-lag value (peak)
    float maxVal = real[0];
    if (maxVal == 0) maxVal = 1.0f;

    // Copy normalized results to output
    int copySize = (outputSize < fftSize) ? outputSize : fftSize;
    for (int i = 0; i < copySize; i++) {
        output[i] = real[i] / maxVal;
    }

    free(real);
    free(imag);
}

// Find repeating patterns using autocorrelation peaks
EMSCRIPTEN_KEEPALIVE
int findRepeatPeriod(const uint8_t* data, int dataSize, float threshold) {
    const int correlationSize = 2048;
    float* correlation = (float*)calloc(correlationSize, sizeof(float));

    if (!correlation) return -1;

    // Compute autocorrelation
    autoCorrelate(data, dataSize, correlation, correlationSize);

    // Find first significant peak after lag 16 (skip small periodicities)
    int bestPeriod = -1;
    for (int lag = 16; lag < correlationSize - 1; lag++) {
        // Check for local maximum above threshold
        if (correlation[lag] > threshold &&
            correlation[lag] > correlation[lag-1] &&
            correlation[lag] > correlation[lag+1]) {
            bestPeriod = lag;
            break;
        }
    }

    free(correlation);
    return bestPeriod;
}

// Get autocorrelation peaks for visualization
EMSCRIPTEN_KEEPALIVE
void getCorrelationPeaks(const uint8_t* data, int dataSize, int* peaks, int maxPeaks, float threshold) {
    const int correlationSize = 2048;
    float* correlation = (float*)calloc(correlationSize, sizeof(float));

    if (!correlation) {
        for (int i = 0; i < maxPeaks; i++) peaks[i] = -1;
        return;
    }

    // Compute autocorrelation
    autoCorrelate(data, dataSize, correlation, correlationSize);

    // Find peaks
    int peakCount = 0;
    for (int lag = 16; lag < correlationSize - 1 && peakCount < maxPeaks; lag++) {
        // Check for local maximum above threshold
        if (correlation[lag] > threshold &&
            correlation[lag] > correlation[lag-1] &&
            correlation[lag] > correlation[lag+1]) {
            peaks[peakCount++] = lag;
            // Skip nearby points to avoid duplicate peaks
            lag += 8;
        }
    }

    // Fill remaining slots with -1
    for (int i = peakCount; i < maxPeaks; i++) {
        peaks[i] = -1;
    }

    free(correlation);
}

// Memory allocation helpers for FFT
EMSCRIPTEN_KEEPALIVE
float* allocateFloatBuffer(size_t count) {
    return (float*)malloc(count * sizeof(float));
}

EMSCRIPTEN_KEEPALIVE
void freeFloatBuffer(float* ptr) {
    free(ptr);
}

} // extern "C"