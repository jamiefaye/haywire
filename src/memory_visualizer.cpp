#include "memory_visualizer.h"
#include "qemu_connection.h"
#include "imgui.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace Haywire {

MemoryVisualizer::MemoryVisualizer() 
    : memoryTexture(0), needsUpdate(true), autoRefresh(false), autoRefreshInitialized(false),
      refreshRate(10.0f), showHexOverlay(false), showNavigator(true), showCorrelation(false),
      showChangeHighlight(true), showMagnifier(false),  // Magnifier off by default
      widthInput(640), heightInput(480), strideInput(640),  // Back to reasonable video dimensions
      pixelFormatIndex(0), mouseX(0), mouseY(0), isDragging(false),
      dragStartX(0), dragStartY(0), isReading(false), readComplete(false),
      marchingAntsPhase(0.0f), magnifierZoom(8), magnifierLocked(false),
      magnifierSize(32), magnifierLockPos(0, 0), memoryViewPos(0, 0), memoryViewSize(0, 0) {
    
    strcpy(addressInput, "0x0");  // Start at 0 where boot ROM lives
    viewport.baseAddress = 0x0;  // Initialize the actual address!
    viewport.width = widthInput;
    viewport.height = heightInput;
    viewport.stride = strideInput;
    CreateTexture();
    lastRefresh = std::chrono::steady_clock::now();
}

MemoryVisualizer::~MemoryVisualizer() {
    // Stop any ongoing read
    if (readThread.joinable()) {
        isReading = false;
        readThread.join();
    }
    
    if (memoryTexture) {
        glDeleteTextures(1, &memoryTexture);
    }
}

