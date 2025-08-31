#include "process_memory_map.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace Haywire {

ProcessMemoryMap::ProcessMemoryMap() : currentPid(-1), selectedIndex(-1) {
}

ProcessMemoryMap::~ProcessMemoryMap() {
}

void ProcessMemoryMap::LoadProcess(int pid, GuestAgent* agent) {
    currentPid = pid;
    segments.clear();
    layoutGroups.clear();
    selectedIndex = -1;
    
    if (!agent || !agent->IsConnected()) {
        return;
    }
    
    std::vector<GuestMemoryRegion> regions;
    if (!agent->GetMemoryMap(pid, regions)) {
        return;
    }
    
    // Convert to our segment format
    for (const auto& region : regions) {
        MemorySegment seg;
        seg.start = region.start;
        seg.end = region.end;
        seg.name = region.name;
        seg.permissions = region.permissions;
        seg.DetermineType();
        
        // Only add interesting segments
        if (seg.IsInteresting()) {
            segments.push_back(seg);
        }
    }
    
    // Sort by address
    std::sort(segments.begin(), segments.end(), 
              [](const MemorySegment& a, const MemorySegment& b) {
                  return a.start < b.start;
              });
    
    CalculateLayout();
}

void ProcessMemoryMap::CalculateLayout() {
    layoutGroups.clear();
    if (segments.empty()) return;
    
    // Group nearby segments together
    const uint64_t GAP_THRESHOLD = 256 * 1024 * 1024; // 256MB gap means new group
    
    LayoutGroup currentGroup;
    currentGroup.startAddr = segments[0].start;
    currentGroup.endAddr = segments[0].end;
    currentGroup.segmentIndices.push_back(0);
    
    for (size_t i = 1; i < segments.size(); i++) {
        uint64_t gap = segments[i].start - currentGroup.endAddr;
        
        if (gap > GAP_THRESHOLD) {
            // Start new group
            layoutGroups.push_back(currentGroup);
            currentGroup = LayoutGroup();
            currentGroup.startAddr = segments[i].start;
            currentGroup.endAddr = segments[i].end;
            currentGroup.segmentIndices.clear();
            currentGroup.segmentIndices.push_back(i);
        } else {
            // Add to current group
            currentGroup.endAddr = segments[i].end;
            currentGroup.segmentIndices.push_back(i);
        }
    }
    
    if (!currentGroup.segmentIndices.empty()) {
        layoutGroups.push_back(currentGroup);
    }
    
    // Name the groups
    for (auto& group : layoutGroups) {
        if (group.startAddr < 0x1000000) {
            group.name = "Low Memory";
        } else if (group.startAddr < 0x100000000ULL) {
            group.name = "Program & Libraries";
        } else if (group.startAddr > 0x7FF000000000ULL) {
            group.name = "Stack & Kernel";
        } else if (group.startAddr > 0xF00000000000ULL) {
            group.name = "Large Mappings";
        } else {
            group.name = "Heap & Data";
        }
    }
}

void ProcessMemoryMap::Draw() {
    if (currentPid < 0) {
        ImGui::Text("No process selected");
        return;
    }
    
    ImGui::Text("Memory Map for PID %d", currentPid);
    ImGui::Separator();
    
    if (segments.empty()) {
        ImGui::Text("No memory regions loaded");
        return;
    }
    
    // Summary stats
    uint64_t totalMapped = 0;
    for (const auto& seg : segments) {
        totalMapped += (seg.end - seg.start);
    }
    
    ImGui::Text("Total mapped: %.2f MB across %zu regions", 
                totalMapped / (1024.0 * 1024.0), segments.size());
    ImGui::Text("Groups: %zu (skipping sparse regions)", layoutGroups.size());
    ImGui::Separator();
    
    // Draw the memory map
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    
    if (canvasSize.y < 100) canvasSize.y = 400; // Min height
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(canvasPos, 
                           ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                           IM_COL32(20, 20, 20, 255));
    
    // Draw groups
    float yOffset = 10;
    float groupHeight = 40;
    float groupSpacing = 60;
    
    for (size_t gi = 0; gi < layoutGroups.size(); gi++) {
        const auto& group = layoutGroups[gi];
        
        float groupY = canvasPos.y + yOffset;
        
        // Group label
        drawList->AddText(ImVec2(canvasPos.x + 5, groupY - 10), 
                         IM_COL32(200, 200, 200, 255),
                         group.name.c_str());
        
        // Address range
        std::stringstream range;
        range << std::hex << "0x" << group.startAddr << " - 0x" << group.endAddr;
        drawList->AddText(ImVec2(canvasPos.x + 150, groupY - 10),
                         IM_COL32(150, 150, 150, 255),
                         range.str().c_str());
        
        // Draw segments in this group
        float segX = canvasPos.x + 10;
        float segWidth = (canvasSize.x - 20) / group.segmentIndices.size();
        segWidth = std::min(segWidth, 100.0f); // Cap width
        
        for (size_t si : group.segmentIndices) {
            const auto& seg = segments[si];
            
            // Segment rectangle
            ImVec2 segStart(segX, groupY);
            ImVec2 segEnd(segX + segWidth - 2, groupY + groupHeight);
            
            uint32_t color = seg.GetTypeColor();
            drawList->AddRectFilled(segStart, segEnd, color);
            
            // Hover detection
            if (ImGui::IsMouseHoveringRect(segStart, segEnd)) {
                drawList->AddRect(segStart, segEnd, IM_COL32(255, 255, 255, 255), 0, 0, 2);
                
                // Tooltip
                ImGui::BeginTooltip();
                ImGui::Text("%s", seg.GetTypeName());
                ImGui::Text("Range: 0x%llx - 0x%llx", seg.start, seg.end);
                ImGui::Text("Size: %.2f MB", (seg.end - seg.start) / (1024.0 * 1024.0));
                ImGui::Text("Perms: %s", seg.permissions.c_str());
                if (!seg.name.empty()) {
                    ImGui::Text("Name: %s", seg.name.c_str());
                }
                ImGui::EndTooltip();
                
                // Click to select
                if (ImGui::IsMouseClicked(0)) {
                    selectedIndex = si;
                }
            }
            
            // Highlight selected
            if (selectedIndex == (int)si) {
                drawList->AddRect(segStart, segEnd, IM_COL32(255, 255, 0, 255), 0, 0, 3);
            }
            
            segX += segWidth;
        }
        
        yOffset += groupSpacing;
    }
    
    // Make canvas clickable
    ImGui::InvisibleButton("canvas", canvasSize);
    
    // Legend
    ImGui::Separator();
    ImGui::Text("Legend:");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Code");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "Data");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 1, 0.3f, 1), "Heap");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.3f, 1, 1), "Stack");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.3f, 1, 1, 1), "Libraries");
}

bool ProcessMemoryMap::GetSelectedRegion(uint64_t& start, uint64_t& end) const {
    if (selectedIndex >= 0 && selectedIndex < (int)segments.size()) {
        start = segments[selectedIndex].start;
        end = segments[selectedIndex].end;
        return true;
    }
    return false;
}

}