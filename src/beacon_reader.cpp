#include "beacon_reader.h"
#include "beacon_decoder.h"
#include "guest_agent.h"
#include <iostream>
#include <iomanip>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <ctime>
#include <sys/stat.h>
#include <algorithm>
#include <map>

// Compile-time structure size verification
#define STATIC_ASSERT(cond, msg) static_assert(cond, msg)

namespace Haywire {

// Verify structure sizes match companion expectations
// These checks are now in beacon_protocol.h

BeaconReader::BeaconReader() : memFd(-1), memBase(nullptr), memSize(0), companionPid(0), lastCompanionCheck(0) {
    discovery.valid = false;
    discovery.allPagesFound = false;
    decoder = std::make_unique<BeaconDecoder>();
}

BeaconReader::~BeaconReader() {
    Cleanup();
}

bool BeaconReader::Initialize(const std::string& memoryPath) {
    // Open memory file
    memFd = open(memoryPath.c_str(), O_RDWR);
    if (memFd < 0) {
        std::cerr << "Failed to open memory file: " << memoryPath << "\n";
        return false;
    }
    
    // Get file size
    struct stat st;
    if (fstat(memFd, &st) < 0) {
        close(memFd);
        memFd = -1;
        return false;
    }
    memSize = st.st_size;
    
    // Map the memory
    memBase = mmap(nullptr, memSize, PROT_READ | PROT_WRITE, MAP_SHARED, memFd, 0);
    if (memBase == MAP_FAILED) {
        std::cerr << "Failed to map memory\n";
        close(memFd);
        memFd = -1;
        memBase = nullptr;
        return false;
    }
    
    std::cout << "Beacon reader initialized: " << memSize / (1024*1024) << " MB mapped\n";
    return true;
}

void BeaconReader::Cleanup() {
    if (memBase && memBase != MAP_FAILED) {
        munmap(memBase, memSize);
        memBase = nullptr;
    }
    if (memFd >= 0) {
        close(memFd);
        memFd = -1;
    }
}

bool BeaconReader::FindDiscovery() {
    if (!memBase) return false;
    
    // If we already found all pages, just refresh the data from known locations
    if (discovery.valid && discovery.allPagesFound) {
        // Refresh the pages we already know about
        return RefreshCategoryPages();
    }
    
    // Otherwise do a full scan
    return ScanForDiscovery();
}

bool BeaconReader::ScanForDiscovery() {
    if (!memBase || !memSize) return false;
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    size_t pagesScanned = 0;
    size_t beaconsFound = 0;
    
    std::cout << "Scanning " << (memSize / (1024*1024)) << " MB for discovery page...\n";
    
    // Look for the discovery page (BEACON_CATEGORY_MASTER with index 0)
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        pagesScanned++;
        
        // Report progress every 256MB
        if (pagesScanned % 65536 == 0) {
            std::cout << "  Scanned " << (pagesScanned * PAGE_SIZE / (1024*1024)) 
                      << " MB, found " << beaconsFound << " beacon pages so far...\n";
        }
        // Count all beacon pages we encounter
        if (*reinterpret_cast<uint32_t*>(mem + offset) == BEACON_MAGIC) {
            beaconsFound++;
        }
        
        BeaconDiscoveryPage* page = reinterpret_cast<BeaconDiscoveryPage*>(mem + offset);
        
        // Check if this is the discovery page
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
            
            std::cout << "\nFound discovery page at offset 0x" << std::hex << offset << std::dec
                      << " (session=" << discovery.pid 
                      << ", timestamp=" << discovery.timestamp << ")\n";
            std::cout << "Total pages scanned: " << pagesScanned 
                      << " (" << beaconsFound << " were beacon pages)\n";
            
            // Now build the category mappings
            BuildCategoryMappings();
            
            return true;
        }
    }
    
    std::cout << "\nDiscovery page not found after scanning " << pagesScanned << " pages\n";
    std::cout << "Found " << beaconsFound << " beacon pages total (but no discovery page)\n";
    return false;
}

