#include "bitmap_viewer.h"
#include "memory_visualizer.h"
#include "beacon_reader.h"
#include "memory_mapper.h"
#include "qemu_connection.h"
#include "crunched_memory_reader.h"
#include "address_space_flattener.h"
#include "font_data.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <chrono>

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

void BitmapViewerManager::CreateViewer(TypedAddress address, ImVec2 anchorPos, PixelFormat format) {
    BitmapViewer viewer;
    viewer.id = nextId++;
    
    // Initialize anchor address to the clicked memory address
    viewer.anchorAddress = address;
    viewer.anchorPos = anchorPos;
    
    // Set the format first
    viewer.format = format;
    
    // Calculate stride based on width and format
    if (viewer.format.type == PixelFormat::BINARY) {
        viewer.stride = (viewer.memWidth + 7) / 8;  // Binary: bits to bytes
    } else {
        viewer.stride = viewer.memWidth * viewer.format.bytesPerPixel;
    }
    
    // Center the viewer around the clicked point
    // Calculate offset to center the anchor in the viewer
    int centerOffsetX = viewer.memWidth / 2;
    int centerOffsetY = viewer.memHeight / 2;
    int centerOffset = centerOffsetY * viewer.stride + centerOffsetX * format.bytesPerPixel;
    
    // Adjust the viewer's base address to center on the anchor
    uint64_t viewerBaseAddr = address.value;
    if (viewerBaseAddr >= centerOffset) {
        viewerBaseAddr -= centerOffset;
    }
    viewer.memoryAddress = TypedAddress(viewerBaseAddr, address.space);
    
    // Use adjusted address as the name with proper space prefix
    viewer.name = AddressParser::Format(viewer.memoryAddress);
    
    // Calculate relative position in view
    if (memoryViewSize.x > 0 && memoryViewSize.y > 0) {
        viewer.anchorRelativePos.x = (anchorPos.x - memoryViewPos.x) / memoryViewSize.x;
        viewer.anchorRelativePos.y = (anchorPos.y - memoryViewPos.y) / memoryViewSize.y;
        // Clamp to 0-1 range
        viewer.anchorRelativePos.x = std::max(0.0f, std::min(1.0f, viewer.anchorRelativePos.x));
        viewer.anchorRelativePos.y = std::max(0.0f, std::min(1.0f, viewer.anchorRelativePos.y));
    }
    
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
    
    // Use forced resize when user changes size in settings, otherwise first use only
    ImGuiCond sizeCondition = viewer.forceResize ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowSize(viewer.windowSize, sizeCondition);
    if (viewer.forceResize) {
        viewer.forceResize = false;  // Clear the flag after using it
    }
    
    // Create window with custom title bar
    std::string windowTitle = viewer.name + "###BitmapViewer" + std::to_string(viewer.id);
    
    if (ImGui::Begin(windowTitle.c_str(), &viewer.active, flags)) {
        // Get window position for leader line
        viewer.windowPos = ImGui::GetWindowPos();
        ImVec2 newWindowSize = ImGui::GetWindowSize();
        
        // Check if window was resized by dragging
        if (newWindowSize.x != viewer.windowSize.x || newWindowSize.y != viewer.windowSize.y) {
            // Window was resized - update memory dimensions to match
            int contentWidth = (int)(newWindowSize.x - 10);  // Padding
            int contentHeight = (int)(newWindowSize.y - 35); // Title bar + padding
            
            // Update viewer dimensions
            viewer.memWidth = std::max(16, contentWidth);
            viewer.memHeight = std::max(16, contentHeight);
            
            // Update stride based on format
            if (viewer.format.type == PixelFormat::BINARY) {
                viewer.stride = (viewer.memWidth + 7) / 8;  // Binary: bits to bytes
            } else {
                viewer.stride = viewer.memWidth * viewer.format.bytesPerPixel;
            }
            viewer.needsUpdate = true;
            
            // Resize pixel buffer
            viewer.pixels.resize(viewer.memWidth * viewer.memHeight);
        }
        viewer.windowSize = newWindowSize;
        
        // Custom title bar with controls
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.2f, 0.2f, 0.25f, 1.0f));
        ImGui::BeginChild("TitleBar", ImVec2(0, 25), true);
        
        // Settings button first so it's always accessible
        if (ImGui::SmallButton("⚙")) {
            ImGui::OpenPopup("ViewerSettings");
        }
        
        // Address after settings button
        ImGui::SameLine();
        ImGui::Text("%s", viewer.name.c_str());
        
        // Format selector (keeping in title bar for quick access)
        ImGui::SameLine(100);
        ImGui::SetNextItemWidth(80);
        
        // Build format list with separator and split option
        const char* formats[] = { 
            "RGB888", "RGBA8888", "BGR888", "BGRA8888", 
            "ARGB8888", "ABGR8888", "RGB565", "GRAYSCALE", 
            "BINARY", "HEX", "CHAR",
            "---",  // Separator
            viewer.splitComponents ? "[X] Split" : "[ ] Split"
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
                    if (i == 12) { // Split toggle
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
                        // Update stride based on new format
                        if (viewer.format.type == PixelFormat::BINARY) {
                            viewer.stride = (viewer.memWidth + 7) / 8;  // Binary: bits to bytes  
                        } else {
                            viewer.stride = viewer.memWidth * viewer.format.bytesPerPixel;
                        }
                        viewer.needsUpdate = true;
                    }
                }
                
                if (isSelected && i < 11) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        
        // Size info after format selector
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%dx%d", viewer.memWidth, viewer.memHeight);
        
        // Close button on the right
        float buttonX = ImGui::GetWindowWidth() - 25;
        
        // Settings popup
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        ImGui::SetNextWindowSizeConstraints(ImVec2(140, 0), ImVec2(160, FLT_MAX));
        if (ImGui::BeginPopup("ViewerSettings")) {
            ImGui::Text("Viewer Settings");
            ImGui::Separator();
            
            // Address input with notation support
            char addrBuf[64];
            std::stringstream ss;
            switch (viewer.memoryAddress.space) {
                case AddressSpace::VIRTUAL: ss << "v:"; break;
                case AddressSpace::CRUNCHED: ss << "c:"; break;
                case AddressSpace::PHYSICAL: ss << "p:"; break;
                case AddressSpace::SHARED: default: ss << "s:"; break;
            }
            ss << std::hex << viewer.memoryAddress.value;
            strcpy(addrBuf, ss.str().c_str());
            
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputText("##Address", addrBuf, sizeof(addrBuf), 
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                // Parse using basic notation (TODO: use AddressParser)
                uint64_t newAddr = 0;
                AddressSpace newSpace = viewer.memoryAddress.space;
                
                // Simple parsing for now
                if (strncmp(addrBuf, "s:", 2) == 0) {
                    newSpace = AddressSpace::SHARED;
                    sscanf(addrBuf + 2, "%llx", &newAddr);
                } else if (strncmp(addrBuf, "p:", 2) == 0) {
                    newSpace = AddressSpace::PHYSICAL;
                    sscanf(addrBuf + 2, "%llx", &newAddr);
                } else if (strncmp(addrBuf, "v:", 2) == 0) {
                    newSpace = AddressSpace::VIRTUAL;
                    sscanf(addrBuf + 2, "%llx", &newAddr);
                } else if (strncmp(addrBuf, "c:", 2) == 0) {
                    newSpace = AddressSpace::CRUNCHED;
                    sscanf(addrBuf + 2, "%llx", &newAddr);
                } else {
                    // No prefix, assume hex
                    sscanf(addrBuf, "%llx", &newAddr);
                }
                
                if (newAddr != 0) {
                    viewer.memoryAddress = TypedAddress(newAddr, newSpace);
                    viewer.needsUpdate = true;
                    // Update name
                    viewer.name = addrBuf;
                }
            }
            
            // Format selector in popup with split option
            ImGui::Separator();
            const char* formats[] = { 
                "RGB888", "RGBA8888", "BGR888", "BGRA8888", 
                "ARGB8888", "ABGR8888", "RGB565", "Grayscale", 
                "Binary", "Hex Pixel", "Char 8-bit",
                "---",  // Separator
                viewer.splitComponents ? "[X] Split" : "[ ] Split"
            };
            
            // Custom combo to handle the split option specially
            int displayIndex = viewer.formatIndex;
            ImGui::SetNextItemWidth(120);
            if (ImGui::BeginCombo("##Format", formats[displayIndex])) {
                for (int i = 0; i < IM_ARRAYSIZE(formats); i++) {
                    if (i == 11) { // Separator line
                        ImGui::Separator();
                        continue;
                    }
                    
                    bool isSelected = (i == displayIndex);
                    if (ImGui::Selectable(formats[i], isSelected)) {
                        if (i == 12) { // Split toggle
                            viewer.splitComponents = !viewer.splitComponents;
                            viewer.needsUpdate = true;
                        } else if (i < 11) { // Regular format selection
                            viewer.formatIndex = i;
                            // Map combo index to PixelFormat::Type
                            PixelFormat::Type newType;
                            switch(viewer.formatIndex) {
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
                            // Update stride based on new format
                            if (viewer.format.type == PixelFormat::BINARY) {
                                viewer.stride = (viewer.memWidth + 7) / 8;  // Binary: bits to bytes
                            } else {
                                viewer.stride = viewer.memWidth * viewer.format.bytesPerPixel;
                            }
                            viewer.needsUpdate = true;
                        }
                    }
                    
                    if (isSelected && i < 11) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            
            ImGui::Separator();
            // Anchor settings
            ImGui::Text("Anchor Mode:");
            
            // Track mode changes
            BitmapViewer::AnchorMode oldMode = viewer.anchorMode;
            
            ImGui::RadioButton("Stick to Address", (int*)&viewer.anchorMode, BitmapViewer::ANCHOR_TO_ADDRESS);
            ImGui::RadioButton("Stick to Position", (int*)&viewer.anchorMode, BitmapViewer::ANCHOR_TO_POSITION);
            
            // When switching from Position to Address mode, update the anchor address
            // to the current memory address at the relative position
            if (oldMode == BitmapViewer::ANCHOR_TO_POSITION && 
                viewer.anchorMode == BitmapViewer::ANCHOR_TO_ADDRESS) {
                // Calculate the memory address at the current relative position
                ImVec2 currentPos = ImVec2(
                    memoryViewPos.x + viewer.anchorRelativePos.x * memoryViewSize.x,
                    memoryViewPos.y + viewer.anchorRelativePos.y * memoryViewSize.y
                );
                viewer.anchorAddress = ScreenToMemoryAddress(currentPos);
                // Update the viewer to show this address
                viewer.memoryAddress = viewer.anchorAddress;
                viewer.name = AddressParser::Format(viewer.memoryAddress);
                viewer.needsUpdate = true;
            }
            
            // Show anchor position info based on mode
            if (viewer.anchorMode == BitmapViewer::ANCHOR_TO_ADDRESS) {
                ImGui::Text("Anchor: %s", AddressParser::Format(viewer.anchorAddress).c_str());
            } else {
                ImGui::Text("Anchor: %.1f%%, %.1f%%", 
                           viewer.anchorRelativePos.x * 100.0f, 
                           viewer.anchorRelativePos.y * 100.0f);
            }
            
            if (ImGui::Checkbox("Show Leader Line", &viewer.showLeader)) {
                // Just toggle the visibility
            }
            
            ImGui::Separator();
            // Size controls
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("W", &viewer.memWidth)) {
                viewer.memWidth = std::max(16, viewer.memWidth);
                // Update stride based on format
                if (viewer.format.type == PixelFormat::BINARY) {
                    viewer.stride = (viewer.memWidth + 7) / 8;  // Binary: bits to bytes
                } else {
                    viewer.stride = viewer.memWidth * viewer.format.bytesPerPixel;
                }
                viewer.needsUpdate = true;
                // Resize pixel buffer
                viewer.pixels.resize(viewer.memWidth * viewer.memHeight);
                // Update window size to match
                viewer.windowSize.x = viewer.memWidth + 10;  // Add padding
                viewer.forceResize = true;  // Force window resize on next frame
            }
            ImGui::SetNextItemWidth(90);
            if (ImGui::InputInt("H", &viewer.memHeight)) {
                viewer.memHeight = std::max(16, viewer.memHeight);
                viewer.needsUpdate = true;
                // Resize pixel buffer  
                viewer.pixels.resize(viewer.memWidth * viewer.memHeight);
                // Update window size to match
                viewer.windowSize.y = viewer.memHeight + 35;  // Add title bar + padding
                viewer.forceResize = true;  // Force window resize on next frame
            }
            
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();  // Pop WindowPadding
        
        // Close button on the right
        ImGui::SameLine(buttonX);
        if (ImGui::SmallButton("X")) {
            viewer.active = false;
        }
        
        ImGui::EndChild();
        ImGui::PopStyleColor();
        
        // Track focus for keyboard input
        if (ImGui::IsWindowFocused()) {
            focusedViewerID = viewer.id;
        }
        
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
    
    // Update anchor position based on mode
    if (viewer.anchorMode == BitmapViewer::ANCHOR_TO_ADDRESS) {
        // Anchor follows a specific memory address as the view scrolls
        if (viewer.anchorAddress.value != 0) {
            // Always update anchor position to track the address
            // This makes the anchor move with scrolling and can go off-screen
            // Note: anchorAddress might be in a different space than viewport
            // For now, assume they're in the same space (both Physical or both Crunched)
            ImVec2 newPos = MemoryToScreen(viewer.anchorAddress.value);
            viewer.anchorPos = newPos;
            // Don't constrain - let it scroll off screen
        }
    } else {
        // ANCHOR_TO_POSITION: Anchor stays at relative position in view
        viewer.anchorPos.x = memoryViewPos.x + viewer.anchorRelativePos.x * memoryViewSize.x;
        viewer.anchorPos.y = memoryViewPos.y + viewer.anchorRelativePos.y * memoryViewSize.y;
        
        // Constrain to view bounds in relative mode
        viewer.anchorPos.x = std::max(memoryViewPos.x, std::min(memoryViewPos.x + memoryViewSize.x, viewer.anchorPos.x));
        viewer.anchorPos.y = std::max(memoryViewPos.y, std::min(memoryViewPos.y + memoryViewSize.y, viewer.anchorPos.y));
        
        // Update the memory address to match what's at this relative position
        // This ensures the mini viewer shows the memory at the current anchor position
        TypedAddress newAddress = ScreenToMemoryAddress(viewer.anchorPos);
        if (newAddress.value != viewer.memoryAddress.value) {
            viewer.memoryAddress = newAddress;
            viewer.name = AddressParser::Format(viewer.memoryAddress);
            viewer.needsUpdate = true;
        }
    }
    
    // Calculate line endpoints - connect to upper-left of image
    ImVec2 imageTopLeft = viewer.imageScreenPos;
    
    // Set up clipping rectangle for the main view area
    drawList->PushClipRect(memoryViewPos, 
                           ImVec2(memoryViewPos.x + memoryViewSize.x, memoryViewPos.y + memoryViewSize.y), 
                           true);
    
    // Draw line from image top-left to anchor
    ImU32 lineColor = viewer.isPinned ? IM_COL32(255, 0, 0, 128) : IM_COL32(255, 255, 0, 128);
    drawList->AddLine(imageTopLeft, viewer.anchorPos, lineColor, 2.0f);
    
    // Draw anchor point only if it's within the view bounds
    bool anchorInView = viewer.anchorPos.x >= memoryViewPos.x && 
                       viewer.anchorPos.x <= memoryViewPos.x + memoryViewSize.x &&
                       viewer.anchorPos.y >= memoryViewPos.y && 
                       viewer.anchorPos.y <= memoryViewPos.y + memoryViewSize.y;
    
    if (anchorInView) {
        drawList->AddCircleFilled(viewer.anchorPos, 6, IM_COL32(255, 255, 0, 255));
        drawList->AddCircle(viewer.anchorPos, 8, IM_COL32(0, 0, 0, 255), 12, 2);
    }
    
    // Pop the clipping rectangle
    drawList->PopClipRect();
    
    // Check for anchor dragging (only if anchor is visible)
    if (anchorInView) {
        ImVec2 mousePos = ImGui::GetMousePos();
        float dist = std::sqrt(std::pow(mousePos.x - viewer.anchorPos.x, 2) + 
                              std::pow(mousePos.y - viewer.anchorPos.y, 2));
        
        if (dist < 10) {
            // Hover effect - draw within clipped area
            drawList->PushClipRect(memoryViewPos, 
                                   ImVec2(memoryViewPos.x + memoryViewSize.x, memoryViewPos.y + memoryViewSize.y), 
                                   true);
            drawList->AddCircle(viewer.anchorPos, 10, IM_COL32(255, 255, 255, 255), 12, 2);
            drawList->PopClipRect();
            
            // Handle anchor dragging - check for click on the anchor itself
            if (ImGui::IsMouseClicked(0)) {
                // Start dragging this anchor
                viewer.isDraggingAnchor = true;
            }
        }
    }
    
    // Handle anchor dragging for this viewer
    if (viewer.isDraggingAnchor) {
        if (ImGui::IsMouseDragging(0)) {
            ImVec2 newPos = ImGui::GetMousePos();
            
            // Only constrain in relative position mode
            if (viewer.anchorMode == BitmapViewer::ANCHOR_TO_POSITION) {
                // Constrain to main view bounds
                newPos.x = std::max(memoryViewPos.x, std::min(memoryViewPos.x + memoryViewSize.x, newPos.x));
                newPos.y = std::max(memoryViewPos.y, std::min(memoryViewPos.y + memoryViewSize.y, newPos.y));
            }
            // In address mode, allow dragging anywhere (will update the address)
            
            viewer.anchorPos = newPos;
            
            // Update based on anchor mode
            if (viewer.anchorMode == BitmapViewer::ANCHOR_TO_ADDRESS) {
                // Update memory address based on new position
                viewer.anchorAddress = ScreenToMemoryAddress(viewer.anchorPos);
                viewer.memoryAddress = viewer.anchorAddress;
                viewer.name = AddressParser::Format(viewer.memoryAddress);
                viewer.needsUpdate = true;
            } else {
                // Update relative position
                viewer.anchorRelativePos.x = (viewer.anchorPos.x - memoryViewPos.x) / memoryViewSize.x;
                viewer.anchorRelativePos.y = (viewer.anchorPos.y - memoryViewPos.y) / memoryViewSize.y;
                // Clamp to 0-1 range
                viewer.anchorRelativePos.x = std::max(0.0f, std::min(1.0f, viewer.anchorRelativePos.x));
                viewer.anchorRelativePos.y = std::max(0.0f, std::min(1.0f, viewer.anchorRelativePos.y));
                
                // Also update memory address from current position
                viewer.memoryAddress = ScreenToMemoryAddress(viewer.anchorPos);
                viewer.name = AddressParser::Format(viewer.memoryAddress);
                viewer.needsUpdate = true;
            }
        } else if (ImGui::IsMouseReleased(0)) {
            viewer.isDraggingAnchor = false;
        }
    }
}

void BitmapViewerManager::UpdateViewers() {
    // Update all active viewers periodically for dynamic refresh
    static auto lastUpdate = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - lastUpdate).count();
    
    // Refresh at 10 Hz (every 0.1 seconds)
    const float refreshInterval = 0.1f;
    bool shouldRefresh = elapsed >= refreshInterval;
    
    for (auto& viewer : viewers) {
        if (viewer.active) {
            if (viewer.needsUpdate || shouldRefresh) {
                ExtractMemory(viewer);
                UpdateViewerTexture(viewer);
                viewer.needsUpdate = false;
            }
        }
    }
    
    if (shouldRefresh) {
        lastUpdate = now;
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
    // Calculate bytes needed based on format
    size_t bytesPerPixel = viewer.format.bytesPerPixel;
    size_t totalBytes = viewer.stride * viewer.memHeight;
    
    // If the address is in crunched space and we have a crunched reader, use it
    if (viewer.memoryAddress.space == AddressSpace::CRUNCHED && 
        crunchedReader && currentPid > 0) {
        // Allocate buffer for memory
        std::vector<uint8_t> buffer(totalBytes);
        
        // Read memory using crunched reader (handles all VA->PA translation)
        size_t bytesRead = crunchedReader->ReadCrunchedMemory(
            viewer.memoryAddress.value, totalBytes, buffer);
        
        if (bytesRead > 0) {
            // Convert to pixels based on format
            viewer.pixels.clear();
            
            // Check if we should split components
            if (viewer.splitComponents && 
                (viewer.format.type == PixelFormat::RGB888 || 
                 viewer.format.type == PixelFormat::RGBA8888 ||
                 viewer.format.type == PixelFormat::BGR888 || 
                 viewer.format.type == PixelFormat::BGRA8888 ||
                 viewer.format.type == PixelFormat::ARGB8888 || 
                 viewer.format.type == PixelFormat::ABGR8888)) {
                ConvertMemoryToSplitPixels(viewer, buffer.data(), bytesRead);
            } else if (viewer.format.type == PixelFormat::HEX_PIXEL) {
                ConvertMemoryToHexPixels(viewer, buffer.data(), bytesRead);
            } else if (viewer.format.type == PixelFormat::CHAR_8BIT) {
                ConvertMemoryToCharPixels(viewer, buffer.data(), bytesRead);
            } else {
                // Normal pixel format conversion
                ExtractPixelsFromMemory(viewer, buffer.data(), bytesRead);
            }
            return;
        }
        
        // Fall back to test pattern if read failed
        printf("CrunchedReader failed for crunched address c:0x%llx (pid: %d, bytes: %zu)\n", 
               viewer.memoryAddress.value, currentPid, totalBytes);
    }
    
    // For shared memory addresses - use beacon reader directly
    if (!beaconReader) {
        printf("No beacon reader available for memory extraction\n");
        // Fill with test pattern
        FillTestPattern(viewer);
        return;
    }
    
    // Get the file offset based on address space
    uint64_t fileOffset = 0;
    if (viewer.memoryAddress.space == AddressSpace::SHARED) {
        fileOffset = viewer.memoryAddress.value;  // Already a file offset
    } else if (viewer.memoryAddress.space == AddressSpace::PHYSICAL) {
        // Convert physical to file offset using MemoryMapper
        if (memoryMapper) {
            int64_t mappedOffset = memoryMapper->TranslateGPAToFileOffset(viewer.memoryAddress.value);
            if (mappedOffset >= 0) {
                fileOffset = mappedOffset;
            } else {
                printf("Physical address 0x%llx not found in memory regions\n", viewer.memoryAddress.value);
                FillTestPattern(viewer);
                return;
            }
        } else {
            // Fallback: hardcoded ARM64 RAM base (not ideal)
            const uint64_t RAM_BASE = 0x40000000;
            if (viewer.memoryAddress.value >= RAM_BASE) {
                fileOffset = viewer.memoryAddress.value - RAM_BASE;
            } else {
                printf("Physical address 0x%llx is below RAM base (no MemoryMapper available)\n", 
                       viewer.memoryAddress.value);
                FillTestPattern(viewer);
                return;
            }
        }
    } else {
        const char* spaceStr = "unknown";
        switch (viewer.memoryAddress.space) {
            case AddressSpace::VIRTUAL: spaceStr = "virtual"; break;
            case AddressSpace::NONE: spaceStr = "NONE (uninitialized?)"; break;
            default: spaceStr = "unknown"; break;
        }
        printf("Cannot directly access address 0x%llx in %s space without translation\n",
               viewer.memoryAddress.value, spaceStr);
        FillTestPattern(viewer);
        return;
    }
    
    // Validate the offset is within beacon reader's range
    if (beaconReader) {
        // Get the memory size from beacon reader (this is the mmap'd file size)
        size_t memSize = beaconReader->GetMemorySize();
        if (fileOffset >= memSize) {
            printf("File offset 0x%llx exceeds memory size 0x%llx for %s:0x%llx\n",
                   fileOffset, memSize,
                   viewer.memoryAddress.space == AddressSpace::SHARED ? "s" : "?",
                   viewer.memoryAddress.value);
            FillTestPattern(viewer);
            return;
        }
    }
    
    // Get direct memory pointer from beacon reader
    const uint8_t* memPtr = beaconReader->GetMemoryPointer(fileOffset);
    if (!memPtr) {
        const char* spaceStr = "";
        switch (viewer.memoryAddress.space) {
            case AddressSpace::SHARED: spaceStr = "s:"; break;
            case AddressSpace::PHYSICAL: spaceStr = "p:"; break;
            case AddressSpace::VIRTUAL: spaceStr = "v:"; break;
            case AddressSpace::CRUNCHED: spaceStr = "c:"; break;
            default: break;
        }
        printf("Failed to get memory pointer for address %s0x%llx (offset: 0x%llx, beacon=%p)\n", 
               spaceStr, viewer.memoryAddress.value, fileOffset, beaconReader.get());
        // Fill with test pattern instead
        FillTestPattern(viewer);
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
    } else if (viewer.format.type == PixelFormat::BINARY) {
        // For BINARY format: each byte becomes 8 pixels (one per bit)
        ConvertMemoryToBinaryPixels(viewer, memPtr, totalBytes);
        return;
    }
    
    // Extract pixels using the existing logic
    ExtractPixelsFromMemory(viewer, memPtr, totalBytes);
}

void BitmapViewerManager::FillTestPattern(BitmapViewer& viewer) {
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
}

void BitmapViewerManager::ExtractPixelsFromMemory(BitmapViewer& viewer, const uint8_t* memPtr, size_t totalBytes) {
    // Normal pixel formats
    size_t bytesPerPixel = viewer.format.bytesPerPixel;
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
            // Create typed address - assume shared memory for now
            // (This function seems unused - the real context menu is in memory_visualizer)
            TypedAddress typedAddr = TypedAddress::Shared(clickAddress);
            CreateViewer(typedAddr, clickPos, format);
        }
        ImGui::EndPopup();
    }
}