void MemoryVisualizer::CreateTexture() {
    if (memoryTexture) {
        glDeleteTextures(1, &memoryTexture);
    }
    
    glGenTextures(1, &memoryTexture);
    glBindTexture(GL_TEXTURE_2D, memoryTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void MemoryVisualizer::DrawControlBar(QemuConnection& qemu) {
    // Horizontal layout for controls
    DrawControls();
    
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    // Check if async read completed
    if (readComplete.exchange(false)) {
        std::lock_guard<std::mutex> lock(memoryMutex);
        currentMemory = std::move(pendingMemory);  // Just replace atomically
        needsUpdate = true;  // Mark for update in main thread
        
        // Debug: confirm texture was updated
        static int updateCount = 0;
        if (++updateCount % 10 == 0) {
            std::cerr << "Texture updated " << updateCount << " times\n";
        }
    }
    
    // Update texture in main thread (OpenGL context)
    if (needsUpdate && !currentMemory.data.empty()) {
        UpdateTexture();
        needsUpdate = false;
        glFlush();
    }
    
    // Auto-enable refresh - always on for zero-config experience
    if (!autoRefreshInitialized) {
        autoRefresh = true;
        // Set refresh rate based on connection type
        // Memory backend can handle much higher refresh rates
        refreshRate = qemu.IsUsingMemoryBackend() ? 30.0f : 5.0f;
        autoRefreshInitialized = true;
        
        std::cerr << "Auto-refresh enabled at " << refreshRate << " Hz"
                  << (qemu.IsUsingMemoryBackend() ? " (memory backend)" : " (QMP/GDB)") << "\n";
    }
    
    // Simple synchronous refresh for testing
    if (qemu.IsConnected()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - lastRefresh).count();
        
        if (elapsed >= 1.0f / refreshRate) {
            // Do a simple, direct read - no threading
            size_t size = viewport.stride * viewport.height;
            uint64_t addr = viewport.baseAddress;
            
            std::vector<uint8_t> buffer;
            if (qemu.ReadMemory(addr, size, buffer)) {
                // Just show the actual memory - we know it's changing
                static std::vector<uint8_t> lastBuffer;
                static uint64_t lastAddr = 0;
                
                // Only compare if we're at the same address
                if (!lastBuffer.empty() && lastBuffer.size() == buffer.size() && lastAddr == addr) {
                    int changedBytes = 0;
                    int firstChangeOffset = -1;
                    std::vector<ChangeRegion> currentChanges;  // Changes for this frame
                    
                    for (size_t i = 0; i < buffer.size(); i++) {
                        if (buffer[i] != lastBuffer[i]) {
                            changedBytes++;
                            
                            // Calculate X,Y position in the viewport
                            int x = (i % viewport.stride) / viewport.format.bytesPerPixel;
                            int y = i / viewport.stride;
                            
                            // Add to current frame's changes
                            bool foundAdjacentRegion = false;
                            for (auto& region : currentChanges) {
                                // Merge adjacent changes into single box
                                if (region.y == y && 
                                    (region.x + region.width == x || region.x == x + 1)) {
                                    region.width = std::max(region.x + region.width, x + 1) - std::min(region.x, x);
                                    region.x = std::min(region.x, x);
                                    foundAdjacentRegion = true;
                                    break;
                                }
                            }
                            if (!foundAdjacentRegion) {
                                currentChanges.push_back({x, y, 1, 1, std::chrono::steady_clock::now()});
                            }
                            
                            if (firstChangeOffset == -1) {
                                firstChangeOffset = i;
                            }
                        }
                    }
                    
                    if (changedBytes > 0) {
                        // Add to ring buffer
                        changeHistory.push_back(currentChanges);
                        if (changeHistory.size() > CHANGE_HISTORY_SIZE) {
                            changeHistory.pop_front();
                        }
                        lastChangeTime = std::chrono::steady_clock::now();
                        
                        // Reduced logging - only report significant changes
                        if (changedBytes > 100) {
                            std::cerr << changedBytes << " bytes changed in " 
                                     << currentChanges.size() << " regions\n";
                        }
                    }
                } else if (lastAddr != addr) {
                    changeHistory.clear();  // Clear history when address changes
                    std::cerr << "Address changed to 0x" << std::hex << addr 
                             << " - starting fresh comparison\n" << std::dec;
                }
                
                lastBuffer = buffer;
                lastAddr = addr;
                
                currentMemory.address = addr;
                currentMemory.data = std::move(buffer);
                currentMemory.stride = viewport.stride;
                
                UpdateTexture();  // Update immediately
                lastRefresh = now;
                
                // std::cerr << "Direct read and texture update at " << std::hex << addr << std::dec << "\n";
            }
        }
    }
    
    // Comment out all the async thread code for now
    /*
            readThread = std::thread([this, &qemu, addr, size, stride]() {
                std::vector<uint8_t> buffer;
                static int readCount = 0;
                readCount++;
                
                // Log every 10 reads to verify we're actually reading
                if (readCount % 10 == 0) {
                    std::cerr << "Read #" << readCount << " from 0x" << std::hex << addr 
                              << " size: " << std::dec << size;
                    
                    // Force a timestamp to ensure we're getting fresh data
                    auto now = std::chrono::system_clock::now();
                    auto time_t = std::chrono::system_clock::to_time_t(now);
                    std::cerr << " at " << std::ctime(&time_t);
                }
                
                if (qemu.ReadMemory(addr, size, buffer)) {
                    // Calculate a simple checksum of the entire buffer
                    uint32_t checksum = 0;
                    for (size_t i = 0; i < buffer.size(); i += 4) {
                        if (i + 3 < buffer.size()) {
                            checksum ^= *(uint32_t*)&buffer[i];
                        }
                    }
                    
                    static uint32_t lastChecksum = 0;
                    static uint64_t lastAddr = 0;
                    
                    if (lastAddr == addr && lastChecksum != checksum && lastChecksum != 0) {
                        std::cerr << "CHECKSUM CHANGED! Old: 0x" << std::hex << lastChecksum 
                                 << " New: 0x" << checksum << " at address 0x" << addr << std::dec << "\n";
                    }
                    
                    lastChecksum = checksum;
                    
                    // Sample more bytes to detect changes (check multiple regions)
                    static std::vector<uint8_t> lastSample;
                    
                    // Check up to 4KB or 10% of buffer, whichever is smaller
                    size_t sampleSize = std::min({size_t(4096), buffer.size(), buffer.size() / 10});
                    std::vector<uint8_t> sample(buffer.begin(), buffer.begin() + sampleSize);
                    
                    // Only compare if we're still looking at the same address
                    if (!lastSample.empty() && lastAddr == addr && sample != lastSample) {
                        // Count how many bytes changed
                        int changedBytes = 0;
                        for (size_t i = 0; i < sampleSize; i++) {
                            if (i < lastSample.size() && sample[i] != lastSample[i]) {
                                changedBytes++;
                            }
                        }
                        
                        // Report every change, with more detail
                        if (changedBytes > 0) {
                            // Track change rate for UI indicator
                            static auto lastChangeTime = std::chrono::steady_clock::now();
                            lastChangeTime = std::chrono::steady_clock::now();
                            
                            std::cerr << "Memory changed: " << changedBytes << "/" << sampleSize 
                                     << " bytes at 0x" << std::hex << addr 
                                     << " (read #" << std::dec << readCount << ")\n";
                            
                            // Show first few changed bytes
                            if (changedBytes <= 10) {
                                for (size_t i = 0; i < sampleSize && changedBytes > 0; i++) {
                                    if (i < lastSample.size() && sample[i] != lastSample[i]) {
                                        std::cerr << "  [" << i << "]: 0x" << std::hex 
                                                 << (int)lastSample[i] << " -> 0x" 
                                                 << (int)sample[i] << std::dec << "\n";
                                        changedBytes--;
                                    }
                                }
                            }
                        }
                    }
                    // Silently reset detection when address changes (scrolling)
                    
                    lastSample = sample;
                    lastAddr = addr;
                    
                    std::lock_guard<std::mutex> lock(memoryMutex);
                    pendingMemory.address = addr;
                    pendingMemory.data = std::move(buffer);
                    pendingMemory.stride = stride;
                    readComplete = true;
                }
                
                isReading = false;
            });
            
            lastRefresh = now;
        }
    }
    */
    
}

void MemoryVisualizer::DrawMemoryBitmap() {
    // Update visible height based on window size
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    heightInput = (int)(availSize.y / viewport.zoom);  // Visible rows
    viewport.height = std::max(1, heightInput);
    
    // Layout: Vertical slider on left, memory view on right
    float sliderWidth = 200;
    float memoryWidth = availSize.x - sliderWidth - 10;  // -10 for spacing
    
    // Vertical address slider on the left
    ImGui::BeginChild("AddressSlider", ImVec2(sliderWidth, availSize.y), true);
    DrawVerticalAddressSlider();
    ImGui::EndChild();
    
    ImGui::SameLine();
    
    // Memory view on the right
    ImGui::BeginChild("BitmapView", ImVec2(memoryWidth, availSize.y), false);
    DrawMemoryView();
    ImGui::EndChild();
    
    // Texture updates now happen immediately when data arrives
    // This ensures smooth 30Hz refresh with memory backend
}

void MemoryVisualizer::Draw(QemuConnection& qemu) {
    // Legacy method for compatibility - combines both
    ImGui::Columns(2, "VisualizerColumns", true);
    ImGui::SetColumnWidth(0, 300);
    
    DrawControlBar(qemu);
    
    ImGui::NextColumn();
    
    DrawMemoryBitmap();
    
    ImGui::Columns(1);
}

void MemoryVisualizer::DrawControls() {
    // First row: Address, Width, Height, Format, Refresh
    ImGui::Text("Addr:");
    ImGui::SameLine();
    ImGui::PushItemWidth(120);
    if (ImGui::InputText("##Address", addressInput, sizeof(addressInput),
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::stringstream ss;
        ss << std::hex << addressInput;
        ss >> viewport.baseAddress;
        // Round to 64K boundary
        viewport.baseAddress = (viewport.baseAddress / 65536) * 65536;
        needsUpdate = true;
    }
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    ImGui::Text("W:");
    ImGui::SameLine();
    ImGui::PushItemWidth(80);  // Bigger width field
    if (ImGui::InputInt("##Width", &widthInput)) {
        viewport.width = std::max(1, widthInput);
        viewport.stride = viewport.width;
        strideInput = viewport.stride;
        needsUpdate = true;  // Immediate update
    }
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    ImGui::Text("H:");
    ImGui::SameLine();
    ImGui::PushItemWidth(80);  // Shows visible window height
    ImGui::Text("%d", heightInput);  // Display only, not editable
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Visible window height in pixels");
    }
    
    ImGui::SameLine();
    const char* formats[] = { "RGB888", "RGBA8888", "BGR888", "BGRA8888", 
                              "ARGB8888", "ABGR8888", "RGB565", "Grayscale", "Binary" };
    ImGui::PushItemWidth(100);
    if (ImGui::Combo("##Format", &pixelFormatIndex, formats, IM_ARRAYSIZE(formats))) {
        viewport.format = PixelFormat(static_cast<PixelFormat::Type>(pixelFormatIndex));
        needsUpdate = true;  // Immediate update
    }
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    ImGui::Checkbox("Hex", &showHexOverlay);
    
    ImGui::SameLine();
    ImGui::Checkbox("Corr", &showCorrelation);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show autocorrelation for width detection");
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("Changes", &showChangeHighlight);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Highlight memory changes with yellow boxes");
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("Magnifier", &showMagnifier);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show magnified view under cursor (8x zoom)");
    }
    
    // Refresh rate is now automatic based on connection type
    // Memory backend: 30Hz, Others: 5Hz
    
    if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 windowPos = ImGui::GetWindowPos();
        int x = (mousePos.x - windowPos.x - viewport.panX) / viewport.zoom;
        int y = (mousePos.y - windowPos.y - viewport.panY) / viewport.zoom;
        
        if (x >= 0 && x < viewport.width && y >= 0 && y < viewport.height) {
            uint64_t addr = GetAddressAt(x, y);
            ImGui::SetTooltip("Address: 0x%llx", addr);
        }
    }
}

