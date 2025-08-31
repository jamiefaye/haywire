#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>
#include "guest_agent.h"

namespace Haywire {

// Flattens sparse 64-bit address space into continuous navigable range
class AddressSpaceFlattener {
public:
    struct MappedRegion {
        uint64_t virtualStart;   // Original VA
        uint64_t virtualEnd;
        uint64_t flatStart;      // Position in flattened space
        uint64_t flatEnd;
        std::string name;
        
        uint64_t Size() const { return virtualEnd - virtualStart; }
        uint64_t FlatSize() const { return flatEnd - flatStart; }
    };
    
    AddressSpaceFlattener();
    
    // Build flattened map from memory regions
    void BuildFromRegions(const std::vector<GuestMemoryRegion>& regions);
    
    // Convert between virtual and flattened addresses
    uint64_t VirtualToFlat(uint64_t virtualAddr) const;
    uint64_t FlatToVirtual(uint64_t flatAddr) const;
    
    // Get region containing a virtual address
    const MappedRegion* GetRegionForVirtual(uint64_t virtualAddr) const;
    const MappedRegion* GetRegionForFlat(uint64_t flatAddr) const;
    
    // Get total flattened size (sum of all mapped regions)
    uint64_t GetFlatSize() const { return totalFlatSize; }
    
    // Get actual memory usage
    uint64_t GetMappedSize() const { return totalMappedSize; }
    
    // Get compression ratio (sparse space removed)
    float GetCompressionRatio() const {
        if (regions.empty()) return 1.0f;
        uint64_t virtualRange = regions.back().virtualEnd - regions.front().virtualStart;
        return virtualRange > 0 ? (float)totalMappedSize / virtualRange : 1.0f;
    }
    
    // Get all regions for visualization
    const std::vector<MappedRegion>& GetRegions() const { return regions; }
    
    // Generate navigation hints
    struct NavHint {
        uint64_t flatAddr;
        std::string label;
        bool isMajor;  // Major landmarks vs minor
    };
    std::vector<NavHint> GetNavigationHints() const;
    
private:
    std::vector<MappedRegion> regions;
    uint64_t totalFlatSize;
    uint64_t totalMappedSize;
    
    // Binary search helper
    const MappedRegion* FindRegion(uint64_t addr, bool useFlat) const;
};

// Specialized navigator for flattened address space
class CrunchedRangeNavigator {
public:
    CrunchedRangeNavigator();
    
    void SetFlattener(AddressSpaceFlattener* flattener) { 
        this->flattener = flattener; 
    }
    
    // Draw navigation UI
    void DrawNavigator();
    
    // Get current position in virtual space
    uint64_t GetCurrentVirtualAddress() const { return currentVirtualAddr; }
    
    // Navigate to virtual address
    void NavigateToVirtual(uint64_t virtualAddr);
    
    // Navigate by percentage (0.0 to 1.0)
    void NavigateToPercent(float percent);
    
    // Callback when navigation changes
    using NavigationCallback = std::function<void(uint64_t)>;
    void SetNavigationCallback(NavigationCallback cb) { callback = cb; }
    
private:
    AddressSpaceFlattener* flattener;
    uint64_t currentVirtualAddr;
    uint64_t currentFlatAddr;
    NavigationCallback callback;
    
    // UI state
    float sliderPos;  // 0.0 to 1.0
    bool isDragging;
    
    void UpdateFromSlider();
    void UpdateSliderFromAddress();
};

}