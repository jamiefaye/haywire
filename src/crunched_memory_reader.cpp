#include "crunched_memory_reader.h"
#include "micro_timer.h"
#include <iostream>
#include <algorithm>

namespace Haywire {

// Timing instrumentation (toggle MICRO_TIMER_ENABLE in micro_timer.h)
MICRO_TIMER_DECL(timer_ReadCrunchedMemory, 100);
MICRO_TIMER_DECL(timer_GetRegionForFlat, 1000);
MICRO_TIMER_DECL(timer_TranslateAddress, 1000);

CrunchedMemoryReader::CrunchedMemoryReader()
    : flattener(nullptr), translator(nullptr), // beaconTranslator removed
      qemu(nullptr), targetPid(-1) {
}

CrunchedMemoryReader::~CrunchedMemoryReader() {
}

size_t CrunchedMemoryReader::ReadCrunchedMemory(uint64_t flatAddress, size_t size,
                                                std::vector<uint8_t>& buffer) {
    MICRO_TIMER_START(timer_ReadCrunchedMemory);
    static bool firstCall = true;
    
    if (!flattener) {
        if (firstCall) std::cerr << "CrunchedReader: No flattener!" << std::endl;
        return 0;
    }
    if (!translator) {
        if (firstCall) std::cerr << "CrunchedReader: No translator!" << std::endl;
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
                  << " using ViewportTranslator" 
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
            uint64_t chunkFlatAddr = currentFlat + totalRead + regionBytesRead;

            // Try PA lookup table first (fast path)
            uint64_t physAddr = 0;
            if (flattener) {
                physAddr = flattener->GetPhysicalAddress(chunkFlatAddr);
            }

            // Fallback to translation if lookup failed
            if (physAddr == 0 && translator) {
                translationsNeeded++;
                uint64_t chunkVA = virtualAddr + regionBytesRead;

                // Validate VA before translation to prevent crashes
                // ARM64 userspace VA max is 0x0001000000000000 (48-bit address space)
                if (chunkVA < 0x0001000000000000ULL) {
                    physAddr = translator->TranslateAddress(targetPid, chunkVA);
                } else {
                    static int invalidVACount = 0;
                    if (++invalidVACount <= 3) {
                        std::cerr << "CrunchedReader: Invalid VA 0x" << std::hex << chunkVA
                                  << " exceeds userspace limit" << std::dec << std::endl;
                    }
                }
            }
            
            // Debug: VA->PA translation (disabled for production)
            // static int translationCount = 0;
            // if (++translationCount <= 5) {
            //     std::cerr << "VA->PA: 0x" << std::hex << chunkVA << " -> ";
            //     if (physAddr != 0) {
            //         std::cerr << "0x" << physAddr << std::dec << " (success)" << std::endl;
            //     } else {
            //         std::cerr << "not mapped" << std::dec << std::endl;
            //     }
            // }
            
            if (physAddr == 0) {
                // Page not present - fill with zeros but remember it's unmapped
                buffer.resize(buffer.size() + chunkSize, 0);
            } else {
                // Read from physical memory
                std::vector<uint8_t> tempBuffer;

                if (qemu->ReadMemory(physAddr, chunkSize, tempBuffer)) {
                    buffer.insert(buffer.end(), tempBuffer.begin(), tempBuffer.end());
                } else {
                    // Read failed - fill with zeros
                    buffer.resize(buffer.size() + chunkSize, 0);
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

    MICRO_TIMER_STOP(timer_ReadCrunchedMemory);
    return totalRead;
}

void CrunchedMemoryReader::InitializeRenderCache() {
    // Only used in VA mode
    if (!flattener || !translator || targetPid <= 0) {
        renderPageCache.clear();
        return;
    }

    uint64_t crunchedSize = flattener->GetFlatSize();
    size_t numPages = (crunchedSize + PAGE_SIZE - 1) / PAGE_SIZE;

    // Build VA lookup table eagerly (O(1) flat→VA, no more binary search!)
    renderPageCache.clear();
    renderPageCache.resize(numPages);

    const auto& regions = flattener->GetRegions();
    for (const auto& region : regions) {
        // For each page in this region, store its VA
        uint64_t regionPages = (region.FlatSize() + PAGE_SIZE - 1) / PAGE_SIZE;
        size_t startPageIdx = region.flatStart / PAGE_SIZE;

        for (size_t i = 0; i < regionPages; i++) {
            size_t pageIdx = startPageIdx + i;
            if (pageIdx < numPages) {
                uint64_t pageVA = region.virtualStart + (i * PAGE_SIZE);
                renderPageCache[pageIdx].va = pageVA;
                renderPageCache[pageIdx].pa = 0;  // Will translate lazily
                renderPageCache[pageIdx].flags = 0;
            }
        }
    }

    std::cout << "Rendering page cache: " << numPages << " pages ("
              << (numPages * sizeof(PageCacheEntry) / (1024.0 * 1024.0)) << " MB)\n";
}

const uint8_t* CrunchedMemoryReader::GetDirectPointer(uint64_t flatAddress) {
    if (!flattener || !qemu) {
        return nullptr;
    }

    // Check if we're in VA mode (have translator and PID)
    if (translator && targetPid > 0 && !renderPageCache.empty()) {
        // VA mode: Use rendering page cache (O(1) flat→VA→PA, no binary search!)
        size_t pageIdx = flatAddress / PAGE_SIZE;
        if (pageIdx >= renderPageCache.size()) {
            return nullptr;  // Out of bounds
        }

        auto& entry = renderPageCache[pageIdx];

        // Check for cached unmapped page (marked as bad)
        if (entry.flags != 0) {
            return nullptr;  // Previously determined to be unmapped, skip immediately
        }

        uint64_t physAddr = entry.pa;

        // Lazy translation: translate on first access
        if (physAddr == 0) {
            // VA is already cached from initialization (O(1) lookup!)
            uint64_t pageVA = (entry.va / PAGE_SIZE) * PAGE_SIZE;
            physAddr = translator->TranslateAddress(targetPid, pageVA);

            // Cache the result - mark unmapped pages with flag
            if (physAddr != 0) {
                entry.pa = physAddr;
            } else {
                // Mark as unmapped so we skip it immediately next time
                entry.flags = 1;
                return nullptr;
            }
        }

        // Add offset within page
        physAddr += (flatAddress % PAGE_SIZE);

        // Get memory backend and return direct pointer
        auto* backend = qemu->GetMemoryBackend();
        if (!backend || !backend->IsAvailable()) {
            return nullptr;
        }

        const uint8_t* ptr = backend->GetDirectPointer(physAddr);
        return ptr;
    } else {
        // PA mode or old path: Use AddressSpaceFlattener's PA lookup
        uint64_t physAddr = flattener->GetPhysicalAddress(flatAddress);
        if (physAddr == 0) {
            return nullptr;  // Not mapped
        }

        // Get memory backend and return direct pointer
        auto* backend = qemu->GetMemoryBackend();
        if (!backend || !backend->IsAvailable()) {
            return nullptr;
        }

        const uint8_t* ptr = backend->GetDirectPointer(physAddr);
        return ptr;
    }
}

bool CrunchedMemoryReader::IsPageKnownUnmapped(uint64_t flatAddress) const {
    // Only valid in VA mode with cache
    if (!translator || targetPid <= 0 || renderPageCache.empty()) {
        return false;
    }

    size_t pageIdx = flatAddress / PAGE_SIZE;
    if (pageIdx >= renderPageCache.size()) {
        return false;
    }

    // Check if page is marked as unmapped (flags != 0)
    return renderPageCache[pageIdx].flags != 0;
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

    if (!translator) {
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
        if (translator) {
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