void MemoryVisualizer::DrawVerticalAddressSlider() {
    ImGui::Text("Memory Navigation");
    ImGui::Separator();
    
    const uint64_t sliderUnit = 65536;  // 64K units
    const uint64_t maxAddress = 0x200000000ULL;  // 8GB range to cover extended memory
    
    // Vertical slider
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    float sliderHeight = availSize.y - 100;  // Leave room for buttons
    
    // Convert address to vertical slider position (inverted - top is 0)
    uint64_t sliderValue = viewport.baseAddress / sliderUnit;
    uint64_t maxSliderValue = maxAddress / sliderUnit;
    
    ImGui::PushItemWidth(-1);  // Full width
    
    // - button
    if (ImGui::Button("-64K", ImVec2(-1, 30))) {
        if (viewport.baseAddress >= sliderUnit) {
            viewport.baseAddress -= sliderUnit;
            std::stringstream ss;
            ss << "0x" << std::hex << viewport.baseAddress;
            strcpy(addressInput, ss.str().c_str());
            needsUpdate = true;
        }
    }
    
    // Vertical slider
    uint64_t minSliderValue = 0;
    if (ImGui::VSliderScalar("##VAddr", ImVec2(180, sliderHeight), 
                            ImGuiDataType_U64, &sliderValue,
                            &maxSliderValue, &minSliderValue,  // Max at top, 0 at bottom
                            "0x%llx")) {
        viewport.baseAddress = sliderValue * sliderUnit;
        std::stringstream ss;
        ss << "0x" << std::hex << viewport.baseAddress;
        strcpy(addressInput, ss.str().c_str());
        needsUpdate = true;
    }
    
    // + button  
    if (ImGui::Button("+64K", ImVec2(-1, 30))) {
        if (viewport.baseAddress + sliderUnit <= maxAddress) {
            viewport.baseAddress += sliderUnit;
            std::stringstream ss;
            ss << "0x" << std::hex << viewport.baseAddress;
            strcpy(addressInput, ss.str().c_str());
            needsUpdate = true;
        }
    }
    
    ImGui::PopItemWidth();
    
    // Current address display
    ImGui::Separator();
    ImGui::Text("Current:");
    ImGui::Text("0x%llx", viewport.baseAddress);
}