void BeaconReader::BuildCategoryMappings() {
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    // First pass: collect ALL beacon pages and organize by category and index
    std::map<uint32_t, std::map<uint32_t, size_t>> categoryPages; // [category][index] = offset
    int totalBeacons = 0;
    int validBeacons = 0;
    int staleBeacons = 0;  // From different sessions
    int bogusBeacons = 0;  // Invalid category or index
    
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        uint32_t* magic = reinterpret_cast<uint32_t*>(mem + offset);
        if (*magic == BEACON_MAGIC) {
            totalBeacons++;
            BeaconPage* page = reinterpret_cast<BeaconPage*>(mem + offset);
            
            // Check if page is from current session
            if (page->session_id != discovery.pid) {
                staleBeacons++;
                continue;  // Different session, skip this stale beacon
            }
            
            // Extract category and page index
            uint32_t category = page->category;
            uint32_t pageIndex = page->category_index;
            
            // Validate category and index
            if (category >= BEACON_NUM_CATEGORIES || pageIndex >= 10000) {
                bogusBeacons++;
                if (bogusBeacons <= 5) {  // Report first few bogus pages
                    std::cout << "  Bogus beacon at 0x" << std::hex << offset << std::dec 
                              << " (cat=" << category << ", idx=" << pageIndex << ")\n";
                }
            } else {
                // Valid beacon page
                validBeacons++;
                categoryPages[category][pageIndex] = offset;
            }
        }
    }
    
    std::cout << "\nBeacon Discovery Results:\n";
    std::cout << "  Total beacon pages found: " << totalBeacons << "\n";
    std::cout << "  Valid beacon pages (current session): " << validBeacons << "\n";
    std::cout << "  Stale beacon pages (old sessions): " << staleBeacons << "\n";
    std::cout << "  Bogus beacon pages (invalid cat/idx): " << bogusBeacons << "\n";
    
    // Build the COMPLETE mappings for each category
    for (int cat = 0; cat < 4; cat++) {
        auto& catInfo = discovery.categories[cat];
        auto& mapping = categoryMappings[cat];
        
        // Clear and initialize
        mapping.sourceOffsets.clear();
        mapping.sourcePresent.clear();
        mapping.expectedCount = catInfo.page_count;
        mapping.foundCount = 0;
        mapping.valid = false;
        
        // Allocate the FULL arrays as declared by discovery
        if (catInfo.page_count > 0) {
            mapping.sourceOffsets.resize(catInfo.page_count, 0);
            mapping.sourcePresent.resize(catInfo.page_count, false);
            
            // Fill in the pages we found
            if (categoryPages.find(cat) != categoryPages.end()) {
                auto& pages = categoryPages[cat];
                
                for (auto& [idx, offset] : pages) {
                    if (idx < catInfo.page_count) {
                        mapping.sourceOffsets[idx] = offset;
                        mapping.sourcePresent[idx] = true;
                        mapping.foundCount++;
                    } else {
                        std::cerr << "WARNING: Cat " << cat << " page index " << idx 
                                  << " exceeds expected count " << catInfo.page_count << "\n";
                    }
                }
                
                std::cout << "Category " << cat << ": " << mapping.foundCount 
                          << "/" << mapping.expectedCount << " pages found";
                
                if (mapping.foundCount < mapping.expectedCount) {
                    std::cout << " (MISSING " << (mapping.expectedCount - mapping.foundCount) 
                              << " pages - likely torn during scan)";
                }
                std::cout << "\n";
                
                // Show distribution info
                if (mapping.foundCount > 0) {
                    // Find gaps in the mapping
                    std::vector<std::pair<size_t, size_t>> gaps;
                    size_t gapStart = 0;
                    bool inGap = false;
                    
                    for (size_t i = 0; i < catInfo.page_count; i++) {
                        if (!mapping.sourcePresent[i]) {
                            if (!inGap) {
                                gapStart = i;
                                inGap = true;
                            }
                        } else {
                            if (inGap) {
                                gaps.push_back({gapStart, i - 1});
                                inGap = false;
                            }
                        }
                    }
                    if (inGap) {
                        gaps.push_back({gapStart, catInfo.page_count - 1});
                    }
                    
                    if (!gaps.empty()) {
                        std::cout << "  Missing pages: ";
                        for (auto& [start, end] : gaps) {
                            if (start == end) {
                                std::cout << start << " ";
                            } else {
                                std::cout << start << "-" << end << " ";
                            }
                        }
                        std::cout << "\n";
                    }
                    
                    // Show first few present pages
                    int shown = 0;
                    for (size_t i = 0; i < catInfo.page_count && shown < 3; i++) {
                        if (mapping.sourcePresent[i]) {
                            std::cout << "  [" << i << "] -> 0x" << std::hex 
                                      << mapping.sourceOffsets[i] << std::dec << "\n";
                            shown++;
                        }
                    }
                }
                
                mapping.valid = (mapping.foundCount > 0);
            } else {
                std::cout << "Category " << cat << ": 0/" << mapping.expectedCount 
                          << " pages found (COMPLETELY MISSING!)\n";
            }
        } else {
            std::cout << "Category " << cat << ": not configured (0 pages expected)\n";
        }
    }
    
    // Check if we found all expected pages
    size_t totalExpected = 0;
    size_t totalFound = 0;
    for (int cat = 0; cat < 4; cat++) {
        totalExpected += categoryMappings[cat].expectedCount;
        totalFound += categoryMappings[cat].foundCount;
    }
    
    discovery.allPagesFound = (totalFound == totalExpected);
    if (discovery.allPagesFound) {
        std::cout << "\n✓ All " << totalExpected << " expected beacon pages found!\n";
        std::cout << "  Switching to fast refresh mode (no more full memory scans)\n";
    } else {
        std::cout << "\n⚠ Found " << totalFound << "/" << totalExpected << " expected pages\n";
        std::cout << "  Will continue scanning for missing pages\n";
    }
    
    // Now allocate and copy the category arrays
    AllocateCategoryArrays();
    CopyPagesToArrays();
}

