#ifndef BEACON_MAP_H
#define BEACON_MAP_H

#include <stdint.h>
#include <unordered_map>
#include <vector>
#include <chrono>

#define MAX_BEACONS 8192
#define PAGE_SIZE 4096
#define BEACON_MAGIC1 0x3142FACE
#define BEACON_MAGIC2 0xCAFEBABE

struct BeaconInfo {
    uint64_t phys_addr;      // Physical address in memory file
    uint32_t session_id;     // Session ID from beacon
    uint32_t protocol_ver;   // Protocol version
    uint64_t timestamp;      // When we discovered it
    uint32_t page_index;     // Which page in the companion's allocation
    bool is_active;          // Still valid?
    
    // Quick accessors
    uint64_t request_addr() const { return phys_addr + PAGE_SIZE; }      // Requests at page 1
    uint64_t response_addr() const { return phys_addr + 5 * PAGE_SIZE; } // Responses at page 5
    uint64_t data_addr() const { return phys_addr + 9 * PAGE_SIZE; }     // Data at page 9
};

class BeaconMap {
private:
    // Primary storage - indexed array
    std::vector<BeaconInfo> beacons;
    
    // Bidirectional mappings
    std::unordered_map<uint64_t, size_t> addr_to_index;  // phys_addr -> beacon index
    std::unordered_map<uint32_t, std::vector<size_t>> session_to_indices; // session -> beacon indices
    
    // Memory file handle
    void* mapped_memory;
    size_t mapped_size;
    
public:
    BeaconMap() : mapped_memory(nullptr), mapped_size(0) {}
    
    // Add a discovered beacon
    size_t add_beacon(uint64_t phys_addr, uint32_t session_id, uint32_t protocol_ver, uint32_t page_index) {
        BeaconInfo info;
        info.phys_addr = phys_addr;
        info.session_id = session_id;
        info.protocol_ver = protocol_ver;
        info.page_index = page_index;
        info.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        info.is_active = true;
        
        size_t index = beacons.size();
        beacons.push_back(info);
        
        // Update mappings
        addr_to_index[phys_addr] = index;
        session_to_indices[session_id].push_back(index);
        
        return index;
    }
    
    // Find beacon by physical address
    BeaconInfo* find_by_addr(uint64_t phys_addr) {
        auto it = addr_to_index.find(phys_addr);
        if (it != addr_to_index.end()) {
            return &beacons[it->second];
        }
        return nullptr;
    }
    
    // Find all beacons for a session
    std::vector<BeaconInfo*> find_by_session(uint32_t session_id) {
        std::vector<BeaconInfo*> result;
        auto it = session_to_indices.find(session_id);
        if (it != session_to_indices.end()) {
            for (size_t idx : it->second) {
                if (beacons[idx].is_active) {
                    result.push_back(&beacons[idx]);
                }
            }
        }
        return result;
    }
    
    // Get beacon by index
    BeaconInfo* get_by_index(size_t index) {
        if (index < beacons.size()) {
            return &beacons[index];
        }
        return nullptr;
    }
    
    // Find contiguous beacon region for a session
    struct Region {
        uint64_t base_addr;
        size_t page_count;
        uint32_t session_id;
        uint32_t protocol_ver;
    };
    
    std::vector<Region> find_regions(uint32_t session_id) {
        std::vector<Region> regions;
        auto session_beacons = find_by_session(session_id);
        
        if (session_beacons.empty()) return regions;
        
        // Sort by physical address
        std::sort(session_beacons.begin(), session_beacons.end(),
                  [](BeaconInfo* a, BeaconInfo* b) { return a->phys_addr < b->phys_addr; });
        
        // Find contiguous regions
        Region current;
        current.base_addr = session_beacons[0]->phys_addr;
        current.page_count = 1;
        current.session_id = session_id;
        current.protocol_ver = session_beacons[0]->protocol_ver;
        
        for (size_t i = 1; i < session_beacons.size(); i++) {
            uint64_t expected_addr = current.base_addr + current.page_count * PAGE_SIZE;
            if (session_beacons[i]->phys_addr == expected_addr) {
                // Contiguous
                current.page_count++;
            } else {
                // Gap - save current region and start new one
                regions.push_back(current);
                current.base_addr = session_beacons[i]->phys_addr;
                current.page_count = 1;
            }
        }
        regions.push_back(current);
        
        return regions;
    }
    
    // Mark stale beacons (e.g., old timestamp or dead session)
    void mark_stale(uint32_t session_id) {
        auto it = session_to_indices.find(session_id);
        if (it != session_to_indices.end()) {
            for (size_t idx : it->second) {
                beacons[idx].is_active = false;
            }
        }
    }
    
    // Clean up inactive beacons from mappings
    void cleanup() {
        // Remove inactive from addr mapping
        auto addr_it = addr_to_index.begin();
        while (addr_it != addr_to_index.end()) {
            if (!beacons[addr_it->second].is_active) {
                addr_it = addr_to_index.erase(addr_it);
            } else {
                ++addr_it;
            }
        }
        
        // Remove inactive from session mapping
        for (auto& pair : session_to_indices) {
            auto& indices = pair.second;
            indices.erase(
                std::remove_if(indices.begin(), indices.end(),
                    [this](size_t idx) { return !beacons[idx].is_active; }),
                indices.end());
        }
    }
    
    // Stats
    size_t total_beacons() const { return beacons.size(); }
    size_t active_beacons() const {
        return std::count_if(beacons.begin(), beacons.end(),
                           [](const BeaconInfo& b) { return b.is_active; });
    }
    
    // Clear everything
    void clear() {
        beacons.clear();
        addr_to_index.clear();
        session_to_indices.clear();
    }
};

#endif // BEACON_MAP_H