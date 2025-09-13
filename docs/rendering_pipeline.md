# Haywire Memory Rendering Pipeline

## Overview

The Haywire memory visualizer uses a unified rendering pipeline that can display memory in two modes:
- **Linear Mode**: Traditional row-by-row memory layout
- **Column Mode**: Multi-column layout where memory flows vertically down each column

## Architecture

### Key Components

1. **MemoryVisualizer** (`memory_visualizer.cpp/h`)
   - Main UI and control logic
   - Manages viewport settings (base address, width, height, format)
   - Handles user input and navigation
   - Coordinates between all subsystems

2. **MemoryRenderer** (`memory_renderer.cpp/h`)
   - Unified rendering engine for both main view and mini viewers
   - Converts raw memory bytes to pixels based on format
   - Implements column mode layout transformation

3. **RenderConfig** (`memory_renderer.h`)
   - Configuration structure passed to renderer
   - Contains display dimensions, format, and column mode settings
   - All dimensions in pixels for consistency

## Rendering Pipeline

### Step 1: Memory Reading
```
QemuConnection/CrunchedMemoryReader
    ↓
MemoryBlock (raw bytes + metadata)
```

### Step 2: Configuration
```cpp
RenderConfig config;
config.displayWidth = viewport.width;      // Display dimensions in pixels
config.displayHeight = viewport.height;
config.format = viewport.format;           // Pixel format (RGB888, etc.)
config.columnMode = columnMode;            // Enable/disable column mode
config.columnWidth = columnWidth;          // Column width in pixels
config.columnGap = columnGap;              // Gap between columns in pixels
```

### Step 3: Memory-to-Pixel Transformation

The renderer converts memory bytes to display pixels using one of two layout modes:

#### Linear Mode (Traditional)
```
Memory Layout:
[Row 0: bytes 0-255    ]
[Row 1: bytes 256-511  ]
[Row 2: bytes 512-767  ]
...

Formula:
offset = y * stride + x * bytesPerPixel
```

#### Column Mode
```
Display Layout:
[Col 0]  [Gap]  [Col 1]  [Gap]  [Col 2]
  ↓              ↓              ↓
  ↓              ↓              ↓
  ↓              ↓              ↓

Memory flows vertically down each column,
then continues at the top of the next column.

Formula:
bytesPerColumn = displayHeight * columnWidth * bytesPerPixel
columnIndex = x / (columnWidth + columnGap)
xInColumn = x % (columnWidth + columnGap)
if (xInColumn >= columnWidth) return -1  // In gap

offset = columnIndex * bytesPerColumn +
         y * columnWidth * bytesPerPixel +
         xInColumn * bytesPerPixel
```

### Step 4: Pixel Format Conversion

The renderer supports multiple pixel formats:
- **RGB888/BGR888**: 3 bytes per pixel
- **RGBA8888/BGRA8888/ARGB8888/ABGR8888**: 4 bytes per pixel
- **RGB565**: 2 bytes per pixel (16-bit color)
- **Grayscale**: 1 byte per pixel
- **Binary**: 1 bit per pixel (8 pixels per byte)
- **Hex Pixel**: 4 bytes displayed as 8 hex digits
- **Char 8-bit**: 1 byte displayed as ASCII character glyph
- **Split Components**: RGB/RGBA channels displayed separately

### Step 5: Display
```
Pixel Buffer (RGBA8888)
    ↓
OpenGL Texture
    ↓
ImGui Image
```

## Coordinate System Transformations

### Display → Memory Address

All components use consistent transformation logic:

```cpp
// Linear Mode
offset = (y * stride + x) * bytesPerPixel
address = viewport.baseAddress + offset

// Column Mode
col = x / (columnWidth + columnGap)
xInColumn = x % (columnWidth + columnGap)
if (xInColumn >= columnWidth) {
    // In gap - snap to next column
    col++; xInColumn = 0;
}
bytesPerColumn = height * columnWidth * bytesPerPixel
offset = col * bytesPerColumn +
         y * columnWidth * bytesPerPixel +
         xInColumn * bytesPerPixel
address = viewport.baseAddress + offset
```

### Memory Offset → Display Position

Used for change tracking and anchor positioning:

```cpp
// Linear Mode
x = (offset % strideBytes) / bytesPerPixel
y = offset / strideBytes

// Column Mode
bytesPerColumn = height * columnWidth * bytesPerPixel
col = offset / bytesPerColumn
posInColumn = offset % bytesPerColumn
y = posInColumn / (columnWidth * bytesPerPixel)
xInColumn = (posInColumn % (columnWidth * bytesPerPixel)) / bytesPerPixel
x = col * (columnWidth + columnGap) + xInColumn
```

## Usage Examples

### Enabling Column Mode

```cpp
// In UI (memory_visualizer.cpp)
if (ImGui::Checkbox("Columns", &columnMode)) {
    needsUpdate = true;
}

// Column width control (in pixels, same units as main width)
if (ImGui::InputInt("##ColWidth", &columnWidth)) {
    columnWidth = std::max(1, columnWidth);
    needsUpdate = true;
}
```

### Rendering Memory

```cpp
// Create render configuration
RenderConfig config;
config.displayWidth = 1024;         // Display width in pixels
config.displayHeight = 768;         // Display height in pixels
config.format = PixelFormat::RGB888;
config.columnMode = true;           // Enable columns
config.columnWidth = 256;           // 256 pixels per column
config.columnGap = 8;               // 8 pixel gap

// Render memory to pixels
std::vector<uint32_t> pixels = MemoryRenderer::RenderMemory(
    data, dataSize, config
);
```

## Synchronized Components

The following components all use the same coordinate transformation logic:

1. **Main Memory Renderer** - Displays the memory bitmap
2. **Mouse Hover/Click Handler** - Shows correct address under cursor
3. **Change Tracker** - Highlights modified memory regions with yellow boxes
4. **Mini Bitmap Viewers** - Anchor points stay at clicked positions
5. **Address Navigation** - Jump to address works correctly in both modes
6. **Search Results** - Highlights appear at correct positions

## Key Design Decisions

### Units Consistency
All width values use **pixels** as the unit:
- Main viewport width: pixels
- Column width: pixels (not bytes)
- Stride: pixels (multiplied by bytesPerPixel internally)

This prevents confusion and ensures consistent behavior when changing pixel formats.

### Column Height
Column height automatically matches the display window height. This simplifies the interface and makes column mode more intuitive - memory flows down the full height of each column before wrapping to the next.

### Stride Handling
In column mode, stride is automatically set to column width. This ensures proper alignment within each column without user configuration.

### Gap Behavior
Clicking in the gap between columns snaps to the start of the next column. This provides predictable behavior for address selection and mini-viewer placement.

## Performance Considerations

- Column mode requires additional calculations per pixel but performance impact is minimal
- The renderer processes only visible memory, not the entire address space
- Texture updates happen on demand when memory or settings change
- Change tracking uses efficient byte comparison with region merging

## Future Enhancements

Potential improvements to the rendering pipeline:
- Hardware acceleration for format conversions
- Cached rendering for static memory regions
- Progressive rendering for very large memory areas
- Custom shaders for specialized visualizations
- Multi-resolution rendering (overview + detail)