bool BeaconReader::GetPIDList(std::vector<uint32_t>& pids) {
    if (!discovery.valid) {
        if (!FindDiscovery()) return false;
    }
    
    // NEW: Use the new beacon protocol format instead of old decoder
    auto generations = GetPIDGenerations();
    if (generations.empty()) {
        std::cerr << "ERROR: No PID generations found in new beacon format\n";
        std::cerr << "       Make sure companion_camera_v2 is using new beacon_protocol.h\n";
        return false;
    }
    
    // Find the most recent complete generation
    for (auto it = generations.rbegin(); it != generations.rend(); ++it) {
        if (it->is_complete) {
            pids = it->pids;
            std::cout << "Got " << pids.size() << " PIDs from generation " << it->generation 
                      << " (new beacon format)\n";
            return true;
        }
    }
    
    // If no complete generation, use the most recent one anyway
    if (!generations.empty()) {
        pids = generations.back().pids;
        std::cout << "Got " << pids.size() << " PIDs from partial generation " 
                  << generations.back().generation << " (new beacon format)\n";
        return true;
    }
    
    return false;
}

std::vector<PIDGeneration> BeaconReader::GetPIDGenerations() {
    std::vector<PIDGeneration> generations;
    if (!memBase || !discovery.valid) return generations;
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    std::map<uint32_t, PIDGeneration> genMap;
    
    // Use the mapped PID category pages
    if (!categoryMappings[BEACON_CATEGORY_PID].valid) {
        std::cerr << "No valid PID category mapping\n";
        return generations;
    }
    
    std::cout << "Scanning " << categoryMappings[BEACON_CATEGORY_PID].foundCount 
              << " available PID pages (of " << categoryMappings[BEACON_CATEGORY_PID].expectedCount << " expected)\n";
    
    // Scan all valid pages in PID category array
    auto& pidArray = categoryArrays[BEACON_CATEGORY_PID];
    if (!pidArray.initialized) {
        std::cerr << "PID array not initialized\n";
        return generations;
    }
    
    for (size_t i = 0; i < pidArray.pageCount; i++) {
        if (!pidArray.isPageValid(i)) {
            // Page missing or torn
            continue;
        }
        
        PIDListPage* page = reinterpret_cast<PIDListPage*>(pidArray.getPage(i));
        
        // Check if page is valid (not torn)
        if (page->magic != BEACON_MAGIC || 
            page->version_top != page->version_bottom ||
            page->category != BEACON_CATEGORY_PID) {
            continue;
        }
        
        // Add PIDs to this generation
        uint32_t gen = page->generation;
        if (genMap.find(gen) == genMap.end()) {
            genMap[gen].generation = gen;
            genMap[gen].total_pids = page->total_pids;
            genMap[gen].is_complete = false;
        }
        
        // Add PIDs from this page (now reading from entries array)
        for (uint32_t j = 0; j < page->pids_in_page && j < BEACON_MAX_PIDS_PER_PAGE; j++) {
            genMap[gen].pids.push_back(page->entries[j].pid);
        }
    }
    
    // Convert map to vector and check completeness
    for (auto& [gen, genData] : genMap) {
        genData.is_complete = (genData.pids.size() == genData.total_pids);
        generations.push_back(genData);
    }
    
    // Sort by generation number
    std::sort(generations.begin(), generations.end(), 
              [](const PIDGeneration& a, const PIDGeneration& b) {
                  return a.generation < b.generation;
              });
    
    return generations;
}

