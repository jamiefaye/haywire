#include "memory_visualizer.h"
#include "qemu_connection.h"
#include "imgui.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace Haywire {

MemoryVisualizer::MemoryVisualizer() 
    : memoryTexture(0), needsUpdate(true), autoRefresh(false),
      refreshRate(10.0f), showHexOverlay(false), showNavigator(true),
      widthInput(256), heightInput(256), strideInput(256),
      pixelFormatIndex(0), mouseX(0), mouseY(0), isDragging(false),
      dragStartX(0), dragStartY(0) {
    
    strcpy(addressInput, "0x0");
    CreateTexture();
    lastRefresh = std::chrono::steady_clock::now();
}

MemoryVisualizer::~MemoryVisualizer() {
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

void MemoryVisualizer::Draw(QemuConnection& qemu) {
    ImGui::Columns(2, "VisualizerColumns", true);
    ImGui::SetColumnWidth(0, 300);
    
    DrawControls();
    
    if (ImGui::Button("Read Memory") && qemu.IsConnected()) {
        size_t size = viewport.stride * viewport.height;
        std::vector<uint8_t> buffer;
        
        if (qemu.ReadMemory(viewport.baseAddress, size, buffer)) {
            currentMemory.address = viewport.baseAddress;
            currentMemory.data = std::move(buffer);
            currentMemory.stride = viewport.stride;
            needsUpdate = true;
        }
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("Auto Refresh", &autoRefresh);
    
    if (autoRefresh) {
        ImGui::SliderFloat("Refresh Rate", &refreshRate, 1.0f, 60.0f, "%.1f Hz");
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - lastRefresh).count();
        
        if (elapsed >= 1.0f / refreshRate && qemu.IsConnected()) {
            size_t size = viewport.stride * viewport.height;
            std::vector<uint8_t> buffer;
            
            if (qemu.ReadMemory(viewport.baseAddress, size, buffer)) {
                currentMemory.address = viewport.baseAddress;
                currentMemory.data = std::move(buffer);
                currentMemory.stride = viewport.stride;
                needsUpdate = true;
                lastRefresh = now;
            }
        }
    }
    
    ImGui::NextColumn();
    
    DrawMemoryView();
    
    ImGui::Columns(1);
    
    if (needsUpdate) {
        UpdateTexture();
        needsUpdate = false;
    }
}

void MemoryVisualizer::DrawControls() {
    ImGui::Text("Memory Settings");
    ImGui::Separator();
    
    ImGui::InputText("Address", addressInput, sizeof(addressInput));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        std::stringstream ss;
        ss << std::hex << addressInput;
        ss >> viewport.baseAddress;
    }
    
    ImGui::InputInt("Width", &widthInput);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        viewport.width = std::max(1, widthInput);
    }
    
    ImGui::InputInt("Height", &heightInput);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        viewport.height = std::max(1, heightInput);
    }
    
    ImGui::InputInt("Stride", &strideInput);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        viewport.stride = std::max(1, strideInput);
    }
    
    const char* formats[] = { "RGB888", "RGBA8888", "RGB565", "Grayscale", "Binary" };
    if (ImGui::Combo("Pixel Format", &pixelFormatIndex, formats, IM_ARRAYSIZE(formats))) {
        viewport.format = PixelFormat(static_cast<PixelFormat::Type>(pixelFormatIndex));
    }
    
    ImGui::SliderFloat("Zoom", &viewport.zoom, 0.1f, 10.0f, "%.2fx");
    
    if (ImGui::Button("Reset View")) {
        viewport.zoom = 1.0f;
        viewport.panX = 0;
        viewport.panY = 0;
    }
    
    ImGui::Separator();
    ImGui::Text("Display Options");
    ImGui::Checkbox("Hex Overlay", &showHexOverlay);
    ImGui::Checkbox("Navigator", &showNavigator);
    
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

void MemoryVisualizer::DrawMemoryView() {
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    drawList->AddRectFilled(canvasPos, 
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(50, 50, 50, 255));
    
    if (memoryTexture && !pixelBuffer.empty()) {
        float texW = viewport.width * viewport.zoom;
        float texH = viewport.height * viewport.zoom;
        
        ImVec2 imgPos(canvasPos.x + viewport.panX, canvasPos.y + viewport.panY);
        ImVec2 imgSize(imgPos.x + texW, imgPos.y + texH);
        
        drawList->PushClipRect(canvasPos, 
                              ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                              true);
        
        drawList->AddImage((ImTextureID)(intptr_t)memoryTexture,
                          imgPos, imgSize,
                          ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(255, 255, 255, 255));
        
        drawList->PopClipRect();
    }
    
    HandleInput();
    
    ImGui::InvisibleButton("canvas", canvasSize);
}

void MemoryVisualizer::HandleInput() {
    ImGuiIO& io = ImGui::GetIO();
    
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0) {
            float oldZoom = viewport.zoom;
            viewport.zoom *= (io.MouseWheel > 0) ? 1.1f : 0.9f;
            viewport.zoom = std::max(0.1f, std::min(10.0f, viewport.zoom));
            
            float zoomRatio = viewport.zoom / oldZoom;
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 canvasPos = ImGui::GetCursorScreenPos();
            
            viewport.panX = mousePos.x - canvasPos.x - (mousePos.x - canvasPos.x - viewport.panX) * zoomRatio;
            viewport.panY = mousePos.y - canvasPos.y - (mousePos.y - canvasPos.y - viewport.panY) * zoomRatio;
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