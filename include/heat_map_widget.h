#ifndef HAYWIRE_HEAT_MAP_WIDGET_H
#define HAYWIRE_HEAT_MAP_WIDGET_H

#include <cstdint>
#include <functional>

namespace Haywire {

class ChangeDetector;
struct ChunkInfo;

// ImGui widget that displays memory change heat map
class HeatMapWidget {
public:
    explicit HeatMapWidget(ChangeDetector* detector);
    ~HeatMapWidget();

    // Draw the heat map widget in ImGui context
    // Returns true if user clicked to jump to an address
    bool Draw(float width, float height, uint64_t current_offset, uint64_t view_size);

    // Configuration
    void SetPixelSize(int pixels_per_chunk) { pixel_size_ = pixels_per_chunk; }
    void SetZoomLevel(int level);  // 0=1px, 1=2px, 2=4px, 3=6px per chunk
    int GetZoomLevel() const { return zoom_level_; }

    // Get the address that was clicked (if Draw returned true)
    uint64_t GetClickedOffset() const { return clicked_offset_; }

private:
    ChangeDetector* detector_;
    int pixel_size_;      // Pixels per chunk (1, 2, 4, 6, or 8)
    int zoom_level_;      // Current zoom level (0-4)
    uint64_t clicked_offset_;

    // Color calculation based on chunk state
    struct Color {
        uint8_t r, g, b, a;
    };
    Color GetChunkColor(const ChunkInfo& chunk) const;
    Color LerpColor(Color a, Color b, float t) const;

    // ImGui helpers
    void DrawLegend();
    void DrawTooltip(size_t chunk_idx, const ChunkInfo& chunk);
};

} // namespace Haywire

#endif // HAYWIRE_HEAT_MAP_WIDGET_H