bool BeaconReader::GetProcessInfo(uint32_t pid, BeaconProcessInfo& info) {
    if (!memBase) return false;
    
    // OLD DECODER NO LONGER SUPPORTED
    std::cerr << "WARNING: GetProcessInfo() called - old decoder no longer supported\n";
    std::cerr << "         Process details should come from camera beacon data\n";
    
    // Return minimal info for now
    info.pid = pid;
    info.ppid = 0;
    info.name = "PID " + std::to_string(pid);
    info.state = '?';
    info.rss = 0;
    info.vsize = 0;
    info.num_threads = 0;
    info.exe_path = "";
    info.hasDetails = false;
    
    return false;
}

// Process scanning moved to camera beacon pages

std::map<uint32_t, BeaconProcessInfo> BeaconReader::GetAllProcessInfo() {
    std::map<uint32_t, BeaconProcessInfo> processes;
    if (!memBase || !discovery.valid) return processes;
    
    // Read process details from the new PID beacon pages
    auto& pidMapping = categoryMappings[BEACON_CATEGORY_PID];
    if (!pidMapping.valid) return processes;
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    // Find the most recent generation
    uint32_t maxGen = 0;
    for (size_t i = 0; i < pidMapping.expectedCount; i++) {
        if (!pidMapping.sourcePresent[i]) continue;
        
        size_t offset = pidMapping.sourceOffsets[i];
        PIDListPage* page = reinterpret_cast<PIDListPage*>(mem + offset);
        
        if (page->magic == BEACON_MAGIC && page->version_top == page->version_bottom) {
            if (page->generation > maxGen) {
                maxGen = page->generation;
            }
        }
    }
    
    // Read all entries from the most recent generation
    for (size_t i = 0; i < pidMapping.expectedCount; i++) {
        if (!pidMapping.sourcePresent[i]) continue;
        
        size_t offset = pidMapping.sourceOffsets[i];
        PIDListPage* page = reinterpret_cast<PIDListPage*>(mem + offset);
        
        if (page->magic == BEACON_MAGIC && 
            page->version_top == page->version_bottom &&
            page->generation == maxGen) {
            
            // Read entries from this page
            for (uint32_t j = 0; j < page->pids_in_page && j < BEACON_MAX_PIDS_PER_PAGE; j++) {
                const auto& entry = page->entries[j];
                
                BeaconProcessInfo info;
                info.pid = entry.pid;
                info.ppid = entry.ppid;
                info.name = std::string(entry.comm);
                info.state = entry.state;
                info.rss = entry.rss_kb * 1024 / 4096;  // Convert KB to pages
                info.vsize = 0;  // Not available in current format
                info.num_threads = 0;  // Not available in current format
                info.exe_path = "";  // Not available in current format
                info.hasDetails = true;
                
                processes[info.pid] = info;
            }
        }
    }
    
    std::cout << "Found " << processes.size() << " processes with details from PID beacon pages\n";
    return processes;
}

