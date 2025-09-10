#include "bitmap_viewer.h"
#include "memory_visualizer.h"
#include "beacon_reader.h"
#include "memory_mapper.h"
#include "qemu_connection.h"
#include "font_data.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Haywire {

BitmapViewerManager::BitmapViewerManager() {
}

BitmapViewerManager::~BitmapViewerManager() {
    // Clean up textures
    for (auto& viewer : viewers) {
        if (viewer.texture) {
            glDeleteTextures(1, &viewer.texture);
        }
    }
}

void BitmapViewerManager::CreateViewer(uint64_t address, ImVec2 anchorPos, PixelFormat format) {
    BitmapViewer viewer;
    viewer.id = nextId++;
    // Use address as the name with proper space prefix (shared memory)
    std::stringstream ss;
    ss << "s:" << std::hex << address;
    viewer.name = ss.str();
    viewer.memoryAddress = address;
    viewer.anchorPos = anchorPos;
    
    // Use the format from the main window
    viewer.format = format;
    
    // Set the format index for the combo box
    switch(format.type) {
        case PixelFormat::RGB888: viewer.formatIndex = 0; break;
        case PixelFormat::RGBA8888: viewer.formatIndex = 1; break;
        case PixelFormat::BGR888: viewer.formatIndex = 2; break;
        case PixelFormat::BGRA8888: viewer.formatIndex = 3; break;
        case PixelFormat::ARGB8888: viewer.formatIndex = 4; break;
        case PixelFormat::ABGR8888: viewer.formatIndex = 5; break;
        case PixelFormat::RGB565: viewer.formatIndex = 6; break;
        case PixelFormat::GRAYSCALE: viewer.formatIndex = 7; break;
        case PixelFormat::BINARY: viewer.formatIndex = 8; break;
        case PixelFormat::HEX_PIXEL: viewer.formatIndex = 9; break;
        case PixelFormat::CHAR_8BIT: viewer.formatIndex = 10; break;
        default: viewer.formatIndex = 0; break;
    }
    
    // Initialize stride based on the format
    viewer.stride = viewer.memWidth * viewer.format.bytesPerPixel;
    
    // Position window offset from anchor
    viewer.windowPos = ImVec2(anchorPos.x + 100, anchorPos.y - 50);
    
    // Initialize texture
    glGenTextures(1, &viewer.texture);
    glBindTexture(GL_TEXTURE_2D, viewer.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    
    // Allocate pixel buffer
    viewer.pixels.resize(viewer.memWidth * viewer.memHeight);
    
    viewers.push_back(viewer);
}

void BitmapViewerManager::RemoveViewer(int id) {
    auto it = std::find_if(viewers.begin(), viewers.end(),
        [id](const BitmapViewer& v) { return v.id == id; });
    
    if (it != viewers.end()) {
        if (it->texture) {
            glDeleteTextures(1, &it->texture);
        }
        viewers.erase(it);
    }
}

void BitmapViewerManager::DrawViewers() {
    
    // Draw all active viewers
    for (auto& viewer : viewers) {
        if (viewer.active) {
            DrawViewer(viewer);
        }
    }
    
    // Draw leader lines last (on top of everything)
    for (auto& viewer : viewers) {
        if (viewer.active && viewer.showLeader) {
            DrawLeaderLine(viewer);
        }
    }
}

void BitmapViewerManager::DrawViewer(BitmapViewer& viewer) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
    
    // Set window position and size
    ImGui::SetNextWindowPos(viewer.windowPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(viewer.windowSize, ImGuiCond_FirstUseEver);
    
    // Create window with custom title bar
    std::string windowTitle = viewer.name + "###BitmapViewer" + std::to_string(viewer.id);
    
    
    if (ImGui::Begin(windowTitle.c_str(), &viewer.active, flags)) {
        // Get window position and size for leader line
        viewer.windowPos = ImGui::GetWindowPos();
        ImVec2 newWindowSize = ImGui::GetWindowSize();
        
        // Check if window was resized
        if (newWindowSize.x != viewer.windowSize.x || newWindowSize.y != viewer.windowSize.y) {
            // Window was resized - update memory dimensions
            // Account for title bar height (approximately 30 pixels)
            int contentWidth = (int)(newWindowSize.x - 10);  // Padding
            int contentHeight = (int)(newWindowSize.y - 35); // Title bar + padding
            
            // Update viewer dimensions
            viewer.memWidth = std::max(16, contentWidth);
            viewer.memHeight = std::max(16, contentHeight);
            viewer.stride = viewer.memWidth * viewer.format.bytesPerPixel;  // Stride = width * bytes per pixel
            viewer.needsUpdate = true;
            
            // Resize pixel buffer
            viewer.pixels.resize(viewer.memWidth * viewer.memHeight);
        }
        viewer.windowSize = newWindowSize;
        
        // Custom title bar with controls
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.2f, 0.2f, 0.25f, 1.0f));
        ImGui::BeginChild("TitleBar", ImVec2(0, 25), true);
        
        // Address on the left (acts as title)
        ImGui::Text("%s", viewer.name.c_str());
        
        // Size info
        ImGui::SameLine(100);
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%dx%d", viewer.memWidth, viewer.memHeight);
        
        // Format selector with split option
        ImGui::SameLine(160);
        ImGui::SetNextItemWidth(80);
        
        // Build format list with separator and split option
        const char* formats[] = { 
            "RGB888", "RGBA8888", "BGR888", "BGRA8888", 
            "ARGB8888", "ABGR8888", "RGB565", "GRAYSCALE", 
            "BINARY", "HEX", "CHAR",
            "---",  // Separator
            viewer.splitComponents ? "✓ Split Components" : "  Split Components"
        };
        
        // Dynamically set height to show all items based on array size
        int numFormats = IM_ARRAYSIZE(formats);
        float itemHeight = ImGui::GetTextLineHeightWithSpacing();
        float maxHeight = itemHeight * (numFormats + 0.5f); // All items plus a bit of padding
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxHeight));
        
        // Custom combo to handle the split option specially
        int displayIndex = viewer.formatIndex;
        if (ImGui::BeginCombo("##Format", formats[displayIndex])) {
            for (int i = 0; i < numFormats; i++) {
                if (i == 11) { // Separator line
                    ImGui::Separator();
                    continue;
                }
                
                bool isSelected = (i == displayIndex);
                if (ImGui::Selectable(formats[i], isSelected)) {
                    if (i == 12) { // Split Components toggle
                        viewer.splitComponents = !viewer.splitComponents;
                        viewer.needsUpdate = true;
                    } else if (i < 11) { // Regular format selection
                        viewer.formatIndex = i;
                        // Map combo index to PixelFormat::Type
                        PixelFormat::Type newType;
                        switch(i) {
                            case 0: newType = PixelFormat::RGB888; break;
                            case 1: newType = PixelFormat::RGBA8888; break;
                            case 2: newType = PixelFormat::BGR888; break;
                            case 3: newType = PixelFormat::BGRA8888; break;
                            case 4: newType = PixelFormat::ARGB8888; break;
                            case 5: newType = PixelFormat::ABGR8888; break;
                            case 6: newType = PixelFormat::RGB565; break;
                            case 7: newType = PixelFormat::GRAYSCALE; break;
                            case 8: newType = PixelFormat::BINARY; break;
                            case 9: newType = PixelFormat::HEX_PIXEL; break;
                            case 10: newType = PixelFormat::CHAR_8BIT; break;
                            default: newType = PixelFormat::RGB888; break;
                        }
                        viewer.format = PixelFormat(newType);
                        // Update stride when format changes
                        viewer.stride = viewer.memWidth * viewer.format.bytesPerPixel;
                        viewer.needsUpdate = true;
                    }
                }
                
                if (isSelected && i < 11) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        
        // Settings and close buttons on the right
        float buttonX = ImGui::GetWindowWidth() - 50;
        ImGui::SameLine(buttonX);
        if (ImGui::SmallButton("⚙")) {
            // TODO: Show settings popup
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            viewer.active = false;
        }
        
        ImGui::EndChild();
        ImGui::PopStyleColor();
        
        // Check if window is being dragged
        if (ImGui::IsWindowFocused() && ImGui::IsMouseDragging(0)) {
            if (!viewer.isDragging && ImGui::IsWindowHovered()) {
                viewer.isDragging = true;
                viewer.dragOffset = ImGui::GetMousePos();
                viewer.dragOffset.x -= viewer.windowPos.x;
                viewer.dragOffset.y -= viewer.windowPos.y;
            }
        } else {
            viewer.isDragging = false;
        }
        
        // Display the texture and store its position
        if (viewer.texture) {
            ImVec2 imagePos = ImGui::GetCursorScreenPos();
            viewer.imageScreenPos = imagePos;  // Store for leader line
            
            // Calculate available space for the image
            ImVec2 availSize = ImGui::GetContentRegionAvail();
            
            // Display the image filling the available space
            ImGui::Image((void*)(intptr_t)viewer.texture, availSize);
        } else {
            // Show placeholder if no texture yet
            ImGui::Text("Loading...");
        }
        
        // Draw resize handle (birdie) in bottom-right corner
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 windowMax = ImVec2(viewer.windowPos.x + viewer.windowSize.x,
                                  viewer.windowPos.y + viewer.windowSize.y);
        ImVec2 birdiePos = ImVec2(windowMax.x - 15, windowMax.y - 15);
        
        // Draw birdie (small circle)
        drawList->AddCircleFilled(birdiePos, 5, IM_COL32(128, 128, 128, 255));
        
        // Check for resize interaction
        ImVec2 mousePos = ImGui::GetMousePos();
        float dist = std::sqrt(std::pow(mousePos.x - birdiePos.x, 2) + 
                              std::pow(mousePos.y - birdiePos.y, 2));
        
        if (dist < 10) {
            // Hover effect
            drawList->AddCircle(birdiePos, 7, IM_COL32(255, 255, 255, 255), 12, 2);
            
            if (ImGui::IsMouseClicked(0)) {
                viewer.isResizing = true;
            }
        }
        
        // Handle resizing with birdie
        if (viewer.isResizing) {
            if (ImGui::IsMouseDragging(0)) {
                ImVec2 newSize = ImGui::GetMousePos();
                viewer.windowSize.x = std::max(100.0f, newSize.x - viewer.windowPos.x);
                viewer.windowSize.y = std::max(100.0f, newSize.y - viewer.windowPos.y);
                // The actual dimension update happens above when we detect size change
            } else {
                viewer.isResizing = false;
            }
        }
    }
    ImGui::End();
}