void MemoryVisualizer::DrawMemoryView() {
    // Create a scrollable child region
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    
    // Reserve space for correlation stripe if enabled
    float correlationHeight = showCorrelation ? 100.0f : 0.0f;
    float memoryHeight = availSize.y - correlationHeight;
    
    // Add vertical scrollbar on the right - use memoryHeight not availSize.y!
    ImGui::BeginChild("MemoryScrollRegion", ImVec2(availSize.x, memoryHeight), false, 
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
    
    // The actual canvas size should be larger than viewport for scrolling
    float canvasHeight = std::max(availSize.y, (float)(viewport.height * viewport.zoom));
    float canvasWidth = std::max(availSize.x - 20, (float)(viewport.width * viewport.zoom)); // -20 for scrollbar
    
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(canvasPos, 
                            ImVec2(canvasPos.x + canvasWidth, canvasPos.y + canvasHeight),
                            IM_COL32(50, 50, 50, 255));
    
    if (memoryTexture && !pixelBuffer.empty()) {
        float texW = viewport.width * viewport.zoom;
        float texH = viewport.height * viewport.zoom;
        
        ImVec2 imgPos(canvasPos.x, canvasPos.y);
        ImVec2 imgSize(imgPos.x + texW, imgPos.y + texH);
        
        // Save position for magnifier
        memoryViewPos = imgPos;
        memoryViewSize = ImVec2(texW, texH);
        
        drawList->PushClipRect(canvasPos, 
                              ImVec2(canvasPos.x + canvasWidth, canvasPos.y + canvasHeight),
                              true);
        
        // Use current texture state (force refresh)
        glBindTexture(GL_TEXTURE_2D, memoryTexture);
        
        drawList->AddImage((ImTextureID)(intptr_t)memoryTexture,
                          imgPos, imgSize,
                          ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(255, 255, 255, 255));
        
        // Draw marching ants around accumulated changed regions (if enabled)
        if (showChangeHighlight && !changeHistory.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<float>(now - lastChangeTime).count();
            
            // Clear old history after 3 seconds of no changes
            if (elapsed > 3.0f) {
                changeHistory.clear();
            } else {
                // Calculate fade based on time since last change
                // Show for 2 seconds, then fade for 1 second
                float alpha = 1.0f;
                if (elapsed > 2.0f) {
                    alpha = 1.0f - ((elapsed - 2.0f) / 1.0f);
                }
                
                // Draw yellow boxes around each individual region
                // Collect all regions from history and merge overlapping ones
                std::vector<ChangeRegion> allRegions;
                for (const auto& frame : changeHistory) {
                    for (const auto& region : frame) {
                        // Check if this region overlaps with existing ones and merge if so
                        bool merged = false;
                        for (auto& existing : allRegions) {
                            // Check for overlap or adjacency (within 2 pixels to avoid crowding)
                            int gap = 2;  // Minimum gap between separate boxes
                            bool closeY = abs(existing.y - region.y) <= gap && 
                                         abs((existing.y + existing.height) - (region.y + region.height)) <= gap;
                            bool closeX = abs(existing.x - region.x) <= gap && 
                                         abs((existing.x + existing.width) - (region.x + region.width)) <= gap;
                            
                            if (closeY && closeX) {
                                // Merge nearby regions to avoid visual clutter
                                int newX = std::min(existing.x, region.x);
                                int newY = std::min(existing.y, region.y);
                                int newRight = std::max(existing.x + existing.width, region.x + region.width);
                                int newBottom = std::max(existing.y + existing.height, region.y + region.height);
                                existing.x = newX;
                                existing.y = newY;
                                existing.width = newRight - newX;
                                existing.height = newBottom - newY;
                                merged = true;
                                break;
                            }
                        }
                        if (!merged) {
                            allRegions.push_back(region);
                        }
                    }
                }
                
                // Draw yellow highlight boxes around each region
                for (const auto& region : allRegions) {
                    // Draw box OUTSIDE the changed pixels with small margin
                    float margin = 1.0f;  // Single pixel margin to not cover content
                    float boxX = imgPos.x + (region.x * viewport.zoom) - margin;
                    float boxY = imgPos.y + (region.y * viewport.zoom) - margin;
                    float boxW = (region.width * viewport.zoom) + (2 * margin);
                    float boxH = (region.height * viewport.zoom) + (2 * margin);
                    
                    // Yellow color with current alpha
                    uint32_t color = IM_COL32(255, 255, 0, (uint8_t)(255 * alpha));
                    
                    // Draw rectangle outline
                    drawList->AddRect(
                        ImVec2(boxX, boxY),
                        ImVec2(boxX + boxW, boxY + boxH),
                        color,
                        0.0f,  // No rounding
                        0,     // All corners
                        2.0f   // 2 pixel thick border
                    );
                }  // End of for each region
            }
        }
        
        drawList->PopClipRect();
    }
    
    // Make the invisible button the size of the content for scrolling
    ImGui::InvisibleButton("canvas", ImVec2(canvasWidth, canvasHeight));
    
    HandleInput();
    
    // Handle vertical scrollbar
    float scrollY = ImGui::GetScrollY();
    float maxScrollY = ImGui::GetScrollMaxY();
    if (maxScrollY > 0) {
        // Map scroll position to memory address
        float scrollRatio = scrollY / maxScrollY;
        int64_t addressRange = 0x100000; // 1MB view range for now
        int64_t scrollOffset = (int64_t)(scrollRatio * addressRange);
        
        // This will be used to offset the memory view
        // For now just track it, actual implementation would update base address
    }
    
    ImGui::EndChild();
    
    // Draw correlation stripe at bottom if enabled
    if (showCorrelation && !currentMemory.data.empty()) {
        DrawCorrelationStripe();
    }
    
    // Draw magnifier window if enabled
    if (showMagnifier) {
        DrawMagnifier();
    }
}

void MemoryVisualizer::DrawMagnifier() {
    // Create a floating window for the magnifier
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
    
    // Start with a reasonable position, but let user move it
    ImGui::SetNextWindowSize(ImVec2(300, 350), ImGuiCond_FirstUseEver);
    
    if (!ImGui::Begin("Magnifier", &showMagnifier, windowFlags)) {
        ImGui::End();
        return;
    }
    
    // Controls - specific zoom levels for better usability
    ImGui::Text("Zoom:");
    ImGui::SameLine();
    
    // Quick zoom buttons for common levels
    int zoomLevels[] = {2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32};
    for (int i = 0; i < sizeof(zoomLevels)/sizeof(zoomLevels[0]); i++) {
        if (i > 0) ImGui::SameLine();
        
        // Highlight current zoom level
        if (magnifierZoom == zoomLevels[i]) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        }
        
        char label[8];
        snprintf(label, sizeof(label), "%dx", zoomLevels[i]);
        if (ImGui::SmallButton(label)) {
            magnifierZoom = zoomLevels[i];
        }
        
        if (magnifierZoom == zoomLevels[i]) {
            ImGui::PopStyleColor();
        }
        
        // Line break after 8x for second row
        if (zoomLevels[i] == 8) {
            ImGui::Text("     ");  // Indent second row
            ImGui::SameLine();
        }
    }
    
    // Get mouse position relative to the actual memory texture (not magnifier window)
    ImVec2 mousePos = ImGui::GetMousePos();
    
    // Calculate which pixel we're over in the main memory view
    // memoryViewPos contains the screen position of the memory texture
    int srcX = (mousePos.x - memoryViewPos.x) / viewport.zoom;
    int srcY = (mousePos.y - memoryViewPos.y) / viewport.zoom;
    
    // Size of magnified area
    int halfSize = magnifierSize / 2;
    ImVec2 magnifiedSize(magnifierSize * magnifierZoom, magnifierSize * magnifierZoom);
    
    // Draw the magnified view
    ImVec2 drawPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(drawPos, 
                            ImVec2(drawPos.x + magnifiedSize.x, drawPos.y + magnifiedSize.y),
                            IM_COL32(30, 30, 30, 255));
    
    // Draw each pixel magnified
    for (int dy = -halfSize; dy < halfSize; dy++) {
        for (int dx = -halfSize; dx < halfSize; dx++) {
            int px = srcX + dx;
            int py = srcY + dy;
            
            if (px >= 0 && px < viewport.width && py >= 0 && py < viewport.height) {
                uint32_t pixel = GetPixelAt(px, py);
                
                // Draw magnified pixel - just show exactly what's in the pixel buffer
                float x1 = drawPos.x + (dx + halfSize) * magnifierZoom;
                float y1 = drawPos.y + (dy + halfSize) * magnifierZoom;
                float x2 = x1 + magnifierZoom;
                float y2 = y1 + magnifierZoom;
                
                drawList->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), pixel);
                
                // Draw grid lines for zoom >= 4x
                if (magnifierZoom >= 4) {
                    uint32_t gridColor = IM_COL32(100, 100, 100, 100);
                    drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), gridColor, 0.0f, 0, 1.0f);
                }
            }
        }
    }
    
    // Draw center crosshair
    uint32_t crosshairColor = IM_COL32(255, 255, 0, 200);
    float centerX = drawPos.x + magnifiedSize.x / 2;
    float centerY = drawPos.y + magnifiedSize.y / 2;
    drawList->AddLine(ImVec2(centerX - 10, centerY), ImVec2(centerX + 10, centerY), crosshairColor);
    drawList->AddLine(ImVec2(centerX, centerY - 10), ImVec2(centerX, centerY + 10), crosshairColor);
    
    // Make space for the drawn content
    ImGui::Dummy(magnifiedSize);
    
    // Show info about center pixel
    uint64_t addr = GetAddressAt(srcX, srcY);
    ImGui::Text("Center: (%d, %d)", srcX, srcY);
    ImGui::Text("Address: 0x%llx", addr);
    
    // Get the pixel value at center
    if (!currentMemory.data.empty()) {
        size_t offset = srcY * viewport.stride + srcX * viewport.format.bytesPerPixel;
        if (offset < currentMemory.data.size()) {
            switch (viewport.format.type) {
                case PixelFormat::RGB888:
                    if (offset + 2 < currentMemory.data.size()) {
                        ImGui::Text("RGB: %02X %02X %02X", 
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2]);
                    }
                    break;
                case PixelFormat::RGBA8888:
                    if (offset + 3 < currentMemory.data.size()) {
                        ImGui::Text("RGBA: %02X %02X %02X %02X",
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2],
                                   currentMemory.data[offset + 3]);
                    }
                    break;
                case PixelFormat::BGR888:
                    if (offset + 2 < currentMemory.data.size()) {
                        ImGui::Text("BGR: %02X %02X %02X", 
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2]);
                    }
                    break;
                case PixelFormat::BGRA8888:
                    if (offset + 3 < currentMemory.data.size()) {
                        ImGui::Text("BGRA: %02X %02X %02X %02X",
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2],
                                   currentMemory.data[offset + 3]);
                    }
                    break;
                case PixelFormat::ARGB8888:
                    if (offset + 3 < currentMemory.data.size()) {
                        ImGui::Text("ARGB: %02X %02X %02X %02X",
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2],
                                   currentMemory.data[offset + 3]);
                    }
                    break;
                case PixelFormat::ABGR8888:
                    if (offset + 3 < currentMemory.data.size()) {
                        ImGui::Text("ABGR: %02X %02X %02X %02X",
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2],
                                   currentMemory.data[offset + 3]);
                    }
                    break;
                case PixelFormat::RGB565:
                    if (offset + 1 < currentMemory.data.size()) {
                        uint16_t val = (currentMemory.data[offset] << 8) | currentMemory.data[offset + 1];
                        ImGui::Text("RGB565: 0x%04X", val);
                    }
                    break;
                default:
                    ImGui::Text("Value: 0x%02X", currentMemory.data[offset]);
                    break;
            }
        }
    }
    
    ImGui::End();
}

