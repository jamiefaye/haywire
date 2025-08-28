#pragma once

#include "common.h"
#include "imgui.h"

namespace Haywire {

class MemoryVisualizer;

class HexOverlay {
public:
    HexOverlay();
    ~HexOverlay();
    
    void Draw(const MemoryVisualizer& visualizer);
    
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    bool IsEnabled() const { return enabled_; }
    
    void SetFontSize(float size) { fontSize = size; }
    float GetFontSize() const { return fontSize; }
    
    void SetDisplayFormat(int format) { displayFormat = format; }
    
    enum DisplayFormat {
        HEX_8BIT,
        HEX_16BIT,
        HEX_32BIT,
        DECIMAL,
        ASCII
    };
    
private:
    void DrawHexValue(ImDrawList* drawList, float x, float y, 
                      uint32_t value, uint32_t bgColor);
    
    bool enabled_;
    float fontSize;
    int displayFormat;
    
    float minZoomLevel;
    bool autoHide;
    bool useContrastColors;
};

}