void BitmapViewerManager::DrawLeaderLine(BitmapViewer& viewer) {
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    
    // Calculate line endpoints - connect to upper-left of image
    ImVec2 imageTopLeft = viewer.imageScreenPos;
    
    // Draw line from image top-left to anchor
    ImU32 lineColor = viewer.isPinned ? IM_COL32(255, 0, 0, 128) : IM_COL32(255, 255, 0, 128);
    drawList->AddLine(imageTopLeft, viewer.anchorPos, lineColor, 2.0f);
    
    // Draw anchor point
    drawList->AddCircleFilled(viewer.anchorPos, 6, IM_COL32(255, 255, 0, 255));
    drawList->AddCircle(viewer.anchorPos, 8, IM_COL32(0, 0, 0, 255), 12, 2);
    
    // Check for anchor dragging
    ImVec2 mousePos = ImGui::GetMousePos();
    float dist = std::sqrt(std::pow(mousePos.x - viewer.anchorPos.x, 2) + 
                          std::pow(mousePos.y - viewer.anchorPos.y, 2));
    
    if (dist < 10) {
        // Hover effect
        drawList->AddCircle(viewer.anchorPos, 10, IM_COL32(255, 255, 255, 255), 12, 2);
        
        // Handle anchor dragging - check for click on the anchor itself
        if (ImGui::IsMouseClicked(0)) {
            // Start dragging this anchor
            viewer.isDraggingAnchor = true;
        }
    }
    
    // Handle anchor dragging for this viewer
    if (viewer.isDraggingAnchor) {
        if (ImGui::IsMouseDragging(0)) {
            viewer.anchorPos = ImGui::GetMousePos();
            // Update memory address based on new position
            // This updates where the viewer is looking in memory
            viewer.memoryAddress = ScreenToMemoryAddress(viewer.anchorPos);
            // Update the viewer name to reflect new address with space prefix
            std::stringstream ss;
            ss << "s:" << std::hex << viewer.memoryAddress;
            viewer.name = ss.str();
            viewer.needsUpdate = true;
        } else if (ImGui::IsMouseReleased(0)) {
            viewer.isDraggingAnchor = false;
        }
    }
}

