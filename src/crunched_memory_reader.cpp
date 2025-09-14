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
    static bool firstCall = true;
    
    if (!flattener) {
        if (firstCall) std::cerr << "CrunchedReader: No flattener!" << std::endl;
        return 0;
    }
    if (!translator && !beaconTranslator) {
        if (firstCall) std::cerr << "CrunchedReader: No translator (neither viewport nor beacon)!" << std::endl;
        return 0;
    }
    if (!qemu) {
        if (firstCall) std::cerr << "CrunchedReader: No QEMU connection!" << std::endl;
        return 0;
    }
    if (targetPid < 0) {
        if (firstCall) std::cerr << "CrunchedReader: Invalid PID: " << targetPid << std::endl;
        return 0;
    }
    
    if (firstCall) {
        std::cerr << "VA Mode: Reading crunched memory for PID " << targetPid
                  << " using " << (beaconTranslator ? "BeaconTranslator" : "ViewportTranslator") 
                  << std::endl;
        std::cerr << "  Flattened address space size: 0x" << std::hex 
                  << flattener->GetFlatSize() << std::dec << " bytes\n";
        firstCall = false;
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
                static int noRegionCount = 0;
                if (++noRegionCount <= 3) {
                    std::cerr << "CrunchedReader: No region at flat address 0x" 
                              << std::hex << currentFlat << std::dec 
                              << " (requested flat 0x" << flatAddress << ")" << std::endl;
                }
            }
            break;
        }
        
        static bool showedRegion = false;
        if (!showedRegion && totalRead == 0) {
            std::cerr << "VA Mode: Flat 0x" << std::hex << currentFlat 
                      << " -> Region [0x" << region->virtualStart 
                      << "-0x" << region->virtualEnd << "] " 
                      << region->name << std::dec << std::endl;
            showedRegion = true;
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
            
            static int translationCount = 0;
            if (++translationCount <= 5) {
                std::cerr << "VA->PA: 0x" << std::hex << chunkVA << " -> ";
                if (physAddr != 0) {
                    std::cerr << "0x" << physAddr << std::dec << " (success)" << std::endl;
                } else {
                    std::cerr << "not mapped" << std::dec << std::endl;
                }
            }
            
            if (physAddr == 0) {
                // Page not present - fill with zeros but remember it's unmapped
                buffer.resize(buffer.size() + chunkSize, 0);
                static int notPresentCount = 0;
                if (++notPresentCount <= 3) {
                    std::cerr << "Page not present at VA 0x" << std::hex << chunkVA 
                              << " in region " << region->name
                              << " (occurrence " << std::dec << notPresentCount << ")" << std::endl;
                }
            } else {
                // Read from physical memory
                std::vector<uint8_t> tempBuffer;
                
                // Debug: Check if PA is in valid range and test offset
                static int readCount = 0;
                if (++readCount <= 10) {
                    std::cerr << "Reading from PA 0x" << std::hex << physAddr;
                    if (physAddr >= 0x40000000) {
                        uint64_t testOffset = physAddr - 0x40000000;
                        std::cerr << " (file offset would be 0x" << std::hex << testOffset << ")";
                    } else {
                        std::cerr << " (below guest RAM start 0x40000000!)";
                    }
                    std::cerr << std::dec << std::endl;
                }
                
                if (qemu->ReadMemory(physAddr, chunkSize, tempBuffer)) {
                    buffer.insert(buffer.end(), tempBuffer.begin(), tempBuffer.end());
                    
                    // Debug: Check if we're getting non-zero data
                    if (readCount <= 10) {
                        bool hasNonZero = false;
                        for (size_t i = 0; i < std::min<size_t>(16, tempBuffer.size()); i++) {
                            if (tempBuffer[i] != 0) {
                                hasNonZero = true;
                                break;
                            }
                        }
                        std::cerr << "  Got " << tempBuffer.size() << " bytes" 
                                  << (hasNonZero ? " (has data)" : " (all zeros)") 
                                  << std::endl;
                    }
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

bool CrunchedMemoryReader::TestPageNonZero(uint64_t flatAddress, size_t size) {
    if (!flattener || !qemu || targetPid < 0) {
        return false;
    }

    if (!translator && !beaconTranslator) {
        return false;
    }

    // Find which region we're in
    const auto* region = flattener->GetRegionForFlat(flatAddress);
    if (!region) {
        // Hit unmapped space - consider it zero
        return false;
    }

    // Calculate offset within this region
    uint64_t offsetInRegion = flatAddress - region->flatStart;
    uint64_t virtualAddr = region->virtualStart + offsetInRegion;

    // Don't cross region boundaries
    size_t regionSize = region->virtualEnd - region->virtualStart;
    size_t remainingInRegion = regionSize - offsetInRegion;
    size_t toTest = std::min(remainingInRegion, size);

    // Process in page-sized chunks as the underlying memory is page-aligned
    const size_t pageSize = 4096;
    size_t tested = 0;

    while (tested < toTest) {
        size_t chunkSize = std::min<size_t>(pageSize, toTest - tested);
        uint64_t chunkVA = virtualAddr + tested;

        // Translate VA to PA
        uint64_t physAddr = 0;
        if (beaconTranslator) {
            physAddr = beaconTranslator->TranslateAddress(targetPid, chunkVA);
        } else if (translator) {
            physAddr = translator->TranslateAddress(targetPid, chunkVA);
        }

        if (physAddr == 0) {
            // Page not present - it's all zeros
            tested += chunkSize;
            continue;
        }

        // Use the zero-copy TestPageNonZero on the physical address
        if (qemu->TestPageNonZero(physAddr, chunkSize)) {
            return true;  // Found non-zero data
        }

        tested += chunkSize;
    }

    return false;  // All zeros or unmapped
}

}