void MemoryVisualizer::DrawCorrelationStripe() {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(ImGui::GetContentRegionAvail().x, 100);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                            IM_COL32(20, 20, 20, 255));
    
    // Compute correlation if we have memory
    if (!currentMemory.data.empty()) {
        auto correlation = correlator.Correlate(currentMemory.data.data(), 
                                               currentMemory.data.size(), 
                                               pixelFormatIndex);
        
        if (!correlation.empty()) {
            // Draw correlation graph
            float xScale = size.x / std::min((size_t)2048, correlation.size());
            float yScale = size.y * 0.8f;  // Use 80% of height
            float baseline = pos.y + size.y - 10;
            
            // Draw grid lines
            for (int x = 64; x < 2048; x += 64) {
                float xPos = pos.x + x * xScale;
                drawList->AddLine(ImVec2(xPos, pos.y), 
                                 ImVec2(xPos, pos.y + size.y),
                                 IM_COL32(40, 40, 40, 255));
                
                // Label major widths
                if (x % 256 == 0) {
                    char label[32];
                    snprintf(label, sizeof(label), "%d", x);
                    drawList->AddText(ImVec2(xPos - 10, pos.y + size.y - 8),
                                     IM_COL32(128, 128, 128, 255), label);
                }
            }
            
            // Draw correlation curve
            ImVec2 prevPoint(pos.x, baseline);
            for (size_t i = 0; i < std::min((size_t)2048, correlation.size()); i++) {
                float x = pos.x + i * xScale;
                float y = baseline - correlation[i] * yScale;
                
                ImVec2 curPoint(x, y);
                drawList->AddLine(prevPoint, curPoint, IM_COL32(0, 255, 128, 255), 1.5f);
                prevPoint = curPoint;
            }
            
            // Find and mark peaks
            auto peaks = correlator.FindPeaks(correlation, 0.3);
            for (int peak : peaks) {
                float x = pos.x + peak * xScale;
                drawList->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y),
                                 IM_COL32(255, 255, 0, 128), 2.0f);
                
                // Label the peak
                char label[32];
                snprintf(label, sizeof(label), "%d", peak);
                drawList->AddText(ImVec2(x + 2, pos.y + 2),
                                 IM_COL32(255, 255, 0, 255), label);
            }
        }
    }
    
    // Label
    drawList->AddText(ImVec2(pos.x + 5, pos.y + 5),
                     IM_COL32(200, 200, 200, 255), "Autocorrelation (Width Detection)");
}

