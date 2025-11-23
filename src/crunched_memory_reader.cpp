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

    if (!flattener) {
        return 0;
    }

    // Use the SAME logic as main view (GetDirectPointer loop)
    // Pre-allocate buffer
    buffer.resize(size);
    size_t bytesRead = 0;

    // Read in page-aligned chunks using GetDirectPointer
    // CRITICAL: GetDirectPointer returns pointer valid only to end of current page
    const size_t pageSize = 4096;
    for (size_t offset = 0; offset < size; ) {
        uint64_t currentFlatAddr = flatAddress + offset;
        size_t offsetInPage = currentFlatAddr % pageSize;
        size_t bytesLeftInPage = pageSize - offsetInPage;
        size_t bytesLeftTotal = size - offset;

        // Clip to page boundary: can only read to end of current page
        size_t chunkSize = std::min(bytesLeftInPage, bytesLeftTotal);

        const uint8_t* ptr = GetDirectPointer(currentFlatAddr);

        if (ptr) {
            // Direct memcpy from mmap'd memory (fast!)
            // ptr is only valid for chunkSize bytes (to end of page)
            std::memcpy(buffer.data() + offset, ptr, chunkSize);
            bytesRead += chunkSize;
            offset += chunkSize;  // Advance by actual bytes copied
        } else {
            // Unmapped page - check if there's a run of unmapped pages
            size_t bytesRemaining = size - offset;
            size_t runLength = std::min(pageSize, bytesRemaining);

            // Scan ahead for more unmapped pages (only if we have full pages left)
            if (bytesRemaining >= pageSize) {
                size_t remainingPages = (bytesRemaining - pageSize) / pageSize;
                for (size_t i = 1; i <= remainingPages; i++) {
                    if (!IsPageKnownUnmapped(flatAddress + offset + i * pageSize)) {
                        break;  // Hit a mapped page, stop run
                    }
                    runLength += pageSize;
                }
            }

            // Fill run with zeros
            std::memset(buffer.data() + offset, 0, runLength);
            offset += runLength;  // Advance by actual bytes filled
        }
    }

    MICRO_TIMER_STOP(timer_ReadCrunchedMemory);
    return bytesRead;
}

void CrunchedMemoryReader::InitializeRenderCache() {
    // Only used in VA mode
    // PID 0 is valid (used for query results), only reject negative PIDs
    if (!flattener || !translator || targetPid < 0) {
        renderPageCache.clear();
        return;
    }

    uint64_t crunchedSize = flattener->GetFlatSize();
    size_t numPages = (crunchedSize + PAGE_SIZE - 1) / PAGE_SIZE;

    // Build VA lookup table eagerly (O(1) flat→VA, no more binary search!)
    renderPageCache.clear();
    renderPageCache.resize(numPages);

    size_t pagesWithVA = 0;
    const auto& regions = flattener->GetRegions();
    for (const auto& region : regions) {
        // For each BYTE in this region, we need to cover it with a page in the cache
        // IMPORTANT: flatStart might not be page-aligned!
        uint64_t regionStartFlat = region.flatStart;
        uint64_t regionEndFlat = region.flatEnd;
        size_t startPageIdx = regionStartFlat / PAGE_SIZE;
        size_t endPageIdx = (regionEndFlat + PAGE_SIZE - 1) / PAGE_SIZE;  // Round up

        for (size_t pageIdx = startPageIdx; pageIdx < endPageIdx && pageIdx < numPages; pageIdx++) {
            // Calculate what VA this flat page corresponds to
            uint64_t flatPageStart = pageIdx * PAGE_SIZE;
            // How many bytes into the region is this page?
            int64_t offsetIntoRegion = flatPageStart - regionStartFlat;
            if (offsetIntoRegion < 0) offsetIntoRegion = 0;  // Partial first page

            uint64_t pageVA = region.virtualStart + offsetIntoRegion;
            renderPageCache[pageIdx].va = pageVA;
            renderPageCache[pageIdx].pa = 0;  // Will translate lazily
            renderPageCache[pageIdx].flags = 0;
            pagesWithVA++;
        }
    }

    std::cout << "Rendering page cache: " << numPages << " pages (" << pagesWithVA << " with VA, "
              << (numPages - pagesWithVA) << " gaps, "
              << (numPages * sizeof(PageCacheEntry) / (1024.0 * 1024.0)) << " MB)\n";
}

const uint8_t* CrunchedMemoryReader::GetDirectPointer(uint64_t flatAddress) {
    if (!flattener || !qemu) {
        return nullptr;
    }

    // Check if we're in VA mode (have translator and PID)
    // PID 0 is valid (used for query results)
    if (translator && targetPid >= 0 && !renderPageCache.empty()) {
        // VA mode: Use rendering page cache (O(1) flat→VA→PA, no binary search!)
        size_t pageIdx = flatAddress / PAGE_SIZE;
        if (pageIdx >= renderPageCache.size()) {
            return nullptr;  // Out of bounds
        }

        auto& entry = renderPageCache[pageIdx];

        // Check for cached unmapped page (marked as bad)
        if (entry.flags != 0) {
            // DEBUG: Log cached unmapped pages
            static int cachedUnmappedCount = 0;
            if (++cachedUnmappedCount <= 5) {
                std::cerr << "CrunchedReader[VA]: flat 0x" << std::hex << flatAddress
                          << " already marked unmapped in cache" << std::dec << std::endl;
            }
            return nullptr;  // Previously determined to be unmapped, skip immediately
        }

        uint64_t physAddr = entry.pa;

        // Lazy translation: get PA from flattener's pre-populated cache
        if (physAddr == 0) {
            // Use AddressSpaceFlattener's PA cache (pre-populated from PTEs)
            // This is MUCH faster and more accurate than fresh page table walks
            physAddr = flattener->GetPhysicalAddress(flatAddress & ~(PAGE_SIZE - 1));

            // Cache the result - mark unmapped pages with flag
            if (physAddr != 0) {
                entry.pa = physAddr;
            } else {
                // DEBUG: Log pages not in PA cache
                static int unmappedCount = 0;
                if (++unmappedCount <= 10) {
                    std::cerr << "CrunchedReader::GetDirectPointer: flat 0x" << std::hex << flatAddress
                              << " not in PA cache (page not resident)" << std::dec << std::endl;
                }
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
        if (!ptr) {
            // DEBUG: Log when backend fails to return pointer (VA mode)
            static int backendFailCount = 0;
            if (++backendFailCount <= 10) {
                std::cerr << "CrunchedReader[VA]: backend returned nullptr for PA 0x"
                          << std::hex << physAddr << " (flat 0x" << flatAddress << ")" << std::dec << std::endl;
            }
        }
        return ptr;
    } else {
        // PA mode: Use AddressSpaceFlattener to map flat→PA
        // In PA mode, the flattener's "virtual" addresses ARE physical addresses
        uint64_t physAddr = flattener->FlatToVirtual(flatAddress);
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
    // PID 0 is valid (used for query results)
    if (!translator || targetPid < 0 || renderPageCache.empty()) {
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

    // PID 0 is valid (used for query results)
    if (translator && targetPid >= 0) {
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