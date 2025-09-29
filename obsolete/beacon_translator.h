#pragma once

#include <cstdint>
#include <unordered_map>
#include <memory>

namespace Haywire {

class BeaconReader;

// Translates virtual addresses to physical addresses using beacon PTE data
class BeaconTranslator {
public:
    BeaconTranslator(std::shared_ptr<BeaconReader> reader);
    ~BeaconTranslator();
    
    // Translate a virtual address to physical for a given PID
    // Returns 0 if translation fails (page not present)
    uint64_t TranslateAddress(int pid, uint64_t virtualAddr);
    
    // Update translation cache from beacon data
    void UpdateFromBeacon(int requestedPid = 0);
    
private:
    std::shared_ptr<BeaconReader> reader;
    
    // Cache of VA->PA mappings per PID
    // Map of PID -> (VA -> PA)
    std::unordered_map<int, std::unordered_map<uint64_t, uint64_t>> pteCache;
    
    // Helper to align address to page boundary
    static uint64_t AlignToPage(uint64_t addr) {
        return addr & ~0xFFFULL;
    }
};

}