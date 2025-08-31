#include "address_space_flattener.h"
#include "imgui.h"
#include <sstream>
#include <iomanip>
#include <iostream>

namespace Haywire {

AddressSpaceFlattener::AddressSpaceFlattener() 
    : totalFlatSize(0), totalMappedSize(0) {
}

void AddressSpaceFlattener::BuildFromRegions(const std::vector<GuestMemoryRegion>& inputRegions) {
    regions.clear();
    totalFlatSize = 0;
    totalMappedSize = 0;
    
    if (inputRegions.empty()) return;
    
    // Sort regions by virtual address
    std::vector<GuestMemoryRegion> sortedRegions = inputRegions;
    std::sort(sortedRegions.begin(), sortedRegions.end(),
              [](const GuestMemoryRegion& a, const GuestMemoryRegion& b) {
                  return a.start < b.start;
              });
    
    // Build flattened map
    uint64_t currentFlatPos = 0;
    
    for (const auto& region : sortedRegions) {
        MappedRegion mapped;
        mapped.virtualStart = region.start;
        mapped.virtualEnd = region.end;
        mapped.flatStart = currentFlatPos;
        mapped.flatEnd = currentFlatPos + (region.end - region.start);
        mapped.name = region.name;
        
        regions.push_back(mapped);
        
        uint64_t regionSize = region.end - region.start;
        currentFlatPos += regionSize;
        totalMappedSize += regionSize;
    }
    
    totalFlatSize = currentFlatPos;
    
    std::cerr << "Flattened address space: " 
              << regions.size() << " regions, "
              << (totalMappedSize / (1024.0 * 1024.0)) << " MB mapped, "
              << "compression ratio: " << (1.0 / GetCompressionRatio()) << ":1\n";
}

uint64_t AddressSpaceFlattener::VirtualToFlat(uint64_t virtualAddr) const {
    auto region = GetRegionForVirtual(virtualAddr);
    if (!region) {
        // Not in any region - find nearest
        if (regions.empty()) return 0;
        
        // If before first region, return 0
        if (virtualAddr < regions.front().virtualStart) return 0;
        
        // If after last region, return end
        if (virtualAddr >= regions.back().virtualEnd) return totalFlatSize;
        
        // Find nearest region
        for (size_t i = 0; i < regions.size() - 1; i++) {
            if (virtualAddr >= regions[i].virtualEnd && 
                virtualAddr < regions[i + 1].virtualStart) {
                // In gap between regions - snap to nearest
                uint64_t distToPrev = virtualAddr - regions[i].virtualEnd;
                uint64_t distToNext = regions[i + 1].virtualStart - virtualAddr;
                
                if (distToPrev < distToNext) {
                    return regions[i].flatEnd;
                } else {
                    return regions[i + 1].flatStart;
                }
            }
        }
        
        return 0;
    }
    
    // Within a region - linear mapping
    uint64_t offset = virtualAddr - region->virtualStart;
    return region->flatStart + offset;
}

uint64_t AddressSpaceFlattener::FlatToVirtual(uint64_t flatAddr) const {
    auto region = GetRegionForFlat(flatAddr);
    if (!region) {
        // Outside all regions
        if (regions.empty()) return 0;
        if (flatAddr >= totalFlatSize && !regions.empty()) {
            return regions.back().virtualEnd;
        }
        return 0;
    }
    
    // Within a region - linear mapping
    uint64_t offset = flatAddr - region->flatStart;
    return region->virtualStart + offset;
}

const AddressSpaceFlattener::MappedRegion* 
AddressSpaceFlattener::GetRegionForVirtual(uint64_t virtualAddr) const {
    return FindRegion(virtualAddr, false);
}

const AddressSpaceFlattener::MappedRegion* 
AddressSpaceFlattener::GetRegionForFlat(uint64_t flatAddr) const {
    return FindRegion(flatAddr, true);
}

const AddressSpaceFlattener::MappedRegion* 
AddressSpaceFlattener::FindRegion(uint64_t addr, bool useFlat) const {
    if (regions.empty()) return nullptr;
    
    // Binary search
    size_t left = 0;
    size_t right = regions.size() - 1;
    
    while (left <= right) {
        size_t mid = left + (right - left) / 2;
        const auto& region = regions[mid];
        
        uint64_t start = useFlat ? region.flatStart : region.virtualStart;
        uint64_t end = useFlat ? region.flatEnd : region.virtualEnd;
        
        if (addr >= start && addr < end) {
            return &regions[mid];
        } else if (addr < start) {
            if (mid == 0) break;
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }
    
    return nullptr;
}

std::vector<AddressSpaceFlattener::NavHint> 
AddressSpaceFlattener::GetNavigationHints() const {
    std::vector<NavHint> hints;
    
    for (const auto& region : regions) {
        // Major landmarks
        if (region.name == "[heap]") {
            hints.push_back({region.flatStart, "Heap Start", true});
        } else if (region.name == "[stack]") {
            hints.push_back({region.flatStart, "Stack", true});
        } else if (region.name.find("vdso") != std::string::npos) {
            hints.push_back({region.flatStart, "VDSO", false});
        } else if (region.virtualStart < 0x1000000) {
            hints.push_back({region.flatStart, "Low Memory", true});
        } else if (region.name.find("/lib") == 0 || region.name.find(".so") != std::string::npos) {
            // First library is major
            static bool firstLib = true;
            if (firstLib) {
                hints.push_back({region.flatStart, "Libraries", true});
                firstLib = false;
            }
        } else if (!region.name.empty() && region.name[0] == '/') {
            // Executable
            if (region.name.find("/bin/") != std::string::npos ||
                region.name.find("/usr/bin/") != std::string::npos) {
                hints.push_back({region.flatStart, "Program", true});
            }
        }
    }
    
    return hints;
}

// CrunchedRangeNavigator implementation

CrunchedRangeNavigator::CrunchedRangeNavigator()
    : flattener(nullptr), currentVirtualAddr(0), currentFlatAddr(0),
      sliderPos(0.0f), isDragging(false) {
}

void CrunchedRangeNavigator::DrawNavigator() {
    if (!flattener || flattener->GetRegions().empty()) {
        ImGui::Text("No memory map loaded");
        return;
    }
    
    ImGui::Text("Memory Navigator (Compressed)");
    ImGui::Separator();
    
    // Stats
    uint64_t totalMapped = flattener->GetMappedSize();
    float compression = 1.0f / flattener->GetCompressionRatio();
    
    ImGui::Text("Mapped: %.2f MB | Compression: %.1fx", 
                totalMapped / (1024.0 * 1024.0), compression);
    
    // Main slider
    ImGui::Text("Navigation:");
    
    float oldSliderPos = sliderPos;
    if (ImGui::SliderFloat("##NavSlider", &sliderPos, 0.0f, 1.0f, "")) {
        UpdateFromSlider();
    }
    
    // Current address display
    if (currentVirtualAddr > 0) {
        ImGui::Text("Current: 0x%llx", currentVirtualAddr);
        
        auto region = flattener->GetRegionForVirtual(currentVirtualAddr);
        if (region && !region->name.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), 
                             "(%s)", region->name.c_str());
        }
    }
    