void MemoryVisualizer::HandleInput() {
    ImGuiIO& io = ImGui::GetIO();
    
    if (ImGui::IsItemHovered()) {
        // Mouse wheel scrolls through memory addresses
        if (io.MouseWheel != 0) {
            // Scroll by rows worth of memory
            int64_t scrollDelta = io.MouseWheel * viewport.stride * 16;  // 16 rows at a time
            
            // Shift for faster scrolling (64K chunks)
            if (io.KeyShift) {
                scrollDelta = io.MouseWheel * 65536;
            }
            
            // Update address - natural scrolling: wheel up = go up (earlier/lower addresses)
            int64_t newAddress = (int64_t)viewport.baseAddress - scrollDelta;  // Natural scrolling
            if (newAddress < 0) newAddress = 0;
            if (newAddress > 0x200000000ULL) newAddress = 0x200000000ULL;  // Cap at 8GB
            
            viewport.baseAddress = (uint64_t)newAddress;
            
            // Update the address input field
            std::stringstream ss;
            ss << "0x" << std::hex << viewport.baseAddress;
            strcpy(addressInput, ss.str().c_str());
            
            needsUpdate = true;
        }
        
        // Drag to scroll through memory - "mouse sticks to paper" in both X and Y
        if (ImGui::IsMouseDragging(0)) {
            ImVec2 delta = ImGui::GetMouseDragDelta(0);
            
            if (!isDragging) {
                // Start of drag
                isDragging = true;
                dragStartX = 0;
                dragStartY = 0;
            }
            
            // Calculate memory offset from drag
            float dragDeltaX = delta.x - dragStartX;
            float dragDeltaY = delta.y - dragStartY;
            
            // Vertical: Each pixel of drag = one row of memory
            // Horizontal: Each pixel of drag = one byte (or pixel format size)
            int64_t verticalDelta = (int64_t)dragDeltaY * viewport.stride;
            int64_t horizontalDelta = (int64_t)dragDeltaX * viewport.format.bytesPerPixel;
            
            int64_t totalDelta = verticalDelta + horizontalDelta;
            
            if (totalDelta != 0) {
                int64_t newAddress = (int64_t)viewport.baseAddress - totalDelta;
                if (newAddress < 0) newAddress = 0;
                if (newAddress > 0x200000000ULL) newAddress = 0x200000000ULL;
                
                viewport.baseAddress = (uint64_t)newAddress;
                
                // Update the address input field
                std::stringstream ss;
                ss << "0x" << std::hex << viewport.baseAddress;
                strcpy(addressInput, ss.str().c_str());
                
                dragStartX = delta.x;  // Reset drag start for continuous scrolling
                dragStartY = delta.y;
                needsUpdate = true;
            }
        } else {
            isDragging = false;
        }
        
        if (ImGui::IsMouseClicked(0)) {
            isDragging = true;
            dragStartX = io.MousePos.x - viewport.panX;
            dragStartY = io.MousePos.y - viewport.panY;
        }
    }
    
    if (isDragging) {
        if (ImGui::IsMouseDragging(0)) {
            viewport.panX = io.MousePos.x - dragStartX;
            viewport.panY = io.MousePos.y - dragStartY;
        }
        
        if (ImGui::IsMouseReleased(0)) {
            isDragging = false;
        }
    }
}

