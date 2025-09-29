#!/bin/bash

# Fix App.vue unused variables
echo "Fixing App.vue unused variables..."

# Remove unused imports and declarations
sed -i '' '/const isLoadingChangeDetection =/d' src/App.vue
sed -i '' '/const changeDetectionError =/d' src/App.vue
sed -i '' '/const scanMemory =/d' src/App.vue
sed -i '' '/const getChunkAtOffset =/d' src/App.vue
sed -i '' '/const lastScanTime =/d' src/App.vue
sed -i '' '/const formatName =/d' src/App.vue
sed -i '' '/const onSliderChange =/d' src/App.vue
sed -i '' '/const tooltipText =/d' src/App.vue
sed -i '' '/const clearFFTSamplePoint =/d' src/App.vue
sed -i '' '/const sizeKB = /d' src/App.vue
sed -i '' '/PixelFormatExport/d' src/App.vue

# Fix function parameters - comment out unused ones
sed -i '' 's/function onFFTMouseMove(event)/function onFFTMouseMove()/g' src/App.vue
sed -i '' 's/({ path, size })/({ size })/g' src/App.vue
sed -i '' 's/const delta =/\/\/ const delta =/g' src/App.vue

# Fix component unused imports
sed -i '' 's/import { .*, onUnmounted } from/import { ref, computed, onMounted, watch } from/g' src/components/AutoCorrelator.vue
sed -i '' '/const emit =/d' src/components/AutoCorrelator.vue
sed -i '' '/const threshold =/d' src/components/AutoCorrelator.vue

sed -i '' 's/import { .*, onMounted } from/import { ref, computed, watch, nextTick } from/g' src/components/KernelDiscoveryReport.vue

sed -i '' '/const isDragging =/d' src/components/MagnifyingGlass.vue

echo "Done fixing unused variables"