bool BeaconReader::SetCameraFocus(int cameraId, uint32_t pid) {
    if (!memBase || !memSize) return false;
    if (cameraId < 1 || cameraId > 2) return false;
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    // Look for the camera control page using new beacon protocol
    // Camera control pages have:
    // - magic == BEACON_MAGIC (0x3142FACE)
    // - category == BEACON_CATEGORY_CAMERA1 or BEACON_CATEGORY_CAMERA2
    // - category_index == 0 (control page is always index 0)
    uint32_t targetCategory = (cameraId == 1) ? BEACON_CATEGORY_CAMERA1 : BEACON_CATEGORY_CAMERA2;
    bool foundControlPage = false;
    size_t maxInitialScan = std::min(memSize, size_t(100 * 1024 * 1024)); // First 100MB
    
    // First pass: Check likely locations (every 1MB in first 100MB)
    for (size_t offset = 0; offset < maxInitialScan && !foundControlPage; offset += (1024 * 1024)) {
        if (offset + PAGE_SIZE > memSize) break;
        
        const BeaconCameraControlPage* control = reinterpret_cast<const BeaconCameraControlPage*>(mem + offset);
        
        // Check if this is a valid camera control page
        if (control->magic == BEACON_MAGIC && 
            control->category == targetCategory &&
            control->category_index == 0) {
            
            // Found the control page - update it
            BeaconCameraControlPage* writableControl = reinterpret_cast<BeaconCameraControlPage*>(mem + offset);
            
            // Increment version for tear detection
            uint32_t newVersion = control->version_top + 1;
            
            // Update the control page
            writableControl->version_top = newVersion;
            writableControl->target_pid = pid;
            writableControl->version_bottom = newVersion;  // Must match version_top for valid write
            
            std::cout << "SetCameraFocus: Camera " << cameraId << " -> PID " << pid 
                      << " (control page at offset 0x" << std::hex << offset << std::dec 
                      << ", version=" << newVersion << ")\n";
            foundControlPage = true;
            
            // Remember this location for next time
            static size_t cachedControlOffset = 0;
            cachedControlOffset = offset;
        }
    }
    
    // If not found in likely locations, do full scan (but warn)
    if (!foundControlPage) {
        std::cout << "SetCameraFocus: Control page not in expected location, scanning all memory...\n";
        for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
            const BeaconCameraControlPage* control = reinterpret_cast<const BeaconCameraControlPage*>(mem + offset);
            
            if (control->magic == BEACON_MAGIC && 
                control->category == targetCategory &&
                control->category_index == 0) {
                
                BeaconCameraControlPage* writableControl = reinterpret_cast<BeaconCameraControlPage*>(mem + offset);
                
                uint32_t newVersion = control->version_top + 1;
                
                writableControl->version_top = newVersion;
                writableControl->target_pid = pid;
                writableControl->version_bottom = newVersion;
                
                std::cout << "SetCameraFocus: Camera " << cameraId << " -> PID " << pid 
                          << " (control page at offset 0x" << std::hex << offset << std::dec 
                          << ", version=" << newVersion << ")\n";
                foundControlPage = true;
                break;
            }
        }
    }
    
    if (!foundControlPage) {
        std::cout << "SetCameraFocus: No camera " << cameraId 
                  << " control page found (category=" << targetCategory << ")\n";
        return false;
    }
    
    return true;
}

uint32_t BeaconReader::GetCameraFocus(int cameraId) {
    if (!memBase || !discovery.valid) return 0;
    if (cameraId < 1 || cameraId > 2) return 0;
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    uint32_t category = (cameraId == 1) ? BEACON_CATEGORY_CAMERA1 : BEACON_CATEGORY_CAMERA2;
    
    auto& camArray = categoryArrays[category];
    if (!camArray.initialized || !camArray.isPageValid(0)) {
        return 0;
    }
    
    // Control page is first page of camera category
    void* pagePtr = camArray.getPage(0);
    CameraControlPage* control = reinterpret_cast<CameraControlPage*>(pagePtr);
    
    if (control->magic == BEACON_MAGIC && 
        control->version_top == control->version_bottom) {
        return control->current_pid;
    }
    
    return 0;
}

