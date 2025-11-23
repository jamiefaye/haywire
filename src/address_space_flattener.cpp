#include "address_space_flattener.h"
#include "kernel_viewport_translator.h"
#include "pte_walker.h"
#include "memory_mapper.h"
#include "imgui.h"
#include <sstream>
#include <iomanip>
#include <iostream>

namespace Haywire {

AddressSpaceFlattener::AddressSpaceFlattener()
    : totalFlatSize(0), totalMappedSize(0), lazyTranslator(nullptr), lazyPid(0) {
}

AddressSpaceFlattener::~AddressSpaceFlattener() {
    StopPTEPatrol();
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
        mapped.ownershipType = static_cast<AddressSpaceFlattener::MappedRegion::OwnershipType>(region.ownershipType);

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

void AddressSpaceFlattener::BuildFromRAMRegions(const MemoryMapper* memoryMapper) {
    regions.clear();
    totalFlatSize = 0;
    totalMappedSize = 0;

    if (!memoryMapper) {
        std::cerr << "BuildFromRAMRegions: null memoryMapper\n";
        return;
    }

    const auto& ramRegions = memoryMapper->GetRegions();
    if (ramRegions.empty()) {
        std::cerr << "BuildFromRAMRegions: no RAM regions\n";
        return;
    }

    // Build flattened map from RAM regions
    // In PA mode, physical addresses are treated as "virtual" addresses
    // The flattener removes the PCI hole automatically
    uint64_t currentFlatPos = 0;

    for (const auto& ramRegion : ramRegions) {
        MappedRegion mapped;
        mapped.virtualStart = ramRegion.gpa_start;  // Physical address as "virtual"
        mapped.virtualEnd = ramRegion.gpa_end;
        mapped.flatStart = currentFlatPos;
        mapped.flatEnd = currentFlatPos + ramRegion.size;
        mapped.name = ramRegion.name;
        mapped.ownershipType = MappedRegion::UNKNOWN;  // No type info for PA regions

        regions.push_back(mapped);

        currentFlatPos += ramRegion.size;
        totalMappedSize += ramRegion.size;
    }

    totalFlatSize = currentFlatPos;

    std::cerr << "Flattened PA space: "
              << regions.size() << " RAM regions, "
              << (totalMappedSize / (1024.0 * 1024.0)) << " MB mapped\n";

    // Log regions for debugging
    for (const auto& region : regions) {
        std::cerr << "  PA 0x" << std::hex << region.virtualStart
                  << "-0x" << region.virtualEnd
                  << " → flat 0x" << region.flatStart
                  << "-0x" << region.flatEnd << std::dec
                  << " (" << region.name << ")\n";
    }
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

void AddressSpaceFlattener::EnableLazyPALookup(KernelViewportTranslator* translator, int pid,
                                               const std::unordered_map<uint64_t, uint64_t>* ptes) {
    if (!translator || totalFlatSize == 0) {
        std::cerr << "Cannot enable PA lookup: invalid translator or empty flat space\n";
        return;
    }

    lazyTranslator = translator;
    lazyPid = pid;

    // Allocate lookup cache
    size_t numPages = (totalFlatSize + PAGE_SIZE - 1) / PAGE_SIZE;
    paCache.Clear();
    paCache.Resize(numPages);

    std::cout << "***** Enabled PA lookup: " << numPages << " pages ("
              << (numPages * sizeof(uint64_t) / (1024.0 * 1024.0)) << " MB allocated)\n";

    // If PTEs provided, pre-populate the cache
    if (ptes && !ptes->empty()) {
        std::cout << "***** Pre-populating PA cache from " << ptes->size() << " PTEs...\n";
        int mappedCount = 0;
        int unmappedCount = 0;

        // First pass: mark all pages in regions as unmapped by default
        for (size_t i = 0; i < numPages; i++) {
            uint64_t flatAddr = i * PAGE_SIZE;
            const auto* region = GetRegionForFlat(flatAddr);
            if (region) {
                paCache.SetUnmapped(i);
                unmappedCount++;
            }
        }

        // Second pass: mark mapped pages from PTE walk
        for (const auto& [va, pa] : *ptes) {
            // Convert VA to flat address
            uint64_t flatAddr = VirtualToFlat(va);
            size_t pageIdx = flatAddr / PAGE_SIZE;

            if (pageIdx < numPages && paCache.IsUnmapped(pageIdx)) {
                paCache.SetPA(pageIdx, (pa / PAGE_SIZE) * PAGE_SIZE);  // Page-aligned PA
                unmappedCount--;
                mappedCount++;
            }
        }

        std::cout << "***** PA cache populated: " << mappedCount << " mapped, "
                  << unmappedCount << " unmapped pages\n";
    } else {
        std::cout << "***** Pages will be translated on first access (no PTEs provided)\n";
    }
}

uint64_t AddressSpaceFlattener::GetPhysicalAddress(uint64_t flatAddr) const {
    if (paCache.Size() == 0) {
        return 0;  // Lookup not enabled
    }

    size_t pageIdx = flatAddr / PAGE_SIZE;
    if (pageIdx >= paCache.Size()) {
        return 0;  // Out of bounds
    }

    // Check if already translated (mapped or unmapped sentinel)
    if (paCache.IsTranslated(pageIdx)) {
        uint64_t pagePA = paCache.GetPA(pageIdx);
        if (pagePA == 0) {
            return 0;  // Known unmapped page
        }
        // Add offset within page
        uint64_t offset = flatAddr % PAGE_SIZE;
        return pagePA + offset;
    }

    // Lazy translation: translate on first access
    if (lazyTranslator && lazyPid > 0) {
        // Find which region this flat address belongs to
        const auto* region = GetRegionForFlat(flatAddr);
        if (region) {
            // Calculate VA from flat address
            uint64_t offsetInRegion = flatAddr - region->flatStart;
            uint64_t va = region->virtualStart + offsetInRegion;

            // Translate VA to PA (page-aligned)
            uint64_t pageVA = (va / PAGE_SIZE) * PAGE_SIZE;
            uint64_t pagePA = lazyTranslator->TranslateAddress(lazyPid, pageVA);

            // Cache it (either mapped PA or unmapped sentinel)
            if (pagePA != 0) {
                paCache.SetPA(pageIdx, pagePA);
                uint64_t offset = flatAddr % PAGE_SIZE;
                return pagePA + offset;
            } else {
                paCache.SetUnmapped(pageIdx);
                return 0;
            }
        }
    }

    return 0;  // Unable to translate
}

void AddressSpaceFlattener::StartPTEPatrol(int patrolHz) {
    if (!lazyTranslator || lazyPid <= 0) {
        std::cerr << "Cannot start PTE patrol: translator not initialized\n";
        return;
    }

    if (!pteWalker) {
        pteWalker = std::make_unique<PTEWalker>();
    }

    if (pteWalker->IsRunning()) {
        std::cerr << "PTE patrol already running\n";
        return;
    }

    pteWalker->Start(&paCache, this, lazyTranslator, lazyPid, patrolHz);
}

void AddressSpaceFlattener::StopPTEPatrol() {
    if (pteWalker) {
        pteWalker->Stop();
    }
}

AddressSpaceFlattener::PTEPatrolStats AddressSpaceFlattener::GetPTEPatrolStats() const {
    PTEPatrolStats stats = {0, 0, 0, 0};
    if (pteWalker) {
        auto walkerStats = pteWalker->GetStats();
        stats.scanCount = walkerStats.scanCount;
        stats.newMappings = walkerStats.newMappings;
        stats.unmappings = walkerStats.unmappings;
        stats.lastScanDurationMs = walkerStats.lastScanDurationMs;
    }
    return stats;
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