void BitmapViewerManager::UpdateViewers() {
    for (auto& viewer : viewers) {
        if (viewer.active && viewer.needsUpdate) {
            ExtractMemory(viewer);
            UpdateViewerTexture(viewer);
            viewer.needsUpdate = false;
        }
    }
}

void BitmapViewerManager::UpdateViewerTexture(BitmapViewer& viewer) {
    if (!viewer.texture) {
        // Create texture if it doesn't exist
        glGenTextures(1, &viewer.texture);
        glBindTexture(GL_TEXTURE_2D, viewer.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    
    // Ensure pixels array is the right size
    size_t expectedSize = viewer.memWidth * viewer.memHeight;
    if (viewer.pixels.size() != expectedSize) {
        viewer.pixels.resize(expectedSize);
    }
    
    // Convert pixels based on format
    std::vector<uint32_t> rgba_pixels;
    rgba_pixels.reserve(viewer.pixels.size());
    
    // For now, just copy pixels directly (assuming RGBA format)
    // TODO: Implement format conversions
    for (uint32_t pixel : viewer.pixels) {
        rgba_pixels.push_back(pixel);
    }
    
    // Update OpenGL texture with new dimensions
    glBindTexture(GL_TEXTURE_2D, viewer.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, viewer.memWidth, viewer.memHeight,
                0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_pixels.data());
}

void BitmapViewerManager::ExtractMemory(BitmapViewer& viewer) {
    if (!beaconReader) {
        printf("No beacon reader available for memory extraction\n");
        return;
    }
    
    // Calculate bytes needed based on format
    size_t bytesPerPixel = viewer.format.bytesPerPixel;
    size_t totalBytes = viewer.stride * viewer.memHeight;
    
    // Start with the viewer's address
    uint64_t targetAddress = viewer.memoryAddress;
    
    // If in VA mode, translate virtual to physical
    if (useVirtualAddresses && qemuConnection && currentPid > 0) {
        uint64_t physAddr;
        // Try to translate VA to PA using QMP
        if (qemuConnection->TranslateVA2PA(0, targetAddress, physAddr)) {
            targetAddress = physAddr;
        } else {
            printf("Failed to translate VA 0x%llx to PA for PID %d\n", 
                   viewer.memoryAddress, currentPid);
        }
    }
    
    // Now translate guest physical address to file offset
    uint64_t fileOffset = targetAddress;
    
    if (memoryMapper) {
        // Use MemoryMapper for proper translation
        int64_t mappedOffset = memoryMapper->TranslateGPAToFileOffset(targetAddress);
        if (mappedOffset >= 0) {
            fileOffset = mappedOffset;
        } else {
            printf("Address 0x%llx not found in memory map\n", targetAddress);
            // Fall back to test pattern
            fileOffset = UINT64_MAX;  // Will fail the GetMemoryPointer call
        }
    } else {
        // Fallback: hardcoded ARM64 offset (not ideal but works for testing)
        if (targetAddress >= 0x40000000) {
            fileOffset = targetAddress - 0x40000000;
        }
    }
    
    // Get direct memory pointer from beacon reader
    const uint8_t* memPtr = beaconReader->GetMemoryPointer(fileOffset);
    if (!memPtr) {
        printf("Failed to get memory pointer for address 0x%llx (PA: 0x%llx, offset: 0x%llx)\n", 
               viewer.memoryAddress, targetAddress, fileOffset);
        // Fill with test pattern instead
        viewer.pixels.clear();
        viewer.pixels.reserve(viewer.memWidth * viewer.memHeight);
        
        for (int y = 0; y < viewer.memHeight; y++) {
            for (int x = 0; x < viewer.memWidth; x++) {
                // Create a test pattern
                uint32_t pixel = 0xFF000000; // Alpha
                pixel |= ((x * 255 / viewer.memWidth) & 0xFF) << 16; // R gradient
                pixel |= ((y * 255 / viewer.memHeight) & 0xFF) << 8; // G gradient
                pixel |= ((x ^ y) & 0xFF); // B pattern
                viewer.pixels.push_back(pixel);
            }
        }
        return;
    }
    
    // Convert to pixels based on format
    viewer.pixels.clear();
    
    // Check if we should split components
    if (viewer.splitComponents && 
        (viewer.format.type == PixelFormat::RGB888 || 
         viewer.format.type == PixelFormat::RGBA8888 ||
         viewer.format.type == PixelFormat::BGR888 ||
         viewer.format.type == PixelFormat::BGRA8888 ||
         viewer.format.type == PixelFormat::ARGB8888 ||
         viewer.format.type == PixelFormat::ABGR8888 ||
         viewer.format.type == PixelFormat::RGB565)) {
        ConvertMemoryToSplitPixels(viewer, memPtr, totalBytes);
        return;
    }
    
    // Handle expanded formats differently
    if (viewer.format.type == PixelFormat::HEX_PIXEL) {
        // For HEX format: each 4-byte value becomes 32x8 pixels
        ConvertMemoryToHexPixels(viewer, memPtr, totalBytes);
        return;
    } else if (viewer.format.type == PixelFormat::CHAR_8BIT) {
        // For CHAR format: each byte becomes 6x8 pixels
        ConvertMemoryToCharPixels(viewer, memPtr, totalBytes);
        return;
    }
    
    // Normal pixel formats
    viewer.pixels.reserve(viewer.memWidth * viewer.memHeight);
    
    for (int y = 0; y < viewer.memHeight; y++) {
        for (int x = 0; x < viewer.memWidth; x++) {
            size_t offset = y * viewer.stride + x * bytesPerPixel;
            
            if (offset + bytesPerPixel <= totalBytes) {
                uint32_t pixel = 0xFF000000; // Default alpha
                
                switch (viewer.format.type) {
                    case PixelFormat::RGB888:
                        pixel = PackRGBA(memPtr[offset], memPtr[offset + 1], memPtr[offset + 2]);
                        break;
                        
                    case PixelFormat::RGBA8888:
                        pixel = PackRGBA(memPtr[offset], memPtr[offset + 1], 
                                       memPtr[offset + 2], memPtr[offset + 3]);
                        break;
                        
                    case PixelFormat::BGR888:
                        pixel = PackRGBA(memPtr[offset + 2], memPtr[offset + 1], memPtr[offset]);
                        break;
                        
                    case PixelFormat::BGRA8888:
                        pixel = PackRGBA(memPtr[offset + 2], memPtr[offset + 1], 
                                       memPtr[offset], memPtr[offset + 3]);
                        break;
                        
                    case PixelFormat::ARGB8888:
                        pixel = PackRGBA(memPtr[offset + 1], memPtr[offset + 2], 
                                       memPtr[offset + 3], memPtr[offset]);
                        break;
                        
                    case PixelFormat::ABGR8888:
                        pixel = PackRGBA(memPtr[offset + 3], memPtr[offset + 2], 
                                       memPtr[offset + 1], memPtr[offset]);
                        break;
                        
                    case PixelFormat::RGB565: {
                        uint16_t val = (memPtr[offset] << 8) | memPtr[offset + 1];
                        uint8_t r = ((val >> 11) & 0x1F) << 3;
                        uint8_t g = ((val >> 5) & 0x3F) << 2;
                        uint8_t b = (val & 0x1F) << 3;
                        pixel = PackRGBA(r, g, b);
                        break;
                    }
                    
                    case PixelFormat::GRAYSCALE:
                        pixel = PackRGBA(memPtr[offset], memPtr[offset], memPtr[offset]);
                        break;
                        
                    case PixelFormat::BINARY: {
                        uint8_t val = (memPtr[offset] > 127) ? 255 : 0;
                        pixel = PackRGBA(val, val, val);
                        break;
                    }
                    
                    case PixelFormat::HEX_PIXEL:
                    case PixelFormat::CHAR_8BIT:
                        // These need special handling - for now just show as grayscale
                        pixel = PackRGBA(memPtr[offset], memPtr[offset], memPtr[offset]);
                        break;
                        
                    default:
                        pixel = PackRGBA(memPtr[offset], 0, 0);  // Red for unknown
                        break;
                }
                
                viewer.pixels.push_back(pixel);
            } else {
                viewer.pixels.push_back(0xFF000000); // Black for out of bounds
            }
        }
    }
    
}

void BitmapViewerManager::HandleContextMenu(uint64_t clickAddress, ImVec2 clickPos, PixelFormat format) {
    if (ImGui::BeginPopupContextVoid("BitmapViewerContext")) {
        if (ImGui::MenuItem("Create Bitmap Viewer Here")) {
            CreateViewer(clickAddress, clickPos, format);
        }
        ImGui::EndPopup();
    }
}

ImVec2 BitmapViewerManager::MemoryToScreen(uint64_t address) {
    // TODO: Convert memory address to screen position based on current viewport
    return ImVec2(100, 100);
}

uint64_t BitmapViewerManager::ScreenToMemoryAddress(ImVec2 screenPos) {
    // Convert screen position to memory address based on memory visualizer's viewport
    
    // Calculate position relative to memory view area
    float relX = screenPos.x - memoryViewPos.x;
    float relY = screenPos.y - memoryViewPos.y;
    
    // Clamp to view bounds
    relX = std::max(0.0f, std::min(relX, memoryViewSize.x));
    relY = std::max(0.0f, std::min(relY, memoryViewSize.y));
    
    // Calculate memory coordinates (which pixel in the memory buffer)
    int memX = (int)(relX * viewportWidth / memoryViewSize.x);
    int memY = (int)(relY * viewportHeight / memoryViewSize.y);
    
    // Calculate byte offset from viewport base
    uint64_t offset = memY * viewportWidth * viewportBytesPerPixel + 
                      memX * viewportBytesPerPixel;
    
    return viewportBaseAddress + offset;
}

const char* PixelFormatToString(int format) {
    return "RGB888";  // Simple for now
}

void BitmapViewerManager::ConvertMemoryToHexPixels(BitmapViewer& viewer, const uint8_t* memPtr, size_t totalBytes) {
    // For HEX display, each 32-bit value (4 bytes) becomes a 32x8 pixel block
    // showing 8 hex digits with color based on the value
    
    // Calculate how many 32-bit values fit in one row
    size_t valuesPerRow = viewer.memWidth / 32;
    if (valuesPerRow == 0) valuesPerRow = 1;
    
    // Calculate pixel buffer size
    viewer.pixels.resize(viewer.memWidth * viewer.memHeight, 0xFF000000);
    
    // Process memory as 32-bit values
    size_t numValues = totalBytes / 4;
    
    for (size_t valueIdx = 0; valueIdx < numValues; ++valueIdx) {
        // Calculate position in the grid
        size_t row = valueIdx / valuesPerRow;
        size_t col = valueIdx % valuesPerRow;
        
        if (row * 8 >= (size_t)viewer.memHeight) break;  // Out of viewport
        
        // Read 32-bit value from memory
        size_t memIdx = valueIdx * 4;
        if (memIdx + 3 >= totalBytes) break;
        
        uint32_t value = (memPtr[memIdx] << 0) |
                        (memPtr[memIdx + 1] << 8) |
                        (memPtr[memIdx + 2] << 16) |
                        (memPtr[memIdx + 3] << 24);
        
        // Calculate colors based on the value
        uint32_t bgColor = PackRGBA(
            memPtr[memIdx + 2],
            memPtr[memIdx + 1],
            memPtr[memIdx],
            255
        );
        uint32_t fgColor = ContrastColor(bgColor);
        
        // Draw 8 hex nibbles (32 pixels wide, 8 pixels tall)
        for (int nibbleIdx = 7; nibbleIdx >= 0; --nibbleIdx) {
            uint8_t nibble = (value >> (nibbleIdx * 4)) & 0xF;
            uint16_t glyph = GetGlyph3x5Hex(nibble);
            
            // Position of this nibble (4 pixels wide each)
            size_t nibbleX = col * 32 + (7 - nibbleIdx) * 4;
            size_t nibbleY = row * 8;
            
            // Draw the 3x5 glyph in a 4x8 box
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 4; ++x) {
                    size_t pixX = nibbleX + x;
                    size_t pixY = nibbleY + y;
                    
                    if (pixX >= (size_t)viewer.memWidth || pixY >= (size_t)viewer.memHeight) continue;
                    
                    // Default to background color
                    uint32_t color = bgColor;
                    
                    // Check if we're in the glyph area (3x5 centered in 4x8)
                    if (x < 3 && y < 5) {
                        // Extract bit from glyph - mirror horizontally
                        int bitPos = (4 - y) * 3 + (2 - x);
                        if (bitPos >= 0 && bitPos < 15) {
                            bool bit = (glyph >> bitPos) & 1;
                            if (bit) {
                                color = fgColor;
                            }
                        }
                    }
                    
                    size_t pixelIdx = pixY * viewer.memWidth + pixX;
                    viewer.pixels[pixelIdx] = color;
                }
            }
        }
    }
}

