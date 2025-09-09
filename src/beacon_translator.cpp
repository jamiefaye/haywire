#include "beacon_translator.h"
#include <iostream>

namespace Haywire {

BeaconTranslator::BeaconTranslator(std::shared_ptr<BeaconDecoder> decoder) 
    : decoder(decoder) {
}

BeaconTranslator::~BeaconTranslator() {
}

uint64_t BeaconTranslator::TranslateAddress(int pid, uint64_t virtualAddr) {
    // Update cache from latest beacon data
    UpdateFromBeacon();
    
    // Check if we have data for this PID
    auto pidIt = pteCache.find(pid);
    if (pidIt == pteCache.end()) {
        return 0; // No data for this PID
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

void BeaconTranslator::UpdateFromBeacon() {
    if (!decoder) return;
    
    // Get PTEs from both cameras
    for (int camera = 1; camera <= 2; camera++) {
        auto ptes = decoder->GetCameraPTEs(camera);
        if (ptes.empty()) continue;
        
        // Get the PID for this camera
        int pid = decoder->GetCameraTargetPID(camera);
        if (pid <= 0) continue;
        
        // Update cache for this PID
        auto& pidCache = pteCache[pid];
        for (const auto& [va, pa] : ptes) {
            uint64_t pageVA = AlignToPage(va);
            pidCache[pageVA] = pa; // PA is already page-aligned from beacon
        }
    }
}

}