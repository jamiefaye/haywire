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
    : memoryTexture(0), needsUpdate(true), autoRefresh(false),
      refreshRate(10.0f), showHexOverlay(false), showNavigator(true),
      widthInput(640), heightInput(480), strideInput(640),  // Back to reasonable video dimensions
      pixelFormatIndex(0), mouseX(0), mouseY(0), isDragging(false),
      dragStartX(0), dragStartY(0), isReading(false), readComplete(false) {
    
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
        needsUpdate = true;
        readStatus = "Read complete";
    }
    
    // Show read button or status
    if (isReading) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Reading memory...");
    } else {
        if (ImGui::Button("Read Memory") && qemu.IsConnected()) {
            // Stop any previous thread
            if (readThread.joinable()) {
                readThread.join();
            }
            
            // Start async read
            isReading = true;
            readStatus = "Starting read...";
            
            size_t size = viewport.stride * viewport.height;
            uint64_t addr = viewport.baseAddress;
            int stride = viewport.stride;
            
            readThread = std::thread([this, &qemu, addr, size, stride]() {
                std::vector<uint8_t> buffer;
                // std::cerr << "Reading from address: 0x" << std::hex << addr << " size: " << std::dec << size << "\n";
                
                if (qemu.ReadMemory(addr, size, buffer)) {
                    // Log first 16 bytes to see if content is consistent
                    // std::cerr << "First 16 bytes: ";
                    // for (size_t i = 0; i < std::min(size_t(16), buffer.size()); i++) {
                    //     std::cerr << std::hex << std::setw(2) << std::setfill('0') << (int)buffer[i] << " ";
                    // }
                    // std::cerr << std::dec << "\n";
                    
                    std::lock_guard<std::mutex> lock(memoryMutex);
                    pendingMemory.address = addr;
                    pendingMemory.data = std::move(buffer);
                    pendingMemory.stride = stride;
                    readComplete = true;
                } else {
                    readStatus = "Read failed";
                }
                
                isReading = false;
            });
        }
    }
    
    if (!readStatus.empty()) {
        ImGui::SameLine();
        ImGui::Text("%s", readStatus.c_str());
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("Auto Refresh", &autoRefresh);
    
    if (autoRefresh) {
        ImGui::SliderFloat("Refresh Rate", &refreshRate, 1.0f, 60.0f, "%.1f Hz");
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - lastRefresh).count();
        
        if (elapsed >= 1.0f / refreshRate && qemu.IsConnected() && !isReading) {
            // Start async read for auto-refresh
            if (readThread.joinable()) {
                readThread.join();
            }
            
            // Don't clear during auto-refresh to avoid flicker
            isReading = true;
            size_t size = viewport.stride * viewport.height;
            uint64_t addr = viewport.baseAddress;
            int stride = viewport.stride;
            
            readThread = std::thread([this, &qemu, addr, size, stride]() {
                std::vector<uint8_t> buffer;
                // std::cerr << "Reading from address: 0x" << std::hex << addr << " size: " << std::dec << size << "\n";
                
                if (qemu.ReadMemory(addr, size, buffer)) {
                    // Log first 16 bytes to see if content is consistent
                    // std::cerr << "First 16 bytes: ";
                    // for (size_t i = 0; i < std::min(size_t(16), buffer.size()); i++) {
                    //     std::cerr << std::hex << std::setw(2) << std::setfill('0') << (int)buffer[i] << " ";
                    // }
                    // std::cerr << std::dec << "\n";
                    
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
    
    // Only update texture once when we have complete data
    if (needsUpdate) {
        UpdateTexture();
        needsUpdate = false;
    }
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
    const char* formats[] = { "RGB888", "RGBA8888", "RGB565", "Grayscale", "Binary" };
    ImGui::PushItemWidth(100);
    if (ImGui::Combo("##Format", &pixelFormatIndex, formats, IM_ARRAYSIZE(formats))) {
        viewport.format = PixelFormat(static_cast<PixelFormat::Type>(pixelFormatIndex));
        needsUpdate = true;  // Immediate update
    }
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    ImGui::Checkbox("Hex", &showHexOverlay);
    
    ImGui::SameLine();
    ImGui::Text("Refresh:");
    ImGui::SameLine();
    int refreshInt = (int)refreshRate;
    ImGui::PushItemWidth(50);
    if (ImGui::InputInt("##RefreshRate", &refreshInt)) {
        refreshRate = (float)std::max(1, std::min(60, refreshInt));
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::Text("Hz");
    
    // Address slider removed from here - moved to side of bitmap window
    
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
    const uint64_t maxAddress = 0x100000000ULL;  // 4GB range
    
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
    
    // Add vertical scrollbar on the right
    ImGui::BeginChild("MemoryScrollRegion", availSize, false, 
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
        
        drawList->PushClipRect(canvasPos, 
                              ImVec2(canvasPos.x + canvasWidth, canvasPos.y + canvasHeight),
                              true);
        
        drawList->AddImage((ImTextureID)(intptr_t)memoryTexture,
                          imgPos, imgSize,
                          ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(255, 255, 255, 255));
        
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
            if (newAddress > 0xFFFFFFFFULL) newAddress = 0xFFFFFFFFULL;  // Cap at 4GB for now
            
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
                if (newAddress > 0xFFFFFFFFULL) newAddress = 0xFFFFFFFFULL;
                
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
    pixelBuffer = ConvertMemoryToPixels(currentMemory);
    
    if (!pixelBuffer.empty()) {
        glBindTexture(GL_TEXTURE_2D, memoryTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                    viewport.width, viewport.height,
                    0, GL_RGBA, GL_UNSIGNED_BYTE, pixelBuffer.data());
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