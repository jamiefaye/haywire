#include "beacon_decoder.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <set>

namespace Haywire {

BeaconDecoder::BeaconDecoder() 
    : lastTimestamp(0), lastGeneration(0), lastWriteSeq(0), currentPageSeq(0),
      currentCameraPID(0), lastCameraPID(0) {
}

BeaconDecoder::~BeaconDecoder() {
}

bool BeaconDecoder::ScanMemory(void* memBase, size_t memSize) {
    // DEPRECATED: This entire decoder uses the OLD beacon protocol
    std::cerr << "ERROR: BeaconDecoder::ScanMemory() uses OLD beacon protocol (MAGIC1/MAGIC2)\n";
    std::cerr << "       The companion now uses NEW beacon protocol from beacon_protocol.h\n";
    std::cerr << "       Use BeaconReader instead of BeaconDecoder\n";
    return false;
    
#ifdef UNUSED
    if (!memBase || memSize < PAGE_SIZE) {
        return false;
    }
    
    // Clear old data
    pidEntries.clear();
    sectionMap.clear();
    pteEntries.clear();
    cameraHeaders.clear();
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    
    // First pass: find the maximum generation number and count torn pages
    uint32_t maxGeneration = 0;
    size_t tornPages = 0;
    size_t totalBeaconPages = 0;
    size_t oldFormatPages = 0;
    
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        const BeaconPageHeader* header = reinterpret_cast<const BeaconPageHeader*>(mem + offset);
        if (header->magic1 == BEACON_MAGIC1 && header->magic2 == BEACON_MAGIC2) {
            oldFormatPages++;
            totalBeaconPages++;
            
            // Check for torn page (generation != write_seq indicates torn write)
            if (header->generation != header->write_seq) {
                tornPages++;
            }
            
            if (header->generation > maxGeneration) {
                maxGeneration = header->generation;
            }
        }
    }
    
    if (oldFormatPages > 0) {
        std::cerr << "WARNING: Found " << oldFormatPages << " OLD format beacon pages (MAGIC1/MAGIC2)\n";
        std::cerr << "         These are from an old companion version and will be ignored\n";
    }
    
    // Only report torn pages if we found any
    if (tornPages > 0) {
        std::cout << "WARNING: Found " << tornPages << " torn beacon pages (out of " 
                  << totalBeaconPages << " total beacon pages)\n";
    }
    
    // Set generation range to decode: (max-1) and max
    // This avoids torn reads while including complete data from previous run
    uint32_t minGenToProcess = (maxGeneration > 0) ? maxGeneration - 1 : 0;
    lastGeneration = maxGeneration;
    lastWriteSeq = 0;
    
    size_t validPages = 0;
    
    // Second pass: decode pages from the last two generations
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        const BeaconPageHeader* header = reinterpret_cast<const BeaconPageHeader*>(mem + offset);
        if (header->magic1 == BEACON_MAGIC1 && header->magic2 == BEACON_MAGIC2 &&
            header->generation >= minGenToProcess && header->generation <= maxGeneration) {
            if (DecodePage(mem + offset)) {
                validPages++;
            }
        }
    }
    
    if (validPages > 0) {
        std::cout << "Decoded " << validPages << " beacon pages: "
                  << pidEntries.size() << " PIDs, "
                  << sectionMap.size() << " sections, "
                  << pteEntries.size() << " PTEs\n";
        return true;
    }
    
    return false;
#endif // UNUSED
}

#ifdef USE_OLD_BEACON_PROTOCOL
bool BeaconDecoder::DecodePage(const uint8_t* pageData) {
    const BeaconPageHeader* header = reinterpret_cast<const BeaconPageHeader*>(pageData);
    
    // Check magic numbers
    if (header->magic1 != BEACON_MAGIC1 || header->magic2 != BEACON_MAGIC2) {
        return false;
    }
    
    // Update tracking for sequence numbers
    if (header->write_seq > lastWriteSeq) {
        lastWriteSeq = header->write_seq;
    }
    lastTimestamp = header->timestamp_ns;
    currentPageSeq = header->write_seq;  // Track for section updates
    
    // Process entries based on observer type
    const uint8_t* dataPtr = pageData + header->data_offset;
    const uint8_t* dataEnd = pageData + header->data_offset + header->data_size;
    
    uint32_t entriesProcessed = 0;
    
    while (dataPtr < dataEnd && entriesProcessed < header->entry_count) {
        // Read entry type
        uint32_t entryType = *reinterpret_cast<const uint32_t*>(dataPtr);
        
        switch (entryType) {
            case ENTRY_PID:
                if (dataPtr + sizeof(PIDEntry) <= dataEnd) {
                    DecodePIDEntry(dataPtr);
                    dataPtr += sizeof(PIDEntry);
                    entriesProcessed++;
                } else {
                    return false; // Incomplete entry
                }
                break;
                
            case ENTRY_SECTION:
                if (dataPtr + sizeof(SectionEntry) <= dataEnd) {
                    DecodeSectionEntry(dataPtr);
                    dataPtr += sizeof(SectionEntry);
                    entriesProcessed++;
                } else {
                    return false;
                }
                break;
                
            case ENTRY_PTE:
                if (dataPtr + sizeof(PTEEntry) <= dataEnd) {
                    DecodePTEEntry(dataPtr);
                    dataPtr += sizeof(PTEEntry);
                    entriesProcessed++;
                } else {
                    return false;
                }
                break;
                
            case ENTRY_CAMERA_HEADER:
                if (dataPtr + sizeof(CameraHeaderEntry) <= dataEnd) {
                    DecodeCameraHeader(dataPtr);
                    dataPtr += sizeof(CameraHeaderEntry);
                    entriesProcessed++;
                } else {
                    return false;
                }
                break;
                
            default:
                // Unknown entry type, can't continue safely
                return false;
        }
    }
    
    return entriesProcessed > 0;
}

