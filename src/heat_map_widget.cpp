#include "heat_map_widget.h"
#include "change_detector.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <iostream>

namespace Haywire {

HeatMapWidget::HeatMapWidget(ChangeDetector* detector)
    : detector_(detector), pixel_size_(6), zoom_level_(3), clicked_offset_(0) {
}

HeatMapWidget::~HeatMapWidget() {
}

void HeatMapWidget::SetZoomLevel(int level) {
    zoom_level_ = std::clamp(level, 0, 4);
    const int zoom_sizes[] = {1, 2, 4, 6, 8};
    pixel_size_ = zoom_sizes[zoom_level_];
}

HeatMapWidget::Color HeatMapWidget::GetChunkColor(const ChunkInfo& chunk) const {
    // Unscanned - very dark
    if (!chunk.scanned) {
        return {10, 10, 10, 255};
    }

    // Zero chunk - dark gray
    if (chunk.is_zero) {
        return {32, 32, 32, 255};
    }

    // First scan - neutral blue (not a change)
    if (chunk.scan_count <= 1) {
        return {64, 128, 192, 255};
    }

    // Has changed - use logarithmic decay from hot (green) to cool (red)
    if (chunk.last_change_time > 0) {
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        uint64_t current_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

        uint64_t ms_since_change = current_ms - chunk.last_change_time;
        double seconds = ms_since_change / 1000.0;

        // Logarithmic decay: log(1) = 0 (just changed), log(1000) = 3 (old change)
        double decay = log10(std::max(1.0, seconds));
        double normalized = std::min(decay / 3.0, 1.0);  // Max at 1000 seconds

        if (normalized < 0.33) {
            // Green → yellow
            double t = normalized * 3.0;
            return {
                static_cast<uint8_t>(255 * t),
                255,
                0,
                255
            };
        } else if (normalized < 0.66) {
            // Yellow → orange
            double t = (normalized - 0.33) * 3.0;
            return {
                255,
                static_cast<uint8_t>(255 * (1.0 - t * 0.5)),
                0,
                255
            };
        } else {
            // Orange → deep red
            double t = (normalized - 0.66) * 3.0;
            return {
                static_cast<uint8_t>(255 * (1.0 - t * 0.3)),
                static_cast<uint8_t>(128 * (1.0 - t)),
                0,
                255
            };
        }
    }

    // Never changed - stable blue
    return {64, 128, 192, 255};
}

HeatMapWidget::Color HeatMapWidget::LerpColor(Color a, Color b, float t) const {
    return {
        static_cast<uint8_t>(a.r + (b.r - a.r) * t),
        static_cast<uint8_t>(a.g + (b.g - a.g) * t),
        static_cast<uint8_t>(a.b + (b.b - a.b) * t),
        static_cast<uint8_t>(a.a + (b.a - a.a) * t)
    };
}

bool HeatMapWidget::Draw(float width, float height, uint64_t current_offset, uint64_t view_size) {
    if (!detector_) {
        ImGui::Text("No detector");
        return false;
    }

    clicked_offset_ = 0;
    bool did_click = false;

    // Get the chunk size from detector
    size_t chunk_count = detector_->GetChunkCount();
    if (chunk_count == 0) {
        ImGui::Text("No data");
        return false;
    }

    const ChunkInfo& first_chunk = detector_->GetChunk(0);
    size_t chunk_size = first_chunk.offset;  // Offset of chunk 1 would be chunk_size
    if (chunk_count > 1) {
        const ChunkInfo& second_chunk = detector_->GetChunk(1);
        chunk_size = second_chunk.offset;  // More reliable
    } else {
        chunk_size = 65536;  // Default fallback
    }

    // Calculate heat map display range - show ENTIRE heat map canvas, not just near viewport
    // The viewport spans from current_offset to current_offset + view_size (for indicator position)
    size_t viewport_start_chunk = current_offset / chunk_size;
    size_t viewport_end_chunk = (current_offset + view_size) / chunk_size;

    int chunks_per_row = std::max(1, (int)(width / pixel_size_));
    int max_rows = std::max(1, (int)(height / pixel_size_));
    int total_visible_chunks = chunks_per_row * max_rows;

    // Debug: Log the dimensions (once)
    static int debug_count = 0;
    if (debug_count == 0) {
        std::cout << "HeatMap dimensions: width=" << width << "px height=" << height << "px\n";
        std::cout << "  pixel_size=" << pixel_size_ << "px\n";
        std::cout << "  chunks_per_row=" << chunks_per_row << " max_rows=" << max_rows << "\n";
        std::cout << "  total_visible_chunks=" << total_visible_chunks << "\n";
    }
    debug_count++;

    // Smart centering: center viewport in heat map display
    size_t viewport_center_chunk = (viewport_start_chunk + viewport_end_chunk) / 2;
    int half_window = total_visible_chunks / 2;

    size_t start_chunk;
    if (viewport_center_chunk < half_window) {
        // Near start - align to top (indicator will be at top)
        start_chunk = 0;
    } else if (viewport_center_chunk + half_window >= chunk_count) {
        // Near end - align to bottom (indicator will be at bottom)
        start_chunk = chunk_count > total_visible_chunks
                     ? chunk_count - total_visible_chunks
                     : 0;
    } else {
        // In middle - center the viewport in the heat map display
        start_chunk = viewport_center_chunk - half_window;
    }

    size_t end_chunk = std::min(start_chunk + total_visible_chunks, chunk_count);

    // Update detector's heat map range
    detector_->SetHeatMapRange(start_chunk, end_chunk);

    // Get draw list for custom rendering
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size(width, height);

    // Background
    draw_list->AddRectFilled(canvas_pos,
                            ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                            IM_COL32(26, 26, 26, 255));

    // Draw chunks
    for (size_t i = start_chunk; i < end_chunk; i++) {
        const ChunkInfo& chunk = detector_->GetChunk(i);

        // Calculate grid position
        size_t display_index = i - start_chunk;
        int row = display_index / chunks_per_row;
        int col = display_index % chunks_per_row;

        float x = canvas_pos.x + col * pixel_size_;
        float y = canvas_pos.y + row * pixel_size_;

        // Get color for this chunk
        Color color = GetChunkColor(chunk);

        // Draw chunk pixel (no gap - fill entire pixel)
        draw_list->AddRectFilled(
            ImVec2(x, y),
            ImVec2(x + pixel_size_, y + pixel_size_),
            IM_COL32(color.r, color.g, color.b, color.a)
        );
    }

    // Draw viewport indicator (yellow box)
    size_t view_start_chunk = current_offset / chunk_size;
    size_t view_end_chunk = (current_offset + view_size) / chunk_size;

    if (view_end_chunk >= start_chunk && view_start_chunk < end_chunk) {
        // Clamp to visible range
        size_t vis_start = std::max(view_start_chunk, start_chunk);
        size_t vis_end = std::min(view_end_chunk, end_chunk);

        size_t display_start = vis_start - start_chunk;
        size_t display_end = vis_end - start_chunk;

        int start_row = display_start / chunks_per_row;
        int start_col = display_start % chunks_per_row;
        int end_row = display_end / chunks_per_row;
        int end_col = display_end % chunks_per_row;

        // Draw the indicator properly handling multi-row spans
        if (start_row == end_row) {
            // Single row - simple rectangle
            float start_x = canvas_pos.x + start_col * pixel_size_;
            float start_y = canvas_pos.y + start_row * pixel_size_;
            float end_x = canvas_pos.x + (end_col + 1) * pixel_size_;
            float end_y = canvas_pos.y + (end_row + 1) * pixel_size_;

            draw_list->AddRect(
                ImVec2(start_x, start_y),
                ImVec2(end_x, end_y),
                IM_COL32(255, 255, 0, 255),
                0.0f, 0, 2.0f
            );
        } else {
            // Multiple rows - draw each row segment
            for (int row = start_row; row <= end_row; row++) {
                int col_start = (row == start_row) ? start_col : 0;
                int col_end = (row == end_row) ? end_col : chunks_per_row - 1;

                float x1 = canvas_pos.x + col_start * pixel_size_;
                float y1 = canvas_pos.y + row * pixel_size_;
                float x2 = canvas_pos.x + (col_end + 1) * pixel_size_;
                float y2 = canvas_pos.y + (row + 1) * pixel_size_;

                draw_list->AddRect(
                    ImVec2(x1, y1),
                    ImVec2(x2, y2),
                    IM_COL32(255, 255, 0, 255),
                    0.0f, 0, 2.0f
                );
            }
        }
    }

    // Make canvas interactive
    ImGui::InvisibleButton("heat_map_canvas", canvas_size);

    // Handle clicks
    if (ImGui::IsItemClicked()) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        float rel_x = mouse_pos.x - canvas_pos.x;
        float rel_y = mouse_pos.y - canvas_pos.y;

        int col = (int)(rel_x / pixel_size_);
        int row = (int)(rel_y / pixel_size_);

        size_t clicked_chunk = start_chunk + row * chunks_per_row + col;
        if (clicked_chunk < chunk_count) {
            clicked_offset_ = clicked_chunk * chunk_size;
            did_click = true;
        }
    }

