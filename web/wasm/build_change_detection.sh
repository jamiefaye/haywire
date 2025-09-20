#!/bin/bash

# Build script for compiling change detection to WebAssembly with SIMD
# Requires Emscripten (emcc) to be installed and in PATH

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
OUTPUT_DIR="$SCRIPT_DIR"

# Create output directory
mkdir -p "$OUTPUT_DIR"

echo "Building change_detection.wasm with SIMD optimization..."

# Compile with Emscripten and SIMD enabled
emcc -O3 \
    "$SCRIPT_DIR/change_detection_wasm.cpp" \
    -o "$OUTPUT_DIR/change_detection.js" \
    -msimd128 \
    -s EXPORTED_FUNCTIONS="[\
        '_testChunkZeroSIMD',\
        '_calculateChunkChecksumSIMD',\
        '_testPageZero',\
        '_calculatePageChecksum',\
        '_testChunk64KZero',\
        '_calculateChunk64KChecksum',\
        '_testChunk1MBZero',\
        '_calculateChunk1MBChecksum',\
        '_allocateBuffer',\
        '_freeBuffer'\
    ]" \
    -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','HEAP8','HEAPU8','HEAP32','HEAPU32']" \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MODULARIZE=1 \
    -s EXPORT_NAME="ChangeDetectionModule" \
    -s ENVIRONMENT="web" \
    -s SINGLE_FILE=1 \
    --no-entry

echo "Build complete! Output in $OUTPUT_DIR"
echo "Files generated:"
ls -la "$OUTPUT_DIR"/change_detection.*