void MemoryVisualizer::UpdateTexture() {
    auto newPixels = ConvertMemoryToPixels(currentMemory);
    
    if (!newPixels.empty()) {
        // Check if pixels actually changed
        static uint32_t lastPixelChecksum = 0;
        uint32_t checksum = 0;
        for (size_t i = 0; i < newPixels.size(); i++) {
            checksum ^= newPixels[i];
        }
        
        // if (checksum != lastPixelChecksum && lastPixelChecksum != 0) {
        //     std::cerr << "PIXELS CHANGED! Checksum: 0x" << std::hex << checksum << std::dec << "\n";
        // }
        lastPixelChecksum = checksum;
        
        pixelBuffer = std::move(newPixels);
        
        glBindTexture(GL_TEXTURE_2D, memoryTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                    viewport.width, viewport.height,
                    0, GL_RGBA, GL_UNSIGNED_BYTE, pixelBuffer.data());
        glFlush();  // Force GPU to process the texture update
    }
}

std::vector<uint32_t> MemoryVisualizer::ConvertMemoryToPixels(const MemoryBlock& memory) {
    std::vector<uint32_t> pixels(viewport.width * viewport.height, 0xFF000000);
    
    if (memory.data.empty()) {
        return pixels;
    }
    
    size_t dataIndex = 0;
    
    for (size_t y = 0; y < viewport.height; ++y) {
        for (size_t x = 0; x < viewport.width; ++x) {
            size_t pixelIndex = y * viewport.width + x;
            size_t memIndex = y * viewport.stride + x * viewport.format.bytesPerPixel;
            
            if (memIndex >= memory.data.size()) {
                continue;
            }
            
            switch (viewport.format.type) {
                case PixelFormat::RGB888:
                    if (memIndex + 2 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex],
                            memory.data[memIndex + 1],
                            memory.data[memIndex + 2]
                        );
                    }
                    break;
                    
                case PixelFormat::RGBA8888:
                    if (memIndex + 3 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex],
                            memory.data[memIndex + 1],
                            memory.data[memIndex + 2],
                            memory.data[memIndex + 3]
                        );
                    }
                    break;
                    
                case PixelFormat::BGR888:
                    if (memIndex + 2 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex + 2],  // B -> R
                            memory.data[memIndex + 1],  // G -> G
                            memory.data[memIndex]        // R -> B
                        );
                    }
                    break;
                    
                case PixelFormat::BGRA8888:
                    if (memIndex + 3 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex + 2],  // B -> R
                            memory.data[memIndex + 1],  // G -> G
                            memory.data[memIndex],      // R -> B
                            memory.data[memIndex + 3]   // A -> A
                        );
                    }
                    break;
                    
                case PixelFormat::ARGB8888:
                    if (memIndex + 3 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex + 1],  // A R G B -> R
                            memory.data[memIndex + 2],  // G
                            memory.data[memIndex + 3],  // B
                            memory.data[memIndex]        // A
                        );
                    }
                    break;
                    
                case PixelFormat::ABGR8888:
                    if (memIndex + 3 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex + 3],  // A B G R -> R
                            memory.data[memIndex + 2],  // G
                            memory.data[memIndex + 1],  // B
                            memory.data[memIndex]        // A
                        );
                    }
                    break;
                    
                case PixelFormat::GRAYSCALE:
                    if (memIndex < memory.data.size()) {
                        uint8_t gray = memory.data[memIndex];
                        pixels[pixelIndex] = PackRGBA(gray, gray, gray);
                    }
                    break;
                    
                case PixelFormat::RGB565:
                    if (memIndex + 1 < memory.data.size()) {
                        uint16_t rgb565 = (memory.data[memIndex] << 8) | memory.data[memIndex + 1];
                        uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;
                        uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;
                        uint8_t b = (rgb565 & 0x1F) << 3;
                        pixels[pixelIndex] = PackRGBA(r, g, b);
                    }
                    break;
                    
                case PixelFormat::BINARY:
                    if (memIndex / 8 < memory.data.size()) {
                        uint8_t byte = memory.data[memIndex / 8];
                        uint8_t bit = (byte >> (7 - (memIndex % 8))) & 1;
                        uint8_t value = bit ? 255 : 0;
                        pixels[pixelIndex] = PackRGBA(value, value, value);
                    }
                    break;
            }
        }
    }
    
    return pixels;
}

uint32_t MemoryVisualizer::GetPixelAt(int x, int y) const {
    if (x < 0 || x >= viewport.width || y < 0 || y >= viewport.height) {
        return 0;
    }
    
    size_t index = y * viewport.width + x;
    if (index < pixelBuffer.size()) {
        return pixelBuffer[index];
    }
    
    return 0;
}

uint64_t MemoryVisualizer::GetAddressAt(int x, int y) const {
    size_t offset = y * viewport.stride + x * viewport.format.bytesPerPixel;
    return viewport.baseAddress + offset;
}

void MemoryVisualizer::NavigateToAddress(uint64_t address) {
    viewport.baseAddress = address;
    std::stringstream ss;
    ss << "0x" << std::hex << address;
    strcpy(addressInput, ss.str().c_str());
    needsUpdate = true;
}

void MemoryVisualizer::SetViewport(const ViewportSettings& settings) {
    viewport = settings;
    widthInput = settings.width;
    heightInput = settings.height;
    strideInput = settings.stride;
    pixelFormatIndex = settings.format.type;
    needsUpdate = true;
}

void MemoryVisualizer::DrawNavigator() {
}

}