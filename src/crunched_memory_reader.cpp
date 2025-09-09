#include "crunched_memory_reader.h"
#include <iostream>
#include <algorithm>

namespace Haywire {

CrunchedMemoryReader::CrunchedMemoryReader() 
    : flattener(nullptr), translator(nullptr), beaconTranslator(nullptr), 
      qemu(nullptr), targetPid(-1) {
}

CrunchedMemoryReader::~CrunchedMemoryReader() {
}

size_t CrunchedMemoryReader::ReadCrunchedMemory(uint64_t flatAddress, size_t size, 
                                                std::vector<uint8_t>& buffer) {
    if (!flattener) {
        std::cerr << "CrunchedReader: No flattener!" << std::endl;
        return 0;
    }
    if (!translator && !beaconTranslator) {
        std::cerr << "CrunchedReader: No translator (neither viewport nor beacon)!" << std::endl;
        return 0;
    }
    if (!qemu) {
        std::cerr << "CrunchedReader: No QEMU connection!" << std::endl;
        return 0;
    }
    if (targetPid < 0) {
        std::cerr << "CrunchedReader: Invalid PID: " << targetPid << std::endl;
        return 0;
    }
    
    buffer.clear();
    buffer.reserve(size);
    
    // Track translation time
    static int readCount = 0;
    readCount++;
    
    size_t totalRead = 0;
    uint64_t currentFlat = flatAddress;
    int translationsNeeded = 0;
    
    while (totalRead < size) {
        // Find which region we're in
        const auto* region = flattener->GetRegionForFlat(currentFlat);
        if (!region) {
            // Hit unmapped space
            if (totalRead == 0) {
                std::cerr << "CrunchedReader: No region at flat address 0x" 
                          << std::hex << currentFlat << std::dec << std::endl;
            }
            break;
        }
        
        // Calculate offset within this region
        uint64_t offsetInRegion = currentFlat - region->flatStart;
        uint64_t virtualAddr = region->virtualStart + offsetInRegion;
        
        // How much can we read from this region?
        size_t regionSize = region->virtualEnd - region->virtualStart;
        size_t remainingInRegion = regionSize - offsetInRegion;
        size_t toRead = std::min(remainingInRegion, size - totalRead);
        
        // Read in page-sized chunks for efficiency
        const size_t pageSize = 4096;
        size_t regionBytesRead = 0;
        
        while (regionBytesRead < toRead) {
            size_t chunkSize = std::min<size_t>(pageSize, toRead - regionBytesRead);
            uint64_t chunkVA = virtualAddr + regionBytesRead;
            
            // Translate VA to PA using beacon translator if available, otherwise viewport
            translationsNeeded++;
            uint64_t physAddr = 0;
            if (beaconTranslator) {
                physAddr = beaconTranslator->TranslateAddress(targetPid, chunkVA);
            } else if (translator) {
                physAddr = translator->TranslateAddress(targetPid, chunkVA);
            }
            
            if (physAddr == 0) {
                // Page not present - fill with zeros
                buffer.resize(buffer.size() + chunkSize, 0);
                static int notPresentCount = 0;
                if (++notPresentCount <= 10) {
                    std::cerr << "Page not present at VA 0x" << std::hex << chunkVA 
                              << " (occurrence " << std::dec << notPresentCount << ")" << std::endl;
                }
            } else {
                // Read from physical memory
                std::vector<uint8_t> tempBuffer;
                if (qemu->ReadMemory(physAddr, chunkSize, tempBuffer)) {
                    buffer.insert(buffer.end(), tempBuffer.begin(), tempBuffer.end());
                } else {
                    // Read failed - fill with zeros
                    buffer.resize(buffer.size() + chunkSize, 0);
                    static int readFailCount = 0;
                    if (++readFailCount <= 10) {
                        std::cerr << "Physical read failed at PA 0x" << std::hex << physAddr 
                                  << " from VA 0x" << chunkVA << std::dec << std::endl;
                    }
                }
            }
            
            regionBytesRead += chunkSize;
        }
        
        totalRead += regionBytesRead;
        currentFlat += regionBytesRead;
    }
    
    // Log read statistics for first few reads only
    if (readCount <= 5) {
        std::cerr << "CrunchedRead #" << readCount << ": " << translationsNeeded 
                  << " translations for " << totalRead << " bytes" << std::endl;
    }
    
    return totalRead;
}

CrunchedMemoryReader::PositionInfo CrunchedMemoryReader::GetPositionInfo(uint64_t flatAddress) const {
    PositionInfo info;
    info.flatAddr = flatAddress;
    info.isValid = false;
    
    if (!flattener) {
        return info;
    }
    
    const auto* region = flattener->GetRegionForFlat(flatAddress);
    if (!region) {
        return info;
    }
    
    uint64_t offsetInRegion = flatAddress - region->flatStart;
    info.virtualAddr = region->virtualStart + offsetInRegion;
    info.regionName = region->name;
    info.isValid = true;
    
    if (translator && targetPid > 0) {
        info.physicalAddr = translator->TranslateAddress(targetPid, info.virtualAddr);
    } else {
        info.physicalAddr = 0;
    }
    
    return info;
}

}