bool BeaconReader::GetCameraProcessSections(int cameraId, uint32_t pid, std::vector<SectionEntry>& sections) {
    sections.clear();
    
    if (!memBase || !discovery.valid) return false;
    if (cameraId < 1 || cameraId > 2) return false;
    
    uint32_t category = (cameraId == 1) ? BEACON_CATEGORY_CAMERA1 : BEACON_CATEGORY_CAMERA2;
    auto& camArray = categoryArrays[category];
    
    if (!camArray.initialized) {
        return false;
    }
    
    // Camera data pages start at index 1 (index 0 is control page)
    for (size_t pageIdx = 1; pageIdx < camArray.pageCount; pageIdx++) {
        if (!camArray.isPageValid(pageIdx)) {
            continue;
        }
        
        void* pagePtr = camArray.getPage(pageIdx);
        BeaconCameraDataPage* dataPage = reinterpret_cast<BeaconCameraDataPage*>(pagePtr);
        
        // Check page validity and if it contains data for the requested PID
        if (dataPage->magic != BEACON_MAGIC || 
            dataPage->version_top != dataPage->version_bottom ||
            dataPage->target_pid != pid) {
            continue;
        }
        
        // Parse the stream of entries in this page
        uint8_t* dataPtr = dataPage->data;
        size_t bytesProcessed = 0;
        
        for (uint16_t i = 0; i < dataPage->entry_count && bytesProcessed < 4052; i++) {
            uint8_t entryType = *dataPtr;
            
            if (entryType == BEACON_ENTRY_TYPE_END) {
                break;  // End of entries in this page
            }
            else if (entryType == BEACON_ENTRY_TYPE_SECTION) {
                BeaconSectionEntry* beaconSection = reinterpret_cast<BeaconSectionEntry*>(dataPtr);
                
                // Convert to SectionEntry format
                SectionEntry section;
                section.type = 1;  // ENTRY_SECTION
                section.pid = beaconSection->pid;
                section.va_start = beaconSection->va_start;
                section.va_end = beaconSection->va_end;
                section.perms = beaconSection->perms;
                strncpy(section.path, beaconSection->path, 63);
                section.path[63] = '\0';
                
                sections.push_back(section);
                
                dataPtr += sizeof(BeaconSectionEntry);
                bytesProcessed += sizeof(BeaconSectionEntry);
            }
            else if (entryType == BEACON_ENTRY_TYPE_PTE) {
                // Skip PTE entries for now (handled in GetCameraPTEs)
                dataPtr += sizeof(BeaconPTEEntry);
                bytesProcessed += sizeof(BeaconPTEEntry);
            }
            else {
                // Unknown entry type, stop processing this page
                std::cerr << "Unknown entry type " << (int)entryType << " in camera data page\n";
                break;
            }
        }
        
        // If continuation flag is not set, we've processed all pages for this PID
        if (dataPage->continuation == 0) {
            break;
        }
    }
    
    if (!sections.empty()) {
        std::cout << "GetCameraProcessSections: Found " << sections.size() 
                  << " sections for PID " << pid << " from camera " << cameraId << "\n";
    }
    
    return !sections.empty();
}

bool BeaconReader::GetCameraPTEs(int cameraId, uint32_t pid, std::unordered_map<uint64_t, uint64_t>& ptes) {
    ptes.clear();
    
    if (!memBase || !discovery.valid) return false;
    if (cameraId < 1 || cameraId > 2) return false;
    
    uint32_t category = (cameraId == 1) ? BEACON_CATEGORY_CAMERA1 : BEACON_CATEGORY_CAMERA2;
    auto& camArray = categoryArrays[category];
    
    if (!camArray.initialized) {
        return false;
    }
    
    // Camera data pages start at index 1 (index 0 is control page)
    for (size_t pageIdx = 1; pageIdx < camArray.pageCount; pageIdx++) {
        if (!camArray.isPageValid(pageIdx)) {
            continue;
        }
        
        void* pagePtr = camArray.getPage(pageIdx);
        BeaconCameraDataPage* dataPage = reinterpret_cast<BeaconCameraDataPage*>(pagePtr);
        
        // Check page validity and if it contains data for the requested PID
        if (dataPage->magic != BEACON_MAGIC || 
            dataPage->version_top != dataPage->version_bottom ||
            dataPage->target_pid != pid) {
            continue;
        }
        
        // Parse the stream of entries in this page
        uint8_t* dataPtr = dataPage->data;
        size_t bytesProcessed = 0;
        
        for (uint16_t i = 0; i < dataPage->entry_count && bytesProcessed < 4052; i++) {
            uint8_t entryType = *dataPtr;
            
            if (entryType == BEACON_ENTRY_TYPE_END) {
                break;  // End of entries in this page
            }
            else if (entryType == BEACON_ENTRY_TYPE_SECTION) {
                // Skip section entries (handled in GetCameraProcessSections)
                dataPtr += sizeof(BeaconSectionEntry);
                bytesProcessed += sizeof(BeaconSectionEntry);
            }
            else if (entryType == BEACON_ENTRY_TYPE_PTE) {
                BeaconPTEEntry* beaconPTE = reinterpret_cast<BeaconPTEEntry*>(dataPtr);
                
                // Add PTE to map (only if physical address is non-zero, indicating it's allocated)
                if (beaconPTE->pa != 0) {
                    ptes[beaconPTE->va] = beaconPTE->pa;
                }
                
                dataPtr += sizeof(BeaconPTEEntry);
                bytesProcessed += sizeof(BeaconPTEEntry);
            }
            else {
                // Unknown entry type, stop processing this page
                break;
            }
        }
        
        // If continuation flag is not set, we've processed all pages for this PID
        if (dataPage->continuation == 0) {
            break;
        }
    }
    
    if (!ptes.empty()) {
        std::cout << "GetCameraPTEs: Found " << ptes.size() 
                  << " PTEs for PID " << pid << " from camera " << cameraId << "\n";
    }
    
    return true;
}

