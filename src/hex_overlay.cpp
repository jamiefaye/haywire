#include "hex_overlay.h"
#include "memory_visualizer.h"
#include <sstream>
#include <iomanip>

namespace Haywire {

HexOverlay::HexOverlay() 
    : enabled_(false), fontSize(12.0f), displayFormat(HEX_8BIT),
      minZoomLevel(2.0f), autoHide(true), useContrastColors(true) {
}

HexOverlay::~HexOverlay() {
}

void HexOverlay::Draw(const MemoryVisualizer& visualizer) {
    if (!enabled_) {
        return;
    }
    
    ViewportSettings viewport = visualizer.GetViewport();
    
    if (autoHide && viewport.zoom < minZoomLevel) {
        return;
    }
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    
    const MemoryBlock& memory = visualizer.GetCurrentMemory();
    if (memory.data.empty()) {
        return;
    }
    
    float cellWidth = viewport.zoom;
    float cellHeight = viewport.zoom;
    
    int startX = std::max(0, (int)(-viewport.panX / cellWidth));
    int startY = std::max(0, (int)(-viewport.panY / cellHeight));
    int endX = std::min((int)viewport.width, (int)((ImGui::GetContentRegionAvail().x - viewport.panX) / cellWidth) + 1);
    int endY = std::min((int)viewport.height, (int)((ImGui::GetContentRegionAvail().y - viewport.panY) / cellHeight) + 1);
    
    for (int y = startY; y < endY; ++y) {
        for (int x = startX; x < endX; ++x) {
            float screenX = canvasPos.x + viewport.panX + x * cellWidth;
            float screenY = canvasPos.y + viewport.panY + y * cellHeight;
            
            size_t memIndex = y * viewport.stride + x * viewport.format.bytesPerPixel;
            
            if (memIndex >= memory.data.size()) {
                continue;
            }
            
            uint32_t value = 0;
            std::stringstream ss;
            
            switch (displayFormat) {
                case HEX_8BIT:
                    value = memory.data[memIndex];
                    ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << value;
                    break;
                    
                case HEX_16BIT:
                    if (memIndex + 1 < memory.data.size()) {
                        value = (memory.data[memIndex] << 8) | memory.data[memIndex + 1];
                        ss << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << value;
                    }
                    break;
                    
                case HEX_32BIT:
                    if (memIndex + 3 < memory.data.size()) {
                        value = (memory.data[memIndex] << 24) | 
                               (memory.data[memIndex + 1] << 16) |
                               (memory.data[memIndex + 2] << 8) |
                               memory.data[memIndex + 3];
                        ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
                    }
                    break;
                    
                case DECIMAL:
                    value = memory.data[memIndex];
                    ss << value;
                    break;
                    
                case ASCII:
                    value = memory.data[memIndex];
                    if (value >= 32 && value < 127) {
                        ss << (char)value;
                    } else {
                        ss << ".";
                    }
                    break;
            }
            
            std::string text = ss.str();
            if (!text.empty()) {
                uint32_t bgColor = visualizer.GetPixelAt(x, y);
                DrawHexValue(drawList, screenX, screenY, value, bgColor);
            }
        }
    }
}

void HexOverlay::DrawHexValue(ImDrawList* drawList, float x, float y, 
                              uint32_t value, uint32_t bgColor) {
    std::stringstream ss;
    
    switch (displayFormat) {
        case HEX_8BIT:
            ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (value & 0xFF);
            break;
        case HEX_16BIT:
            ss << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << (value & 0xFFFF);
            break;
        case HEX_32BIT:
            ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
            break;
        case DECIMAL:
            ss << (value & 0xFF);
            break;
        case ASCII:
            if ((value & 0xFF) >= 32 && (value & 0xFF) < 127) {
                ss << (char)(value & 0xFF);
            } else {
                ss << ".";
            }
            break;
    }
    
    std::string text = ss.str();
    
    uint32_t textColor = useContrastColors ? ContrastColor(bgColor) : 0xFFFFFFFF;
    
    ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    ImVec2 textPos(x + 2, y + 2);
    
    drawList->AddText(textPos, textColor, text.c_str());
}

}