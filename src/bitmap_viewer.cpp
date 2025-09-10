#include "bitmap_viewer.h"
#include "memory_visualizer.h"
#include "beacon_reader.h"
#include "memory_mapper.h"
#include "qemu_connection.h"
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

void BitmapViewerManager::CreateViewer(uint64_t address, ImVec2 anchorPos) {
    BitmapViewer viewer;
    viewer.id = nextId++;
    viewer.name = "Viewer " + std::to_string(viewer.id);
    viewer.memoryAddress = address;
    viewer.anchorPos = anchorPos;
    
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
            viewer.stride = viewer.memWidth;  // Stride = width for now
            viewer.needsUpdate = true;
            
            // Resize pixel buffer
            viewer.pixels.resize(viewer.memWidth * viewer.memHeight);
        }
        viewer.windowSize = newWindowSize;
        
        // Custom title bar with controls
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.2f, 0.2f, 0.25f, 1.0f));
        ImGui::BeginChild("TitleBar", ImVec2(0, 25), true);
        
        // Title on the left
        ImGui::Text("%s", viewer.name.c_str());
        
        // Address in the middle
        ImGui::SameLine(100);
        ImGui::Text("0x%llX", viewer.memoryAddress);
        
        // Format selector
        ImGui::SameLine(200);
        ImGui::SetNextItemWidth(80);
        const char* formats[] = { "RGB", "RGBA", "HEX", "CHAR" };
        int formatIndex = 0;  // TODO: Store in viewer
        ImGui::Combo("##Format", &formatIndex, formats, IM_ARRAYSIZE(formats));
        
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
    size_t bytesPerPixel = 3; // Default RGB888
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
    viewer.pixels.reserve(viewer.memWidth * viewer.memHeight);
    
    for (int y = 0; y < viewer.memHeight; y++) {
        for (int x = 0; x < viewer.memWidth; x++) {
            size_t offset = y * viewer.stride + x * bytesPerPixel;
            
            if (offset + bytesPerPixel <= totalBytes) {
                uint32_t pixel = 0xFF000000; // Default alpha
                
                // Simple RGB888 for now
                pixel |= memPtr[offset] << 16;     // R
                pixel |= memPtr[offset + 1] << 8;  // G
                pixel |= memPtr[offset + 2];       // B
                
                viewer.pixels.push_back(pixel);
            } else {
                viewer.pixels.push_back(0xFF000000); // Black for out of bounds
            }
        }
    }
    
}

void BitmapViewerManager::HandleContextMenu(uint64_t clickAddress, ImVec2 clickPos) {
    if (ImGui::BeginPopupContextVoid("BitmapViewerContext")) {
        if (ImGui::MenuItem("Create Bitmap Viewer Here")) {
            CreateViewer(clickAddress, clickPos);
        }
        ImGui::EndPopup();
    }
}

ImVec2 BitmapViewerManager::MemoryToScreen(uint64_t address) {
    // TODO: Convert memory address to screen position based on current viewport
    return ImVec2(100, 100);
}

uint64_t BitmapViewerManager::ScreenToMemoryAddress(ImVec2 screenPos) {
    // For now, use a simple calculation based on screen position
    // This would ideally use the main memory view's coordinate system
    // TODO: Integrate with main memory visualizer's viewport
    
    // Simple placeholder - treat each pixel as 1 byte offset
    uint64_t baseAddr = 0x40000000;  // Default base address
    uint64_t offset = (uint64_t)(screenPos.y * 1024 + screenPos.x);
    return baseAddr + offset;
}

const char* PixelFormatToString(int format) {
    return "RGB888";  // Simple for now
}

} // namespace Haywire