void BeaconReader::AllocateCategoryArrays() {
    for (int cat = 0; cat < 4; cat++) {
        auto& array = categoryArrays[cat];
        auto& mapping = categoryMappings[cat];
        
        array.pageCount = mapping.expectedCount;
        array.validPages = 0;
        array.initialized = false;
        
        if (array.pageCount > 0) {
            // Allocate contiguous array for all pages
            array.data.resize(array.pageCount * PAGE_SIZE, 0);
            array.pageValid.resize(array.pageCount, false);
            array.pageVersions.resize(array.pageCount, 0);
            array.initialized = true;
            
            std::cout << "Allocated " << array.pageCount << " pages (" 
                      << (array.pageCount * PAGE_SIZE) / 1024 << " KB) for category " << cat << "\n";
        }
    }
}

void BeaconReader::CopyPagesToArrays() {
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    for (int cat = 0; cat < 4; cat++) {
        auto& array = categoryArrays[cat];
        auto& mapping = categoryMappings[cat];
        
        if (!array.initialized) continue;
        
        array.validPages = 0;
        
        for (size_t i = 0; i < array.pageCount; i++) {
            if (!mapping.sourcePresent[i]) {
                array.pageValid[i] = false;
                continue;
            }
            
            // Copy the page
            void* src = mem + mapping.sourceOffsets[i];
            void* dst = &array.data[i * PAGE_SIZE];
            memcpy(dst, src, PAGE_SIZE);
            
            // Check if page is torn
            BeaconPage* page = reinterpret_cast<BeaconPage*>(dst);
            if (page->magic == BEACON_MAGIC && 
                page->version_top == page->version_bottom) {
                array.pageValid[i] = true;
                array.pageVersions[i] = page->version_top;
                array.validPages++;
            } else {
                array.pageValid[i] = false;
                if (cat == 2 && (i == 1 || i == 2)) {
                    std::cerr << "Category " << cat << " page " << i << " torn: "
                              << "magic=0x" << std::hex << page->magic 
                              << " version_top=" << std::dec << page->version_top
                              << " version_bottom=" << page->version_bottom << "\n";
                } else {
                    std::cerr << "Category " << cat << " page " << i << " is torn\n";
                }
            }
        }
        
        std::cout << "Category " << cat << ": copied " << array.validPages 
                  << " valid pages (of " << mapping.foundCount << " found)\n";
    }
}

bool BeaconReader::RefreshCategoryPages() {
    // Fast refresh - just copy from known locations without scanning
    static int refreshCount = 0;
    refreshCount++;
    
    // Only print every 10th refresh to avoid spam
    if (refreshCount % 10 == 0) {
        std::cout << "Refreshing beacon pages from known locations (refresh #" << refreshCount << ")\n";
    }
    
    CopyPagesToArrays();
    
    // Check if we have minimum viable data
    bool viable = false;
    int validCategories = 0;
    for (int cat = 0; cat < 4; cat++) {
        if (categoryArrays[cat].validPages > 0) {
            viable = true;
            validCategories++;
        }
    }
    
    return viable;
}

