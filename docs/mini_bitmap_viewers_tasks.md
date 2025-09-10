# Mini Bitmap Viewers - Implementation Task List

## Phase 1: Core Infrastructure (Foundation)

### 1.1 Data Structures
- [ ] Create `BitmapViewer` struct in `include/bitmap_viewer.h`
- [ ] Create `BitmapViewerManager` class
- [ ] Add viewer collection to `MemoryVisualizer` class
- [ ] Define common dimension arrays and format cycling

### 1.2 Basic Window Rendering
- [ ] Implement basic window drawing with ImGui/OpenGL
- [ ] Add title bar with drag functionality
- [ ] Add resize handle ("birdie") in corner
- [ ] Implement window border with selection states

### 1.3 Memory Extraction
- [ ] Create memory extraction method for viewer
- [ ] Support both PA and VA reading modes
- [ ] Implement texture creation from extracted memory
- [ ] Add pixel format conversion for all formats

## Phase 2: Leader Line and Anchor System

### 2.1 Leader Line Rendering
- [ ] Draw leader line from window to anchor
- [ ] Implement anchor point visualization
- [ ] Add hover effects for anchor
- [ ] Color coding for pinned/free states

### 2.2 Anchor Interaction
- [ ] Implement anchor dragging
- [ ] Add position-to-address conversion
- [ ] Update memory address on anchor move
- [ ] Add anchor position constraints (Shift/Ctrl/Alt)

## Phase 3: Selection and Keyboard Input

### 3.1 Selection System
- [ ] Implement viewer selection on click
- [ ] Add Tab cycling through viewers
- [ ] Visual feedback for selected viewer
- [ ] Number key quick selection (1-9)

### 3.2 Keyboard Controls
- [ ] Arrow key navigation for anchor
- [ ] Format cycling with Tab
- [ ] Dimension adjustment (+/- keys)
- [ ] Pin/lock toggles (P, L, S keys)
- [ ] Grid snapping toggle (G key)

### 3.3 Constrained Operations
- [ ] Shift constraint for H/V movement
- [ ] Ctrl snapping to boundaries
- [ ] Alt snapping to stride multiples
- [ ] Aspect ratio maintenance

## Phase 4: User Interface Elements

### 4.1 Settings Popup
- [ ] Create settings button in title bar
- [ ] Implement popup with dimension controls
- [ ] Add format dropdown
- [ ] Include checkbox options
- [ ] Add Export PNG button

### 4.2 Right-Click Context Menu
- [ ] Add "Create Bitmap Viewer" option
- [ ] Position at mouse click
- [ ] Auto-detect initial dimensions
- [ ] Set initial format based on context

### 4.3 Visual Feedback
- [ ] Constraint guide lines
- [ ] Grid overlay when snapping
- [ ] Address tooltip at anchor
- [ ] Status text for selected viewer

## Phase 5: Format Support

### 5.1 Standard Pixel Formats
- [ ] RGB888/RGBA8888 rendering
- [ ] BGR888/BGRA8888 rendering
- [ ] RGB565 16-bit support
- [ ] Grayscale rendering
- [ ] Binary (1-bit) visualization

### 5.2 Special Formats
- [ ] HEX_PIXEL implementation (32-bit as hex)
- [ ] CHAR_8BIT implementation (ASCII characters)
- [ ] Format-specific pixel size calculations
- [ ] Proper stride handling for each format

## Phase 6: Integration Features

### 6.1 FFT/Correlation Integration
- [ ] Draw width indicator on FFT
- [ ] Draw stride indicator (dashed)
- [ ] Show harmonics
- [ ] Add width/stride labels

### 6.2 Main View Integration
- [ ] Highlight source region
- [ ] Synchronize scrolling (optional)
- [ ] Update on main view changes
- [ ] Coordinate system conversion

### 6.3 PNG Export
- [ ] Export individual viewer to PNG
- [ ] Use existing PNG export code
- [ ] Timestamp-based filenames
- [ ] Include viewer name in filename

## Phase 7: Advanced Features

### 7.1 Persistence
- [ ] Save viewer configurations to JSON
- [ ] Load viewer configurations
- [ ] Auto-save on exit
- [ ] Import/export viewer sets

### 7.2 Performance Optimization
- [ ] Texture caching system
- [ ] Lazy update mechanism
- [ ] Efficient batch memory reads
- [ ] Maximum dimension limits

### 7.3 User Experience
- [ ] Duplicate viewer function (D key)
- [ ] Delete viewer confirmation
- [ ] Reset to auto-detected dimensions (R key)
- [ ] Viewer limit enforcement (max 16)

## Phase 8: Polish and Testing

### 8.1 Visual Polish
- [ ] Smooth animations for selection
- [ ] Professional color scheme
- [ ] Consistent visual language
- [ ] Clear affordances

### 8.2 Error Handling
- [ ] Handle invalid memory addresses
- [ ] Texture creation failures
- [ ] Out of bounds checking
- [ ] Maximum size validation

### 8.3 Testing
- [ ] Test all pixel formats
- [ ] Verify keyboard shortcuts
- [ ] Test constraint modifiers
- [ ] Performance testing with multiple viewers
- [ ] Edge case handling

## Implementation Order (Recommended)

1. **Week 1**: Phase 1 (Core Infrastructure)
2. **Week 2**: Phase 2 (Leader Lines) + Phase 3 (Selection)
3. **Week 3**: Phase 4 (UI Elements) + Phase 5 (Formats)
4. **Week 4**: Phase 6 (Integration) + Phase 7 (Advanced)
5. **Week 5**: Phase 8 (Polish and Testing)

## Technical Notes

### Memory Management
- Use smart pointers for viewer lifetime
- Properly release OpenGL textures
- Clean up on viewer deletion

### Coordinate Systems
- Screen space: Window and UI positioning
- Memory space: Address calculations
- Texture space: Pixel coordinates

### Dependencies
- ImGui for UI elements
- OpenGL for texture rendering
- Existing MemoryVisualizer infrastructure
- PNG export functionality (already implemented)

## Success Criteria

1. Users can create multiple bitmap viewers
2. All pixel formats are supported
3. Keyboard controls are intuitive and responsive
4. Leader lines clearly show memory source
5. FFT integration helps find correct dimensions
6. Performance remains smooth with 8+ viewers
7. Viewer state persists between sessions

## Future Enhancements (Post-MVP)

- Pattern detection for auto-sizing
- Animation playback through memory
- Side-by-side comparison mode
- Image processing filters
- Measurement/ruler tools
- Annotation system
- Shader-based rendering for large bitmaps
- Video codec detection and decoding