void BeaconDecoder::DecodePIDEntry(const uint8_t* data) {
    const PIDEntry* entry = reinterpret_cast<const PIDEntry*>(data);
    
    // Check for duplicates (circular buffer may have multiple copies)
    auto it = std::find_if(pidEntries.begin(), pidEntries.end(),
                           [entry](const PIDEntry& e) { return e.pid == entry->pid; });
    
    if (it != pidEntries.end()) {
        // Update existing entry
        *it = *entry;
    } else {
        // Add new entry
        pidEntries.push_back(*entry);
    }
}

void BeaconDecoder::DecodeSectionEntry(const uint8_t* data) {
    const SectionEntry* entry = reinterpret_cast<const SectionEntry*>(data);
    
    // Create a modified entry with current camera PID if needed
    SectionEntry section = *entry;
    if (currentCameraPID != 0 && section.pid == 0) {
        section.pid = currentCameraPID;
    }
    
    // Create a key based on va_start and size
    uint64_t key = (section.va_start << 16) ^ (section.va_end - section.va_start);
    
    // Check if we have this section already
    auto it = sectionMap.find(key);
    if (it == sectionMap.end() || it->second.second < currentPageSeq) {
        // Either new section or newer version - update it
        sectionMap[key] = std::make_pair(section, currentPageSeq);
    }
}

void BeaconDecoder::DecodePTEEntry(const uint8_t* data) {
    const PTEEntry* entry = reinterpret_cast<const PTEEntry*>(data);
    pteEntries.push_back(*entry);
}

void BeaconDecoder::DecodeCameraHeader(const uint8_t* data) {
    const CameraHeaderEntry* entry = reinterpret_cast<const CameraHeaderEntry*>(data);
    
    // If camera PID changes, clear all old camera data
    if (entry->pid != lastCameraPID && lastCameraPID != 0) {
        sectionMap.clear();
        pteEntries.clear();
    }
    
    cameraHeaders.push_back(*entry);
    currentCameraPID = entry->pid; // Set context for following sections/PTEs
    lastCameraPID = entry->pid;
}

std::vector<SectionEntry> BeaconDecoder::GetSectionsForPID(uint32_t pid) const {
    std::vector<SectionEntry> result;
    for (const auto& pair : sectionMap) {
        const SectionEntry& section = pair.second.first;
        if (section.pid == pid) {
            result.push_back(section);
        }
    }
    return result;
}

std::vector<PTEEntry> BeaconDecoder::GetPTEsForPID(uint32_t pid) const {
    // PTEs don't have direct PID association, need camera header context
    // For now, return all PTEs if requested PID matches last camera PID
    if (currentCameraPID == pid) {
        return pteEntries;
    }
    return std::vector<PTEEntry>();
}

bool BeaconDecoder::HasRecentData(uint64_t maxAgeNs) const {
    if (lastTimestamp == 0) return false;
    
    auto now = std::chrono::steady_clock::now();
    uint64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    
    return (nowNs - lastTimestamp) < maxAgeNs;
}

std::unordered_map<uint64_t, uint64_t> BeaconDecoder::GetCameraPTEs(int camera) const {
    std::unordered_map<uint64_t, uint64_t> result;
    
    // For now, just return all PTEs we have
    // The PTEs are associated with the current camera PID through currentCameraPID
    // TODO: Track which camera the PTEs came from
    for (const auto& pte : pteEntries) {
        result[pte.va] = pte.pa;
    }
    
    return result;
}

int BeaconDecoder::GetCameraTargetPID(int camera) const {
    // The new beacon protocol stores the target PID in camera data pages
    // We need to extract it from the decoded data
    
    // Check if we have any camera headers with PID info
    if (!cameraHeaders.empty()) {
        // Return the PID from the most recent camera header
        return cameraHeaders.back().pid;
    }
    
    // If no camera headers, check if sections have a PID
    if (!sectionMap.empty()) {
        // Get PID from first section (they all should have same PID)
        return sectionMap.begin()->second.first.pid;
    }
    
    // Default: no PID found
    return 0;
}
#endif // USE_OLD_BEACON_PROTOCOL

} // namespace Haywire