// Companion management functions
bool BeaconReader::StartCompanion(GuestAgent* agent) {
    if (!agent) {
        std::cout << "No guest agent available for companion startup\n";
        return false;
    }
    
    if (!agent->IsConnected()) {
        std::cout << "Guest agent not connected\n";
        return false;
    }
    
    std::cout << "Starting companion via QGA...\n";
    
    // First, stop any existing companion
    std::string output;
    agent->ExecuteCommand("killall companion_camera companion_multi companion_camera_v2 2>/dev/null || true", output);
    
    // Check if companion binary already exists
    agent->ExecuteCommand("[ -x /home/ubuntu/companion_camera_v2 ] && echo 'BINARY_EXISTS'", output);
    
    if (output.find("BINARY_EXISTS") == std::string::npos) {
        // Binary doesn't exist - need to check for source and compile
        std::cout << "Companion binary not found, checking source files...\n";
        
        // Check if companion_camera_v2.c exists
        agent->ExecuteCommand("[ -f /home/ubuntu/companion_camera_v2.c ] || echo 'SOURCE_MISSING'", output);
        if (output.find("SOURCE_MISSING") != std::string::npos) {
            std::cout << "companion_camera_v2.c not found in VM. Please deploy:\n";
            std::cout << "  scp src/companion_camera_v2.c vm:~/\n";
            std::cout << "  scp src/beacon_encoder.c vm:~/\n";
            std::cout << "  scp include/beacon_encoder.h vm:~/\n";
            return false;
        }
        
        // Check if beacon_encoder.c exists
        agent->ExecuteCommand("[ -f /home/ubuntu/beacon_encoder.c ] || echo 'ENCODER_MISSING'", output);
        if (output.find("ENCODER_MISSING") != std::string::npos) {
            std::cout << "beacon_encoder.c not found in VM. Please deploy:\n";
            std::cout << "  scp src/beacon_encoder.c vm:~/\n";
            return false;
        }
        
        // Compile the companion
        std::cout << "Compiling companion in VM...\n";
        agent->ExecuteCommand("cd /home/ubuntu && gcc -O2 -o companion_camera_v2 companion_camera_v2.c beacon_encoder.c 2>&1", output);
        
        // Verify binary was created
        agent->ExecuteCommand("[ -x /home/ubuntu/companion_camera_v2 ] && echo 'COMPILE_SUCCESS'", output);
        if (output.find("COMPILE_SUCCESS") == std::string::npos) {
            std::cout << "Compilation failed\n";
            return false;
        }
        std::cout << "Companion compiled successfully\n";
    } else {
        std::cout << "Using existing companion binary\n";
    }
    
    // Start the companion in background (needs sudo for memory access)
    std::cout << "Starting companion process...\n";
    bool success = agent->ExecuteCommand("cd /home/ubuntu && sudo nohup ./companion_camera_v2 > companion.log 2>&1 & echo $!", output);
    if (!success) {
        std::cout << "Failed to start companion\n";
        return false;
    }
    
    // Extract PID from output
    try {
        companionPid = std::stoul(output);
        lastCompanionCheck = time(nullptr);
        std::cout << "Started companion with PID " << companionPid << "\n";
        
        // Give it a moment to start up
        usleep(500000); // 500ms
        
        return true;
    } catch (...) {
        std::cout << "Failed to parse companion PID from: " << output << "\n";
        companionPid = 0;
        return false;
    }
}

bool BeaconReader::IsCompanionRunning() {
    if (companionPid == 0) return false;
    
    // Don't check too frequently
    uint32_t now = time(nullptr);
    if (now - lastCompanionCheck < 5) {
        return companionPid != 0; // Assume it's still running if we checked recently
    }
    
    lastCompanionCheck = now;
    
    // For now, just check if we have a valid discovery page
    // This is a good indicator that the companion is working
    return discovery.valid && discovery.pid == companionPid;
}

bool BeaconReader::StopCompanion(GuestAgent* agent) {
    if (!agent || companionPid == 0) return true;
    
    std::cout << "Stopping companion PID " << companionPid << "...\n";
    
    std::string output;
    std::string killCmd = "kill " + std::to_string(companionPid) + " 2>/dev/null || killall companion_camera 2>/dev/null || true";
    agent->ExecuteCommand(killCmd, output);
    
    companionPid = 0;
    lastCompanionCheck = 0;
    discovery.valid = false;
    
    std::cout << "Companion stopped\n";
    return true;
}

} // namespace Haywire