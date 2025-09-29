#include "beacon_translator.h"
#include "beacon_reader.h"
#include <iostream>

namespace Haywire {

BeaconTranslator::BeaconTranslator(std::shared_ptr<BeaconReader> reader) 
    : reader(reader) {
}

BeaconTranslator::~BeaconTranslator() {
}

uint64_t BeaconTranslator::TranslateAddress(int pid, uint64_t virtualAddr) {
    // Update cache from latest beacon data (rate limited)
    static int callCount = 0;
    if (++callCount % 100 == 1) {  // Update every 100 calls
        UpdateFromBeacon(pid);  // Pass the PID we actually need
    }
    
    // WORKAROUND: Since GetCameraTargetPID isn't working properly,
    // just use whatever PTEs we have, assuming they're for the requested PID
    // This works because we only focus one camera at a time on one PID
    
    // First try to find PTEs for the exact PID
    auto pidIt = pteCache.find(pid);
    if (pidIt == pteCache.end()) {
        // If not found, check if we have PTEs cached with PID 0
        // (happens when decoder doesn't know the PID)
        pidIt = pteCache.find(0);
        if (pidIt != pteCache.end()) {
            // Move these PTEs to the correct PID for future lookups
            pteCache[pid] = std::move(pidIt->second);
            pteCache.erase(0);
            pidIt = pteCache.find(pid);
            
            static bool movedPtes = false;
            if (!movedPtes) {
                std::cerr << "BeaconTranslator: Moved PTEs from PID 0 to PID " << pid 
                          << " (UI-selected PID)" << std::endl;
                movedPtes = true;
            }
        } else if (!pteCache.empty()) {
            // Last resort: use any PTEs we have
            pidIt = pteCache.begin();
            static bool warnedWorkaround = false;
            if (!warnedWorkaround) {
                std::cerr << "BeaconTranslator: Using PTEs from PID " << pidIt->first 
                          << " for requested PID " << pid << " (workaround)" << std::endl;
                warnedWorkaround = true;
            }
        } else {
            static bool warnedNoPid = false;
            if (!warnedNoPid) {
                std::cerr << "BeaconTranslator: No PTE data available at all" << std::endl;
                warnedNoPid = true;
            }
            return 0;
        }
    }
    
    // Align to page boundary
    uint64_t pageVA = AlignToPage(virtualAddr);
    uint64_t offset = virtualAddr & 0xFFF;
    
    // Look up the page
    auto pteIt = pidIt->second.find(pageVA);
    if (pteIt == pidIt->second.end()) {
        return 0; // Page not mapped
    }
    
    // Return physical address with offset
    return pteIt->second + offset;
}

void BeaconTranslator::UpdateFromBeacon(int requestedPid) {
    if (!reader) {
        std::cerr << "BeaconTranslator: No reader!" << std::endl;
        return;
    }

    static int updateCount = 0;
    static int emptyCount = 0;

    // Get PTEs from both cameras for the requested PID
    // Ignore control page - just ask for PTEs for the PID we need
    for (int camera = 1; camera <= 2; camera++) {
        uint32_t pid = requestedPid > 0 ? requestedPid : 0;
        if (pid == 0) {
            continue;  // No valid PID
        }

        // std::cerr << "*** BeaconTranslator: Requesting PTEs for PID " << pid << " from camera " << camera << std::endl;

        // Get PTEs for this PID from this camera
        std::unordered_map<uint64_t, uint64_t> ptes;
        if (!reader->GetCameraPTEs(camera, pid, ptes)) {
            if (++emptyCount <= 10) {
                std::cerr << "BeaconTranslator: Camera " << camera << " returned no PTEs for PID " << pid << std::endl;
            }
            continue;
        }
        
        if (ptes.empty()) {
            continue;
        }
        
        // Update cache for this PID
        auto& pidCache = pteCache[pid];
        int newPtes = 0;
        for (const auto& [va, pa] : ptes) {
            uint64_t pageVA = AlignToPage(va);
            if (pidCache.find(pageVA) == pidCache.end()) {
                newPtes++;
            }
            pidCache[pageVA] = pa; // PA is already page-aligned from beacon
        }
        
        if (ptes.size() > 0 && ++updateCount <= 5) {
            std::cerr << "BeaconTranslator: Camera " << camera << " provided " 
                      << ptes.size() << " PTEs for PID " << pid 
                      << " (" << newPtes << " new)" << std::endl;
        }
    }
}

}