ImVec2 BitmapViewerManager::MemoryToScreen(uint64_t address) {
    // Convert memory address to screen position based on current viewport
    // The address passed in should be in the same space as viewportBaseAddress
    
    // Calculate offset from viewport base
    int64_t offset = (int64_t)address - (int64_t)viewportBaseAddress;
    
    // Convert byte offset to pixel offset
    int64_t pixelOffset = offset / viewportBytesPerPixel;
    
    // Convert to x,y coordinates
    int64_t y = pixelOffset / viewportWidth;
    int64_t x = pixelOffset % viewportWidth;
    
    // Scale to screen coordinates
    float screenX = memoryViewPos.x + (x * memoryViewSize.x / viewportWidth);
    float screenY = memoryViewPos.y + (y * memoryViewSize.y / viewportHeight);
    
    return ImVec2(screenX, screenY);
}

TypedAddress BitmapViewerManager::ScreenToMemoryAddress(ImVec2 screenPos) {
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
    
    uint64_t address = viewportBaseAddress + offset;
    
    // Determine the address space based on current mode
    if (useVirtualAddresses && addressFlattener) {
        // In VA mode, the viewport base is in crunched space
        // But check if the address is reasonable for crunched space
        uint64_t maxFlat = addressFlattener->GetFlatSize();
        if (address < maxFlat) {
            return TypedAddress::Crunched(address);
        } else {
            printf("Warning: Address 0x%llx exceeds crunched space size (0x%llx)\n", 
                   address, maxFlat);
            // Fall back to shared memory interpretation
            return TypedAddress::Shared(address);
        }
    } else {
        // In physical mode, return a physical address
        // The viewport is displaying physical addresses
        return TypedAddress::Physical(address);
    }
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
        
        if (row * 7 >= (size_t)viewer.memHeight) break;  // Out of viewport
        
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
        uint32_t fgColor = CalcHiContrastOpposite(bgColor);  // Use same as main viewer
        
        // Draw 8 hex nibbles (33 pixels wide total, 8 pixels tall)
        for (int nibbleIdx = 7; nibbleIdx >= 0; --nibbleIdx) {
            uint8_t nibble = (value >> (nibbleIdx * 4)) & 0xF;
            uint16_t glyph = GetGlyph3x5Hex(nibble);
            
            // Position of this nibble (4 pixels wide each)
            size_t nibbleX = col * 33 + (7 - nibbleIdx) * 4;
            size_t nibbleY = row * 7;
            
            // Draw the 3x5 glyph in a 4x7 box with 1px border on left, top and bottom
            for (int y = 0; y < 7; ++y) {
                for (int x = 0; x < 4; ++x) {
                    size_t pixX = nibbleX + x;
                    size_t pixY = nibbleY + y;
                    
                    if (pixX >= (size_t)viewer.memWidth || pixY >= (size_t)viewer.memHeight) continue;
                    
                    // Default to background color
                    uint32_t color = bgColor;
                    
                    // Check if we're in the glyph area (3x5 shifted by 1,1 for borders)
                    if (x > 0 && x < 4 && y > 0 && y < 6) {
                        // Adjust coordinates for the shifted glyph
                        int glyphX = x - 1;
                        int glyphY = y - 1;
                        
                        if (glyphX < 3 && glyphY < 5) {
                            // Extract bit from glyph - mirror horizontally
                            int bitPos = (4 - glyphY) * 3 + (2 - glyphX);
                            if (bitPos >= 0 && bitPos < 15) {
                                bool bit = (glyph >> bitPos) & 1;
                                if (bit) {
                                    color = fgColor;
                                }
                            }
                        }
                    }
                    
                    size_t pixelIdx = pixY * viewer.memWidth + pixX;
                    viewer.pixels[pixelIdx] = color;
                }
            }
        }
        
        // Draw the rightmost border column (the 33rd column)
        size_t borderX = col * 33 + 32;
        if (borderX < (size_t)viewer.memWidth) {
            for (int y = 0; y < 7; ++y) {
                size_t pixY = row * 7 + y;
                if (pixY < (size_t)viewer.memHeight) {
                    size_t pixelIdx = pixY * viewer.memWidth + borderX;
                    viewer.pixels[pixelIdx] = bgColor;
                }
            }
        }
        
        // Fill the bottom row (7th row) with background for this value
        for (int x = 0; x < 32; ++x) {  // Don't include the rightmost column (already filled)
            size_t pixX = col * 33 + x;
            size_t pixY = row * 7 + 6;  // The 7th row (index 6)
            if (pixX < (size_t)viewer.memWidth && pixY < (size_t)viewer.memHeight) {
                size_t pixelIdx = pixY * viewer.memWidth + pixX;
                viewer.pixels[pixelIdx] = bgColor;
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

void BitmapViewerManager::ConvertMemoryToBinaryPixels(BitmapViewer& viewer, const uint8_t* memPtr, size_t totalBytes) {
    // For binary display, each byte becomes 8 pixels (one per bit)
    viewer.pixels.clear();
    viewer.pixels.resize(viewer.memWidth * viewer.memHeight, 0xFF000000);
    
    // Calculate how many bytes we need
    size_t totalPixels = viewer.memWidth * viewer.memHeight;
    size_t bytesNeeded = (totalPixels + 7) / 8;  // Round up
    
    // Process each pixel position
    for (size_t pixelIdx = 0; pixelIdx < totalPixels; pixelIdx++) {
        size_t byteIdx = pixelIdx / 8;
        int bitIdx = 7 - (pixelIdx % 8);  // MSB first
        
        if (byteIdx < totalBytes) {
            uint8_t byte = memPtr[byteIdx];
            uint8_t bit = (byte >> bitIdx) & 1;
            uint8_t value = bit ? 255 : 0;
            viewer.pixels[pixelIdx] = PackRGBA(value, value, value);
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

bool BitmapViewerManager::HasFocus() const {
    return focusedViewerID != -1;
}

void BitmapViewerManager::HandleKeyboardInput() {
    if (focusedViewerID == -1) return;
    
    // Find the focused viewer
    BitmapViewer* focusedViewer = nullptr;
    for (auto& viewer : viewers) {
        if (viewer.id == focusedViewerID) {
            focusedViewer = &viewer;
            break;
        }
    }
    
    if (!focusedViewer || !focusedViewer->active) {
        focusedViewerID = -1;
        return;
    }
    
    // Handle keyboard navigation for the focused mini-viewer
    ImGuiIO& io = ImGui::GetIO();
    bool shiftPressed = io.KeyShift;
    bool ctrlPressed = io.KeyCtrl || io.KeySuper;  // Cmd key on macOS
    
    // Ctrl+Arrow keys adjust width/height
    if (ctrlPressed) {
        bool needsUpdate = false;
        
        // Ctrl+Left/Right adjusts width
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            // Decrease width - 1 pixel normally, 8 with Shift
            int step = shiftPressed ? 8 : 1;
            if (focusedViewer->memWidth > 32) {
                focusedViewer->memWidth = std::max(32, focusedViewer->memWidth - step);
                needsUpdate = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            // Increase width - 1 pixel normally, 8 with Shift
            int step = shiftPressed ? 8 : 1;
            if (focusedViewer->memWidth < 2048) {
                focusedViewer->memWidth = std::min(2048, focusedViewer->memWidth + step);
                needsUpdate = true;
            }
        }
        
        // Ctrl+Up/Down adjusts height
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            // Decrease height - 1 pixel normally, 8 with Shift
            int step = shiftPressed ? 8 : 1;
            if (focusedViewer->memHeight > 32) {
                focusedViewer->memHeight = std::max(32, focusedViewer->memHeight - step);
                needsUpdate = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            // Increase height - 1 pixel normally, 8 with Shift
            int step = shiftPressed ? 8 : 1;
            if (focusedViewer->memHeight < 2048) {
                focusedViewer->memHeight = std::min(2048, focusedViewer->memHeight + step);
                needsUpdate = true;
            }
        }
        
        if (needsUpdate) {
            // Update stride based on new width
            if (focusedViewer->format.type == PixelFormat::BINARY) {
                focusedViewer->stride = (focusedViewer->memWidth + 7) / 8;
            } else {
                focusedViewer->stride = focusedViewer->memWidth * focusedViewer->format.bytesPerPixel;
            }
            // Resize pixel buffer
            focusedViewer->pixels.resize(focusedViewer->memWidth * focusedViewer->memHeight);
            // Mark for texture update
            focusedViewer->needsUpdate = true;
            // Update window size to match
            focusedViewer->windowSize.x = focusedViewer->memWidth + 10;
            focusedViewer->windowSize.y = focusedViewer->memHeight + 35;
            focusedViewer->forceResize = true;
        }
        return;  // Don't process normal navigation when Ctrl is held
    }
    
    // Calculate movement based on viewer's pixel format
    int moveX = 1;
    int moveY = 1;
    
    if (shiftPressed) {
        // Shift for 4-byte alignment
        moveX = 4;
        moveY = focusedViewer->stride;  // Move by full row
    } else {
        // Normal movement by pixel size
        moveX = focusedViewer->format.bytesPerPixel;
        moveY = focusedViewer->stride;
    }
    
    bool needsUpdate = false;
    uint64_t oldAddr = focusedViewer->memoryAddress.value;
    
    // Arrow keys
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        if (focusedViewer->memoryAddress.value >= moveX) {
            focusedViewer->memoryAddress.value -= moveX;
            needsUpdate = true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        focusedViewer->memoryAddress.value += moveX;
        needsUpdate = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        if (focusedViewer->memoryAddress.value >= moveY) {
            focusedViewer->memoryAddress.value -= moveY;
            needsUpdate = true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        focusedViewer->memoryAddress.value += moveY;
        needsUpdate = true;
    }
    
    // Page Up/Down
    int pageSize = focusedViewer->memHeight * focusedViewer->stride;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
        if (focusedViewer->memoryAddress.value >= pageSize) {
            focusedViewer->memoryAddress.value -= pageSize;
            needsUpdate = true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        focusedViewer->memoryAddress.value += pageSize;
        needsUpdate = true;
    }
    
    // Home/End keys - move to start/end of current row
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
        // Move to start of row
        uint64_t rowStart = (focusedViewer->memoryAddress.value / focusedViewer->stride) * focusedViewer->stride;
        if (focusedViewer->memoryAddress.value != rowStart) {
            focusedViewer->memoryAddress.value = rowStart;
            needsUpdate = true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End)) {
        // Move to end of row
        uint64_t rowStart = (focusedViewer->memoryAddress.value / focusedViewer->stride) * focusedViewer->stride;
        uint64_t rowEnd = rowStart + focusedViewer->stride - focusedViewer->format.bytesPerPixel;
        if (focusedViewer->memoryAddress.value != rowEnd) {
            focusedViewer->memoryAddress.value = rowEnd;
            needsUpdate = true;
        }
    }
    
    if (needsUpdate) {
        // Update the viewer name with new address
        focusedViewer->name = AddressParser::Format(focusedViewer->memoryAddress);
        focusedViewer->needsUpdate = true;
    }
}

} // namespace Haywire