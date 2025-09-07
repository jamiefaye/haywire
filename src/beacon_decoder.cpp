#include "beacon_decoder.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <algorithm>

namespace Haywire {

BeaconDecoder::BeaconDecoder() 
    : lastTimestamp(0), lastGeneration(0), lastWriteSeq(0), currentCameraPID(0) {
}

BeaconDecoder::~BeaconDecoder() {
}

bool BeaconDecoder::ScanMemory(void* memBase, size_t memSize) {
    if (!memBase || memSize < PAGE_SIZE) {
        return false;
    }
    
    // Clear old data
    pidEntries.clear();
    sectionEntries.clear();
    pteEntries.clear();
    cameraHeaders.clear();
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    size_t validPages = 0;
    
    // Scan all pages
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        if (DecodePage(mem + offset)) {
            validPages++;
        }
    }
    
    if (validPages > 0) {
        std::cout << "Decoded " << validPages << " beacon pages: "
                  << pidEntries.size() << " PIDs, "
                  << sectionEntries.size() << " sections, "
                  << pteEntries.size() << " PTEs\n";
        return true;
    }
    
    return false;
}

bool BeaconDecoder::DecodePage(const uint8_t* pageData) {
    const BeaconPageHeader* header = reinterpret_cast<const BeaconPageHeader*>(pageData);
    
    // Check magic numbers
    if (header->magic1 != BEACON_MAGIC1 || header->magic2 != BEACON_MAGIC2) {
        return false;
    }
    
    // Skip old data (only process recent generations)
    if (lastGeneration != 0 && header->generation < lastGeneration) {
        return false;
    }
    
    // Update tracking
    if (header->generation > lastGeneration) {
        lastGeneration = header->generation;
        lastWriteSeq = 0;
    }
    if (header->write_seq > lastWriteSeq) {
        lastWriteSeq = header->write_seq;
    }
    lastTimestamp = header->timestamp_ns;
    
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
    
    // Associate with current camera PID if set
    if (currentCameraPID != 0 && entry->pid == 0) {
        SectionEntry modEntry = *entry;
        modEntry.pid = currentCameraPID;
        sectionEntries.push_back(modEntry);
    } else {
        sectionEntries.push_back(*entry);
    }
}

void BeaconDecoder::DecodePTEEntry(const uint8_t* data) {
    const PTEEntry* entry = reinterpret_cast<const PTEEntry*>(data);
    pteEntries.push_back(*entry);
}

void BeaconDecoder::DecodeCameraHeader(const uint8_t* data) {
    const CameraHeaderEntry* entry = reinterpret_cast<const CameraHeaderEntry*>(data);
    cameraHeaders.push_back(*entry);
    currentCameraPID = entry->pid; // Set context for following sections/PTEs
}

std::vector<SectionEntry> BeaconDecoder::GetSectionsForPID(uint32_t pid) const {
    std::vector<SectionEntry> result;
    for (const auto& section : sectionEntries) {
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

} // namespace Haywire