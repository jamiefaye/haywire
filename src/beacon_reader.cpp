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
    
    return ScanForDiscovery();
}

bool BeaconReader::ScanForDiscovery() {
    // Use the new decoder to scan for beacon data
    if (decoder->ScanMemory(memBase, memSize)) {
        // Check if we have recent PID data
        const auto& pids = decoder->GetPIDEntries();
        if (!pids.empty()) {
            std::cout << "Found " << pids.size() << " processes in beacon data\n";
            discovery.valid = true;
            return true;
        }
    }
    
    std::cout << "No beacon data found\n";
    return false;
}

void BeaconReader::BuildCategoryMappings() {
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    // First pass: collect ALL beacon pages and organize by category and index
    std::map<uint32_t, std::map<uint32_t, size_t>> categoryPages; // [category][index] = offset
    int totalBeacons = 0;
    
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        uint32_t* magic = reinterpret_cast<uint32_t*>(mem + offset);
        if (*magic == BEACON_MAGIC) {
            totalBeacons++;
            BeaconPage* page = reinterpret_cast<BeaconPage*>(mem + offset);
            
            // Skip pages from different timestamps (stale beacons)
            if (page->timestamp != discovery.timestamp) {
                continue;  // Different timestamp, skip this stale beacon
            }
            
            // Extract category and page index
            uint32_t category = page->category;
            uint32_t pageIndex = page->category_index;  // This should be the page's index within its category
            
            if (category < 5 && pageIndex < 10000) {  // Valid category and sane index
                categoryPages[category][pageIndex] = offset;
            } else if (totalBeacons <= 10) {
                // Debug first few invalid pages
                std::cout << "  Page at 0x" << std::hex << offset << std::dec 
                          << " has invalid cat=" << category << " idx=" << pageIndex << "\n";
            }
        }
    }
    
    std::cout << "Found " << totalBeacons << " total beacon pages in memory\n";
    
    // Build the COMPLETE mappings for each category
    for (int cat = 0; cat < 5; cat++) {
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
}

