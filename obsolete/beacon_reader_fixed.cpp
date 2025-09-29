// Fixed ScanForDiscovery that actually finds the discovery page
#include "beacon_reader.h"
#include "beacon_protocol.h"
#include <iostream>
#include <cstring>

namespace Haywire {

bool BeaconReader::ScanForDiscovery() {
    if (!memBase || !memSize) return false;
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    // Look for the discovery page (BEACON_CATEGORY_MASTER with index 0)
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        BeaconDiscoveryPage* page = reinterpret_cast<BeaconDiscoveryPage*>(mem + offset);
        
        // Check if this is a valid discovery page
        if (page->magic == BEACON_MAGIC &&
            page->category == BEACON_CATEGORY_MASTER &&
            page->category_index == 0 &&
            page->version_top == page->version_bottom) {
            
            // Found the discovery page!
            discovery.offset = offset;
            discovery.version = page->version_top;
            discovery.pid = page->session_id;
            discovery.timestamp = page->timestamp;
            
            // Copy category information
            for (int i = 0; i < BEACON_NUM_CATEGORIES; i++) {
                discovery.categories[i].base_offset = page->categories[i].base_offset;
                discovery.categories[i].page_count = page->categories[i].page_count;
                discovery.categories[i].write_index = page->categories[i].write_index;
                discovery.categories[i].sequence = page->categories[i].sequence;
            }
            
            discovery.valid = true;
            
            std::cout << "Found discovery page at offset 0x" << std::hex << offset << std::dec
                      << " (session=" << discovery.pid 
                      << ", timestamp=" << discovery.timestamp << ")\n";
            
            // Now build the category mappings
            BuildCategoryMappings();
            
            return true;
        }
    }
    
    std::cout << "Discovery page not found\n";
    return false;
}

// Fixed BuildCategoryMappings that doesn't skip pages unnecessarily
void BeaconReader::BuildCategoryMappings() {
    if (!discovery.valid) {
        std::cout << "BuildCategoryMappings: No valid discovery page\n";
        return;
    }
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    // Count all beacon pages with matching session_id
    int totalBeacons = 0;
    std::map<uint32_t, std::map<uint32_t, size_t>> categoryPages; // [category][index] = offset
    
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        uint32_t* magic = reinterpret_cast<uint32_t*>(mem + offset);
        if (*magic == BEACON_MAGIC) {
            BeaconPage* page = reinterpret_cast<BeaconPage*>(mem + offset);
            
            // Only process pages from the current session
            if (page->session_id == discovery.pid) {
                totalBeacons++;
                
                uint32_t category = page->category;
                uint32_t pageIndex = page->category_index;
                
                if (category < BEACON_NUM_CATEGORIES && pageIndex < 1000) {
                    categoryPages[category][pageIndex] = offset;
                }
            }
        }
    }
    
    std::cout << "Found " << totalBeacons << " beacon pages for session " << discovery.pid << "\n";
    
    // Build the mappings for each category
    for (int cat = 0; cat < BEACON_NUM_CATEGORIES; cat++) {
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
        if (categoryPages.find(cat) != categoryPages.end()) {
            for (const auto& [idx, offset] : categoryPages[cat]) {
                if (idx < mapping.expectedCount) {
                    mapping.sourceOffsets[idx] = offset;
                    mapping.sourcePresent[idx] = true;
                    mapping.foundCount++;
                }
            }
        }
        
        mapping.valid = (mapping.foundCount > 0);
        
        const char* catNames[] = {"Master", "PID", "Camera1", "Camera2"};
        std::cout << "  " << catNames[cat] << ": " 
                  << mapping.foundCount << "/" << mapping.expectedCount << " pages";
        
        // Check if this category appears contiguous (single malloc)
        if (mapping.foundCount > 1) {
            bool isContiguous = true;
            size_t firstOffset = 0;
            
            // Find first present page
            for (size_t i = 0; i < mapping.expectedCount; i++) {
                if (mapping.sourcePresent[i]) {
                    firstOffset = mapping.sourceOffsets[i];
                    
                    // Check if subsequent pages are contiguous
                    for (size_t j = i + 1; j < mapping.expectedCount && isContiguous; j++) {
                        if (mapping.sourcePresent[j]) {
                            size_t expectedOffset = firstOffset + (j - i) * PAGE_SIZE;
                            if (mapping.sourceOffsets[j] != expectedOffset) {
                                isContiguous = false;
                            }
                        }
                    }
                    break;
                }
            }
            
            if (isContiguous && firstOffset > 0) {
                std::cout << " (contiguous at 0x" << std::hex << firstOffset << std::dec << ")";
            }
        }
        std::cout << "\n";
    }
    
    // Report total
    int totalFound = 0;
    int totalExpected = 0;
    for (int cat = 0; cat < BEACON_NUM_CATEGORIES; cat++) {
        totalFound += categoryMappings[cat].foundCount;
        totalExpected += categoryMappings[cat].expectedCount;
    }
    
    std::cout << "Total: " << totalFound << "/" << totalExpected << " beacon pages mapped\n";
}

} // namespace Haywire