// beacon_reader_v2.cpp - Updated SetCameraFocus that uses mapping table
#include "beacon_reader.h"
#include "beacon_decoder.h"
#include <iostream>
#include <cstring>

namespace Haywire {

// New implementation of SetCameraFocus that uses the mapping table
bool BeaconReader::SetCameraFocus(int cameraId, uint32_t pid) {
    if (!memBase || !memSize) return false;
    if (cameraId < 1 || cameraId > 2) return false;
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    // First, scan memory to build/refresh the mapping table
    // This finds all 5 malloc regions and builds categoryMappings
    if (!discovery.valid) {
        if (!ScanForDiscovery()) {
            std::cout << "SetCameraFocus: Failed to find discovery page\n";
            return false;
        }
        BuildCategoryMappings();
    }
    
    // Use the mapping table to find the camera control page
    // Control page is always at index 0 of the camera category
    uint32_t targetCategory = (cameraId == 1) ? BEACON_CATEGORY_CAMERA1 : BEACON_CATEGORY_CAMERA2;
    
    // Check if we have a valid mapping for this camera
    if (!categoryMappings[targetCategory].valid || 
        categoryMappings[targetCategory].foundCount == 0) {
        std::cout << "SetCameraFocus: No mapping for camera " << cameraId << "\n";
        return false;
    }
    
    // Get the control page offset (index 0 in camera malloc)
    if (!categoryMappings[targetCategory].sourcePresent[0]) {
        std::cout << "SetCameraFocus: Control page not found for camera " << cameraId << "\n";
        return false;
    }
    
    size_t controlOffset = categoryMappings[targetCategory].sourceOffsets[0];
    BeaconCameraControlPage* control = reinterpret_cast<BeaconCameraControlPage*>(mem + controlOffset);
    
    // Verify it's a valid control page
    if (control->magic != BEACON_MAGIC || 
        control->category != targetCategory ||
        control->category_index != 0) {
        std::cout << "SetCameraFocus: Invalid control page for camera " << cameraId << "\n";
        return false;
    }
    
    // Update version for tear detection
    uint32_t newVersion = control->version_top + 1;
    
    // Write new target PID and version
    control->target_pid = pid;
    control->status = BEACON_CAMERA_STATUS_SWITCHING;
    control->version_top = newVersion;
    control->version_bottom = newVersion;
    
    std::cout << "SetCameraFocus: Camera " << cameraId << " -> PID " << pid 
              << " (control page at offset 0x" << std::hex << controlOffset << std::dec 
              << ", version=" << newVersion << ")\n";
    
    return true;
}

// Enhanced BuildCategoryMappings that understands the 5 malloc structure
void BeaconReader::BuildCategoryMappings() {
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    // Build mapping table for the 5 separate malloc regions
    // Each malloc contains a contiguous set of beacon pages
    
    // First, find all beacon pages and group by session/timestamp
    std::map<uint32_t, std::map<uint32_t, std::vector<size_t>>> sessionCategories;
    
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        uint32_t* magic = reinterpret_cast<uint32_t*>(mem + offset);
        if (*magic == BEACON_MAGIC) {
            BeaconPage* page = reinterpret_cast<BeaconPage*>(mem + offset);
            
            // Group by session_id and category
            sessionCategories[page->session_id][page->category].push_back(offset);
        }
    }
    
    // Find the session with the discovery page (should have BEACON_CATEGORY_MASTER)
    uint32_t activeSession = 0;
    for (const auto& [session, categories] : sessionCategories) {
        if (categories.find(BEACON_CATEGORY_MASTER) != categories.end()) {
            activeSession = session;
            break;
        }
    }
    
    if (activeSession == 0) {
        std::cout << "BuildCategoryMappings: No active session found\n";
        return;
    }
    
    std::cout << "BuildCategoryMappings: Found active session " << activeSession << "\n";
    
    // Now build the mappings for each category
    const auto& activeCategories = sessionCategories[activeSession];
    
    for (int cat = 0; cat < 4; cat++) {
        auto& mapping = categoryMappings[cat];
        
        // Clear and initialize
        mapping.sourceOffsets.clear();
        mapping.sourcePresent.clear();
        mapping.foundCount = 0;
        mapping.valid = false;
        
        // Get expected page count from discovery
        mapping.expectedCount = discovery.categories[cat].page_count;
        
        if (mapping.expectedCount == 0) continue;
        
        // Allocate arrays
        mapping.sourceOffsets.resize(mapping.expectedCount, 0);
        mapping.sourcePresent.resize(mapping.expectedCount, false);
        
        // Fill in pages we found
        if (activeCategories.find(cat) != activeCategories.end()) {
            const auto& pages = activeCategories.at(cat);
            
            // Build index map for this category
            std::map<uint32_t, size_t> indexToOffset;
            for (size_t offset : pages) {
                BeaconPage* page = reinterpret_cast<BeaconPage*>(mem + offset);
                if (page->category_index < mapping.expectedCount) {
                    indexToOffset[page->category_index] = offset;
                }
            }
            
            // Populate the mapping arrays
            for (const auto& [idx, offset] : indexToOffset) {
                mapping.sourceOffsets[idx] = offset;
                mapping.sourcePresent[idx] = true;
                mapping.foundCount++;
            }
            
            // Check if this malloc region appears contiguous
            if (!indexToOffset.empty()) {
                auto firstIdx = indexToOffset.begin()->first;
                auto firstOffset = indexToOffset.begin()->second;
                bool isContiguous = true;
                
                // Check if pages are physically contiguous (each malloc is contiguous)
                for (size_t i = 1; i < mapping.expectedCount && isContiguous; i++) {
                    if (indexToOffset.count(firstIdx + i)) {
                        size_t expectedOffset = firstOffset + i * PAGE_SIZE;
                        if (indexToOffset[firstIdx + i] != expectedOffset) {
                            isContiguous = false;
                        }
                    }
                }
                
                if (isContiguous) {
                    std::cout << "  Category " << cat << ": Found contiguous malloc at 0x" 
                              << std::hex << firstOffset << std::dec 
                              << " (" << mapping.foundCount << "/" << mapping.expectedCount << " pages)\n";
                }
            }
            
            mapping.valid = (mapping.foundCount > 0);
        }
    }
    
    std::cout << "BuildCategoryMappings: Mapping table built\n";
    std::cout << "  Master: " << categoryMappings[0].foundCount << "/" << categoryMappings[0].expectedCount << " pages\n";
    std::cout << "  PID: " << categoryMappings[1].foundCount << "/" << categoryMappings[1].expectedCount << " pages\n";
    std::cout << "  Camera1: " << categoryMappings[2].foundCount << "/" << categoryMappings[2].expectedCount << " pages\n";
    std::cout << "  Camera2: " << categoryMappings[3].foundCount << "/" << categoryMappings[3].expectedCount << " pages\n";
}

} // namespace Haywire