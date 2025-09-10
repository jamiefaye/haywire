# Mini Bitmap Viewers Design Document

## Overview
Mini bitmap viewers are lightweight, draggable overlay windows that extract and display specific memory regions as bitmaps. They provide focused visualization of embedded images, data structures, or patterns within the main memory view.

## Visual Design

```
    ╱─────────────╲  <- Leader line (draggable)
   ╱               
  ○ ← Anchor point (drag to reposition)
  │
┌─[▤ Viewer 1 ⚙]──┐  <- Title bar: grip icon, name, settings button
│                 │
│   [Extracted    │     <- Live bitmap display
│    Bitmap]      │
│                 │
│               ○ │  <- Resize handle ("birdie")
└─────────────────┘
```

## Core Components

### 1. BitmapViewer Structure
```cpp
struct BitmapViewer {
    // Identification
    int id;                    // Unique viewer ID
    std::string name;          // User-defined name
    bool active;               // Enabled/disabled state
    
    // Position and Layout
    ImVec2 windowPos;          // Window position on screen
    ImVec2 windowSize;         // Current window size
    ImVec2 anchorPos;          // Leader line anchor in memory view
    ImVec2 leaderOffset;       // Offset from window to anchor
    
    // Memory Configuration
    uint64_t memoryAddress;    // Base address to read from
    int memWidth;              // Width in bytes
    int memHeight;             // Height in rows
    int stride;                // Bytes per row (can be > width)
    PixelFormat format;        // Current pixel format
    
    // Rendering
    GLuint texture;            // OpenGL texture handle
    std::vector<uint32_t> pixels;  // Extracted pixel data
    bool needsUpdate;          // Refresh flag
    
    // Interaction State
    bool isDragging;           // Window being moved
    bool isResizing;           // Size being adjusted
    bool isDraggingLeader;     // Anchor being repositioned
    bool showSettings;         // Settings popup visible
    bool showLeader;           // Leader line visible
    
    // Constraints
    bool windowPinned;         // Lock window position
    bool anchorPinned;         // Lock anchor position  
    bool sizePinned;           // Lock dimensions
    bool constrainAspect;      // Maintain aspect ratio
    bool snapToGrid;           // Snap to boundaries
    int gridSize;              // Grid snap size (8, 16, 32)
};
```

### 2. BitmapViewerManager
Manages collection of viewers and selection state:
- Track selected viewer for keyboard input
- Handle viewer creation/deletion
- Manage render order (z-ordering)
- Save/load viewer configurations

## Supported Pixel Formats

All formats from main viewer plus:
1. **RGB888** - 3 bytes per pixel
2. **RGBA8888** - 4 bytes per pixel  
3. **BGR888** - Windows bitmap order
4. **BGRA8888** - Windows native
5. **ARGB8888** - Mac native
6. **ABGR8888** - Reverse ARGB
7. **RGB565** - 16-bit color
8. **GRAYSCALE** - 1 byte per pixel
9. **BINARY** - 1 bit per pixel
10. **HEX_PIXEL** - 32-bit values as hex digits (32x6 pixels each)
11. **CHAR_8BIT** - ASCII/extended characters (6x8 pixels each)

## Interaction Model

### Creation
- **Right-click menu**: "Create Bitmap Viewer Here"
- **Hotkey**: Ctrl+B at mouse position
- Initial size: Auto-detect or use defaults (256x256)

### Selection
- **Click**: Select viewer (yellow border)
- **Tab/Shift+Tab**: Cycle through viewers
- **Number keys 1-9**: Quick select by ID
- **Escape**: Deselect all

### Movement and Sizing

#### Window Dragging
- **Drag title bar**: Move window
- **P key**: Toggle position pin

#### Anchor Dragging  
- **Drag anchor point**: Reposition in memory
- **Shift+drag**: Constrain to horizontal/vertical
- **Ctrl+drag**: Snap to 16-byte boundaries
- **Alt+drag**: Snap to stride multiples
- **L key**: Toggle anchor lock

#### Resizing
- **Drag resize handle**: Adjust window size
- **Shift+drag**: Maintain aspect ratio
- **Ctrl+drag**: Snap to powers of 2
- **Alt+drag**: Snap to common dimensions
- **S key**: Toggle size lock