void BitmapViewerManager::ConvertMemoryToSplitPixels(BitmapViewer& viewer, const uint8_t* memPtr, size_t totalBytes) {
    // For split display, each pixel expands horizontally by the number of components
    // Components are shown in memory order with their natural colors
    
    int componentsPerPixel = viewer.format.bytesPerPixel;
    int expandedWidth = viewer.memWidth / componentsPerPixel;  // How many original pixels fit
    
    // Calculate pixel buffer size
    viewer.pixels.resize(viewer.memWidth * viewer.memHeight, 0xFF000000);
    
    // Process each pixel
    for (int y = 0; y < viewer.memHeight; y++) {
        for (int x = 0; x < expandedWidth; x++) {
            size_t offset = y * viewer.stride + x * componentsPerPixel;
            
            if (offset + componentsPerPixel > totalBytes) break;
            
            // Extract components based on format (in memory order)
            for (int comp = 0; comp < componentsPerPixel; comp++) {
                uint8_t value = memPtr[offset + comp];
                uint32_t pixel = 0xFF000000; // Full alpha
                
                // Determine which component this is based on format and position
                switch (viewer.format.type) {
                    case PixelFormat::RGB888:
                        // Memory order: R, G, B
                        if (comp == 0) pixel |= (value << 16); // Red in red channel
                        else if (comp == 1) pixel |= (value << 8); // Green in green channel
                        else if (comp == 2) pixel |= value; // Blue in blue channel
                        break;
                        
                    case PixelFormat::BGR888:
                        // Memory order: B, G, R
                        if (comp == 0) pixel |= value; // Blue in blue channel
                        else if (comp == 1) pixel |= (value << 8); // Green in green channel
                        else if (comp == 2) pixel |= (value << 16); // Red in red channel
                        break;
                        
                    case PixelFormat::RGBA8888:
                        // Memory order: R, G, B, A
                        if (comp == 0) pixel |= (value << 16); // Red
                        else if (comp == 1) pixel |= (value << 8); // Green
                        else if (comp == 2) pixel |= value; // Blue
                        else if (comp == 3) pixel = PackRGBA(value, value, value, 255); // Alpha as white
                        break;
                        
                    case PixelFormat::BGRA8888:
                        // Memory order: B, G, R, A
                        if (comp == 0) pixel |= value; // Blue
                        else if (comp == 1) pixel |= (value << 8); // Green
                        else if (comp == 2) pixel |= (value << 16); // Red
                        else if (comp == 3) pixel = PackRGBA(value, value, value, 255); // Alpha as white
                        break;
                        
                    case PixelFormat::ARGB8888:
                        // Memory order: A, R, G, B
                        if (comp == 0) pixel = PackRGBA(value, value, value, 255); // Alpha as white
                        else if (comp == 1) pixel |= (value << 16); // Red
                        else if (comp == 2) pixel |= (value << 8); // Green
                        else if (comp == 3) pixel |= value; // Blue
                        break;
                        
                    case PixelFormat::ABGR8888:
                        // Memory order: A, B, G, R
                        if (comp == 0) pixel = PackRGBA(value, value, value, 255); // Alpha as white
                        else if (comp == 1) pixel |= value; // Blue
                        else if (comp == 2) pixel |= (value << 8); // Green
                        else if (comp == 3) pixel |= (value << 16); // Red
                        break;
                        
                    case PixelFormat::RGB565: {
                        // Special case: need to extract from 16-bit value
                        if (comp == 0 && offset + 1 < totalBytes) {
                            uint16_t val = (memPtr[offset] << 8) | memPtr[offset + 1];
                            uint8_t r = ((val >> 11) & 0x1F) << 3;
                            uint8_t g = ((val >> 5) & 0x3F) << 2;
                            uint8_t b = (val & 0x1F) << 3;
                            
                            // Show as 3 pixels: R, G, B
                            size_t pixX = x * 3;
                            size_t pixY = y;
                            if (pixX < (size_t)viewer.memWidth && pixY < (size_t)viewer.memHeight) {
                                viewer.pixels[pixY * viewer.memWidth + pixX] = PackRGBA(r, 0, 0, 255);
                            }
                            if (pixX + 1 < (size_t)viewer.memWidth) {
                                viewer.pixels[pixY * viewer.memWidth + pixX + 1] = PackRGBA(0, g, 0, 255);
                            }
                            if (pixX + 2 < (size_t)viewer.memWidth) {
                                viewer.pixels[pixY * viewer.memWidth + pixX + 2] = PackRGBA(0, 0, b, 255);
                            }
                            comp = 1; // Skip second byte
                            continue;
                        }
                        break;
                    }
                }
                
                // Place the pixel (except for RGB565 which handles itself)
                if (viewer.format.type != PixelFormat::RGB565) {
                    size_t pixX = x * componentsPerPixel + comp;
                    size_t pixY = y;
                    
                    if (pixX < (size_t)viewer.memWidth && pixY < (size_t)viewer.memHeight) {
                        size_t pixelIdx = pixY * viewer.memWidth + pixX;
                        viewer.pixels[pixelIdx] = pixel;
                    }
                }
            }
        }
    }
}