    // Handle hover tooltips
    if (ImGui::IsItemHovered()) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        float rel_x = mouse_pos.x - canvas_pos.x;
        float rel_y = mouse_pos.y - canvas_pos.y;

        int col = (int)(rel_x / pixel_size_);
        int row = (int)(rel_y / pixel_size_);

        size_t hovered_chunk = start_chunk + row * chunks_per_row + col;
        if (hovered_chunk < chunk_count) {
            const ChunkInfo& chunk = detector_->GetChunk(hovered_chunk);
            DrawTooltip(hovered_chunk, chunk);
        }
    }

    return did_click;
}

void HeatMapWidget::DrawTooltip(size_t chunk_idx, const ChunkInfo& chunk) {
    ImGui::BeginTooltip();
    ImGui::Text("Chunk: %zu", chunk_idx);
    ImGui::Text("Offset: 0x%llx", (unsigned long long)chunk.offset);

    if (!chunk.scanned) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Not scanned");
    } else if (chunk.is_zero) {
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "ZERO");
    } else {
        ImGui::Text("Checksum: 0x%x", chunk.checksum);
        ImGui::Text("Scans: %u", chunk.scan_count);

        if (chunk.last_change_time > 0) {
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            uint64_t current_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
            double seconds_ago = (current_ms - chunk.last_change_time) / 1000.0;

            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "CHANGED");
            ImGui::Text("%.1f seconds ago", seconds_ago);
        }
    }

    ImGui::EndTooltip();
}

void HeatMapWidget::DrawLegend() {
    ImGui::Text("Legend:");

    // Unscanned
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    draw_list->AddRectFilled(pos, ImVec2(pos.x + 12, pos.y + 12), IM_COL32(10, 10, 10, 255));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 16, pos.y));
    ImGui::Text("Unscanned");
    pos.y += 16;

    // Zero
    ImGui::SetCursorScreenPos(pos);
    draw_list->AddRectFilled(pos, ImVec2(pos.x + 12, pos.y + 12), IM_COL32(32, 32, 32, 255));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 16, pos.y));
    ImGui::Text("Zero");
    pos.y += 16;

    // Changed
    ImGui::SetCursorScreenPos(pos);
    draw_list->AddRectFilled(pos, ImVec2(pos.x + 12, pos.y + 12), IM_COL32(0, 255, 0, 255));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 16, pos.y));
    ImGui::Text("Changed");
    pos.y += 16;

    // Data
    ImGui::SetCursorScreenPos(pos);
    draw_list->AddRectFilled(pos, ImVec2(pos.x + 12, pos.y + 12), IM_COL32(64, 128, 192, 255));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 16, pos.y));
    ImGui::Text("Data");
}

} // namespace Haywire