### Keyboard Controls (Selected Viewer)

#### Navigation
- **Arrow keys**: Move anchor by 1 byte
- **Shift+Arrow**: Move by stride
- **Ctrl+Arrow**: Move by 16 bytes  
- **Page Up/Down**: Move by viewport height

#### Format Cycling
- **Tab/Shift+Tab**: Next/previous pixel format
- **F key**: Quick format menu

#### Dimension Adjustment
- **+/- keys**: Increase/decrease width
- **Shift +/-**: Increase/decrease height
- **Ctrl +/-**: Increase/decrease stride
- **Alt +/-**: Snap to common sizes

#### Other Controls
- **G**: Toggle grid snapping
- **R**: Reset to auto-detected dimensions
- **D**: Duplicate current viewer
- **Delete**: Remove viewer
- **Ctrl+S**: Export viewer to PNG

### Common Dimension Presets
```cpp
const int commonWidths[] = {8, 16, 32, 64, 128, 256, 320, 512, 640, 800, 1024, 1280, 1920};
const int commonHeights[] = {8, 16, 32, 64, 128, 240, 256, 480, 512, 600, 768, 1024, 1080};  
const int commonStrides[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
```

## Visual Feedback

### Selection States
- **Selected**: Yellow border, pulsing effect, star (★) in title
- **Hovered**: Light gray border
- **Default**: Dark gray border
- **Pinned elements**: Red color indicators

### Constraint Indicators
- **Shift constraint**: Show H/V guide lines
- **Grid snapping**: Show grid overlay
- **Stride alignment**: Highlight stride boundaries

### Leader Line
- **Normal**: Yellow semi-transparent line
- **Anchor hover**: Brighten and thicken
- **Pinned**: Red line color

## Integration Features

### FFT/Correlation Display
When a viewer is selected:
- Show width as solid yellow line on FFT
- Show stride as dashed orange line
- Display harmonics (2x, 3x, 4x) as faint lines
- Label with "W:### S:###" values

### Memory View Integration
- Highlight source region in main view
- Show address at anchor point tooltip
- Synchronize with main view scrolling (optional)

### Status Bar
Display selected viewer info:
- "Viewer N: WxH @ 0xADDRESS (Stride: S, Format: FMT)"

## Settings Popup

Activated by gear (⚙) button:
```
┌─ Viewer Settings ─────┐
│ Width:  [256]    ▲▼   │
│ Height: [256]    ▲▼   │  
│ Stride: [256]    ▲▼   │
│ Format: [RGB888] ▼    │
│ ─────────────────     │
│ □ Show leader line    │
│ □ Snap to grid        │
│ □ Lock aspect ratio   │
│ □ Sync with main view │
│ ─────────────────     │
│ [Export PNG] [Delete] │
└───────────────────────┘
```

## Memory Reading

Viewers read memory through the same mechanism as main view:
- Physical memory mode: Direct read from memory-mapped file
- Virtual memory mode: Use CrunchedMemoryReader with VA→PA translation
- Respect current viewport's read mode and connection

## Performance Considerations

1. **Texture Caching**: Only update texture when needed
2. **Lazy Updates**: Update on-demand, not every frame
3. **Size Limits**: Cap maximum dimensions (e.g., 2048x2048)
4. **Viewer Limit**: Maximum 16 concurrent viewers
5. **Efficient Memory Reads**: Batch reads where possible

## Persistence

Viewer configurations can be saved/loaded:
```json
{
  "viewers": [
    {
      "id": 1,
      "name": "Framebuffer",
      "address": "0x7f8000000",
      "width": 1920,
      "height": 1080,
      "stride": 1920,
      "format": "RGBA8888",
      "position": [100, 100],
      "size": [400, 300]
    }
  ]
}
```

## Future Enhancements

1. **Pattern Detection**: Auto-detect image dimensions
2. **Animation**: Play through memory as frames
3. **Comparison**: Side-by-side viewer comparison
4. **Filters**: Apply image processing filters
5. **Measurement**: Ruler/measurement tools
6. **Annotations**: Add labels and notes to viewers