    // Visual map with regions
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 60);
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(canvasPos, 
                           ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                           IM_COL32(30, 30, 30, 255));
    
    // Draw regions
    const auto& regions = flattener->GetRegions();
    uint64_t totalFlat = flattener->GetFlatSize();
    
    for (const auto& region : regions) {
        float x1 = canvasPos.x + (region.flatStart / (float)totalFlat) * canvasSize.x;
        float x2 = canvasPos.x + (region.flatEnd / (float)totalFlat) * canvasSize.x;
        
        // Color based on type
        uint32_t color = IM_COL32(80, 80, 80, 255);
        if (region.name == "[heap]") {
            color = IM_COL32(255, 255, 100, 255);
        } else if (region.name == "[stack]") {
            color = IM_COL32(255, 100, 255, 255);
        } else if (region.name.find(".so") != std::string::npos) {
            color = IM_COL32(100, 255, 255, 255);
        } else if (!region.name.empty() && region.name[0] == '/') {
            color = IM_COL32(100, 100, 255, 255);
        }
        
        drawList->AddRectFilled(ImVec2(x1, canvasPos.y + 10),
                               ImVec2(x2, canvasPos.y + canvasSize.y - 10),
                               color);
    }
    
    // Current position indicator
    float currentX = canvasPos.x + sliderPos * canvasSize.x;
    drawList->AddLine(ImVec2(currentX, canvasPos.y),
                     ImVec2(currentX, canvasPos.y + canvasSize.y),
                     IM_COL32(255, 255, 0, 255), 2.0f);
    
    // Make canvas clickable
    ImGui::InvisibleButton("NavCanvas", canvasSize);
    if (ImGui::IsItemHovered()) {
        // Show address at mouse position
        ImVec2 mousePos = ImGui::GetMousePos();
        float mouseX = (mousePos.x - canvasPos.x) / canvasSize.x;
        mouseX = std::max(0.0f, std::min(1.0f, mouseX));
        
        uint64_t flatAddr = (uint64_t)(mouseX * totalFlat);
        uint64_t virtualAddr = flattener->FlatToVirtual(flatAddr);
        
        ImGui::SetTooltip("0x%llx", virtualAddr);
        
        if (ImGui::IsMouseClicked(0)) {
            sliderPos = mouseX;
            UpdateFromSlider();
        }
    }
    
    // Quick jump buttons
    ImGui::Separator();
    ImGui::Text("Quick Jump:");
    
    auto hints = flattener->GetNavigationHints();
    int buttonCount = 0;
    for (const auto& hint : hints) {
        if (!hint.isMajor) continue;
        
        if (buttonCount > 0) ImGui::SameLine();
        if (ImGui::Button(hint.label.c_str())) {
            NavigateToVirtual(flattener->FlatToVirtual(hint.flatAddr));
        }
        buttonCount++;
        if (buttonCount >= 5) break; // Limit buttons per row
    }
}

void CrunchedRangeNavigator::NavigateToVirtual(uint64_t virtualAddr) {
    if (!flattener) return;
    
    currentVirtualAddr = virtualAddr;
    currentFlatAddr = flattener->VirtualToFlat(virtualAddr);
    UpdateSliderFromAddress();
    
    if (callback) {
        callback(currentVirtualAddr);
    }
}

void CrunchedRangeNavigator::NavigateToPercent(float percent) {
    sliderPos = std::max(0.0f, std::min(1.0f, percent));
    UpdateFromSlider();
}

void CrunchedRangeNavigator::UpdateFromSlider() {
    if (!flattener) return;
    
    uint64_t totalFlat = flattener->GetFlatSize();
    currentFlatAddr = (uint64_t)(sliderPos * totalFlat);
    currentVirtualAddr = flattener->FlatToVirtual(currentFlatAddr);
    
    if (callback) {
        callback(currentVirtualAddr);
    }
}

void CrunchedRangeNavigator::UpdateSliderFromAddress() {
    if (!flattener) return;
    
    uint64_t totalFlat = flattener->GetFlatSize();
    if (totalFlat > 0) {
        sliderPos = (float)currentFlatAddr / totalFlat;
        sliderPos = std::max(0.0f, std::min(1.0f, sliderPos));
    }
}

}