void BitmapViewerManager::ConvertMemoryToCharPixels(BitmapViewer& viewer, const uint8_t* memPtr, size_t totalBytes) {
    // For CHAR display, each byte becomes a 6x8 pixel character
    
    // Calculate how many characters fit in one row
    size_t charsPerRow = viewer.memWidth / 6;
    if (charsPerRow == 0) charsPerRow = 1;
    
    // Calculate pixel buffer size
    viewer.pixels.resize(viewer.memWidth * viewer.memHeight, 0xFF000000);
    
    // Process each byte as a character
    for (size_t byteIdx = 0; byteIdx < totalBytes; ++byteIdx) {
        // Calculate position in the grid
        size_t row = byteIdx / charsPerRow;
        size_t col = byteIdx % charsPerRow;
        
        if (row * 8 >= (size_t)viewer.memHeight) break;  // Out of viewport
        
        uint8_t ch = memPtr[byteIdx];
        
        // Skip null characters - leave them blank (black)
        if (ch == 0) {
            // Fill with black pixels
            for (int y = 0; y < 8; ++y) {
                for (int x = 0; x < 6; ++x) {
                    size_t pixX = col * 6 + x;
                    size_t pixY = row * 8 + y;
                    
                    if (pixX >= (size_t)viewer.memWidth || pixY >= (size_t)viewer.memHeight) continue;
                    
                    size_t pixelIdx = pixY * viewer.memWidth + pixX;
                    viewer.pixels[pixelIdx] = 0xFF000000; // Black
                }
            }
            continue;
        }
        
        // Simple color scheme: white text on black background (matching main visualizer)
        uint32_t fgColor = 0xFFFFFFFF;  // White
        uint32_t bgColor = 0xFF000000;  // Black
        
        // Get the glyph for this character
        uint64_t glyph = 0;
        for (size_t i = 0; i < Font5x7u_count; ++i) {
            uint64_t entry = Font5x7u[i];
            uint16_t glyphCode = (entry >> 48) & 0xFFFF;
            if (glyphCode == ch) {
                glyph = entry;
                break;
            }
        }
        
        // Draw the character (6x8 pixels)
        // Using the rotating bit pattern from memory_visualizer
        uint64_t rotatingBit = 0x0000800000000000ULL;  // bit 47
        
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 6; ++x) {
                size_t pixX = col * 6 + x;
                size_t pixY = row * 8 + y;
                
                if (pixX >= (size_t)viewer.memWidth || pixY >= (size_t)viewer.memHeight) {
                    rotatingBit >>= 1;  // Still need to advance the bit
                    continue;
                }
                
                // Check if bit is set
                bool bit = (glyph & rotatingBit) != 0;
                uint32_t color = bit ? fgColor : bgColor;
                
                size_t pixelIdx = pixY * viewer.memWidth + pixX;
                viewer.pixels[pixelIdx] = color;
                
                rotatingBit >>= 1;  // Move to next bit
            }
        }
    }
}

} // namespace Haywire