bool BeaconReader::GetPIDList(std::vector<uint32_t>& pids) {
    if (!discovery.valid) {
        if (!FindDiscovery()) return false;
    }
    
    // Use the new decoder to get PID list
    if (!decoder) return false;
    
    const auto& pidEntries = decoder->GetPIDEntries();
    if (pidEntries.empty()) {
        return false;
    }
    
    // Extract PIDs from entries
    pids.clear();
    for (const auto& entry : pidEntries) {
        pids.push_back(entry.pid);
    }
    
    std::cout << "Got " << pids.size() << " PIDs from decoder\n";
    return true;
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
        
        // Add PIDs from this page
        for (uint32_t j = 0; j < page->pids_in_page && j < 1012; j++) {
            genMap[gen].pids.push_back(page->pids[j]);
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
    if (!memBase || !decoder) return false;
    
    // Refresh beacon data
    decoder->ScanMemory(memBase, memSize);
    
    // Find the PID in the decoded entries
    const auto& pids = decoder->GetPIDEntries();
    for (const auto& entry : pids) {
        if (entry.pid == pid) {
            info.pid = entry.pid;
            info.ppid = entry.ppid;
            info.name = std::string(entry.comm);
            info.state = entry.state;
            info.rss = entry.rss_kb * 1024;  // Convert KB to bytes
            info.vsize = 0;  // Not available in new format yet
            info.num_threads = 0;  // Not available in new format yet
            info.exe_path = "";  // Not available in new format yet
            info.hasDetails = true;
            return true;
        }
    }
    
    return false;
}

bool BeaconReader::ScanRoundRobinForProcess(uint32_t pid, BeaconProcessInfo& info) {
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    if (!categoryMappings[BEACON_CATEGORY_ROUNDROBIN].valid) return false;
    
    // Scan round-robin array for this PID
    auto& rrArray = categoryArrays[BEACON_CATEGORY_ROUNDROBIN];
    if (!rrArray.initialized) return false;
    
    for (size_t i = 0; i < rrArray.pageCount; i++) {
        if (!rrArray.isPageValid(i)) continue;
        
        void* pagePtr = rrArray.getPage(i);
        BeaconPage* page = reinterpret_cast<BeaconPage*>(pagePtr);
        
        // Check if page is valid
        if (page->magic != BEACON_MAGIC || 
            page->version_top != page->version_bottom ||
            page->category != BEACON_CATEGORY_ROUNDROBIN) {
            continue;
        }
        
        // Check if this page contains a ProcessEntry
        if (page->data_size >= sizeof(ProcessEntry)) {
            ProcessEntry* entry = reinterpret_cast<ProcessEntry*>(page->data);
            
            if (entry->pid == pid) {
                // Found it!
                info.pid = entry->pid;
                info.ppid = entry->ppid;
                info.name = std::string(entry->comm, strnlen(entry->comm, 16));
                info.state = entry->state;
                info.vsize = entry->vsize;
                info.rss = entry->rss;
                info.num_threads = entry->num_threads;
                info.exe_path = std::string(entry->exe_path, strnlen(entry->exe_path, 256));
                info.hasDetails = true;
                return true;
            }
        }
    }
    
    // Not found in round-robin, return basic info
    info.pid = pid;
    info.hasDetails = false;
    return false;
}

std::map<uint32_t, BeaconProcessInfo> BeaconReader::GetAllProcessInfo() {
    std::map<uint32_t, BeaconProcessInfo> processes;
    if (!decoder) return processes;
    
    // Use decoder to get PID entries
    const auto& pidEntries = decoder->GetPIDEntries();
    
    for (const auto& entry : pidEntries) {
        BeaconProcessInfo info;
        info.pid = entry.pid;
        info.ppid = entry.ppid;
        info.name = std::string(entry.comm);
        info.state = entry.state;
        // Note: The new format stores RSS in KB, need to convert for compatibility
        info.vsize = 0;  // Not available in new format
        info.rss = entry.rss_kb * 1024 / 4096;  // Convert KB to pages
        info.num_threads = 0;  // Not available in new format
        info.exe_path = "";  // Not available in new format
        info.hasDetails = true;
        
        processes[info.pid] = info;
    }
    
    std::cout << "Found " << processes.size() << " processes with details from decoder\n";
    return processes;
}

bool BeaconReader::SetCameraFocus(int cameraId, uint32_t pid) {
    if (!memBase || !discovery.valid) return false;
    if (cameraId < 1 || cameraId > 2) return false;
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    uint32_t category = (cameraId == 1) ? BEACON_CATEGORY_CAMERA1 : BEACON_CATEGORY_CAMERA2;
    
    auto& camArray = categoryArrays[category];
    if (!camArray.initialized || camArray.validPages == 0) {
        return false;
    }
    
    // Control page is first page of camera category
    if (!camArray.isPageValid(0)) {
        std::cerr << "Camera " << cameraId << " control page is torn\n";
        return false;
    }
    
    void* pagePtr = camArray.getPage(0);
    CameraControlPage* control = reinterpret_cast<CameraControlPage*>(pagePtr);
    
    // Set up control command
    control->magic = BEACON_MAGIC;
    control->version_top = control->version_bottom = 1;
    control->command = 1;  // change_focus
    control->target_pid = pid;
    
    std::cout << "Set camera " << cameraId << " focus to PID " << pid << "\n";
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

bool BeaconReader::GetCameraProcessSections(int cameraId, uint32_t pid, std::vector<BeaconSectionEntry>& sections) {
    if (!memBase || !discovery.valid) return false;
    if (cameraId < 1 || cameraId > 2) return false;
    
    uint32_t category = (cameraId == 1) ? BEACON_CATEGORY_CAMERA1 : BEACON_CATEGORY_CAMERA2;
    auto& camArray = categoryArrays[category];
    
    if (!camArray.initialized || camArray.validPages == 0) {
        return false;
    }
    
    sections.clear();
    
    // Skip control page (page 0), start reading from page 1
    for (uint32_t pageIdx = 1; pageIdx < camArray.validPages; pageIdx++) {
        if (!camArray.isPageValid(pageIdx)) continue;
        
        BeaconPage* page = reinterpret_cast<BeaconPage*>(camArray.getPage(pageIdx));
        if (page->data_size == 0) continue;
        
        // Parse data in this page
        uint8_t* data = page->data;
        uint32_t offset = 0;
        
        while (offset + sizeof(BeaconProcessEntry) <= page->data_size) {
            // Check if this is a process entry
            BeaconProcessEntry* proc = reinterpret_cast<BeaconProcessEntry*>(data + offset);
            if (proc->pid == pid) {
                // Found the process, now read its sections
                offset += sizeof(BeaconProcessEntry);
                
                for (uint32_t i = 0; i < proc->num_sections && offset + sizeof(BeaconSectionEntry) <= page->data_size; i++) {
                    BeaconSectionEntry* section = reinterpret_cast<BeaconSectionEntry*>(data + offset);
                    sections.push_back(*section);
                    offset += sizeof(BeaconSectionEntry);
                }
                return true;
            }
            offset += sizeof(BeaconProcessEntry);
        }
    }
    
    return false;
}

bool BeaconReader::GetCameraPTEs(int cameraId, uint32_t pid, std::unordered_map<uint64_t, uint64_t>& ptes) {
    // TODO: Implement PTE reading from RLE compressed data
    // For now, return empty - we'll implement this when we need the crunched view
    ptes.clear();
    return true;
}

void BeaconReader::AllocateCategoryArrays() {
    for (int cat = 0; cat < 5; cat++) {
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
    
    for (int cat = 0; cat < 5; cat++) {
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
                std::cerr << "Category " << cat << " page " << i << " is torn\n";
            }
        }
        
        std::cout << "Category " << cat << ": copied " << array.validPages 
                  << " valid pages (of " << mapping.foundCount << " found)\n";
    }
}

bool BeaconReader::RefreshCategoryPages() {
    // This would be called periodically to refresh torn pages
    CopyPagesToArrays();
    
    // Check if we have minimum viable data
    bool viable = false;
    for (int cat = 0; cat < 5; cat++) {
        if (categoryArrays[cat].validPages > 0) {
            viable = true;
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