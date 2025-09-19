#!/bin/bash

# Build script for compiling memory_renderer to WebAssembly
# Requires Emscripten (emcc) to be installed and in PATH

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$SCRIPT_DIR/../.."
OUTPUT_DIR="$SCRIPT_DIR/../public/wasm"

# Create output directory
mkdir -p "$OUTPUT_DIR"

echo "Building memory_renderer.wasm..."

# Compile with Emscripten
# Note: We include the actual renderer source files, not reimplementing them
emcc -O3 \
    -I"$PROJECT_ROOT/include" \
    "$PROJECT_ROOT/src/memory_renderer.cpp" \
    "$SCRIPT_DIR/memory_renderer_wasm.cpp" \
    -o "$OUTPUT_DIR/memory_renderer.js" \
    -s EXPORTED_FUNCTIONS="[\
        '_initRenderer',\
        '_cleanupRenderer',\
        '_renderMemoryToCanvas',\
        '_getFormatBytesPerPixel',\
        '_getExtendedFormat',\
        '_getFormatDescriptor',\
        '_pixelToMemoryCoordinate',\
        '_allocateMemory',\
        '_allocatePixelBuffer',\
        '_freeMemory'\
    ]" \
    -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','HEAP8','HEAPU8','HEAP32','HEAPU32']" \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MODULARIZE=1 \
    -s EXPORT_NAME="MemoryRendererModule" \
    -s ENVIRONMENT="web" \
    -s SINGLE_FILE=1 \
    --no-entry

echo "Build complete! Output in $OUTPUT_DIR"
echo "Files generated:"
ls -la "$OUTPUT_DIR"/memory_renderer.*