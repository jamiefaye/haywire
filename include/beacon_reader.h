#pragma once

#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <memory>
#include "beacon_protocol.h"

namespace Haywire {

// Forward declarations
class BeaconDecoder;
struct SectionEntry;

// Process information from beacon pages
struct BeaconProcessInfo {
    uint32_t pid;
    uint32_t ppid;
    std::string name;      // Process name (comm)
    char state;            // R/S/D/Z/T
    uint64_t vsize;        // Virtual memory size
    uint64_t rss;          // Resident set size
    uint32_t num_threads;
    std::string exe_path;  // Executable path
    bool hasDetails;       // True if we have full ProcessEntry data
};

// PID list generation from beacon
struct PIDGeneration {
    uint32_t generation;
    uint32_t total_pids;
    std::vector<uint32_t> pids;
    bool is_complete;
};

class BeaconReader {
public:
    BeaconReader();
    ~BeaconReader();
    
    // Initialize with memory file path
    bool Initialize(const std::string& memoryPath = "/tmp/haywire-vm-mem");
    void Cleanup();
    
    // Find and read discovery page
    bool FindDiscovery();
    
    // Get PID list from most recent complete generation
    bool GetPIDList(std::vector<uint32_t>& pids);
    
    // Get all available PID generations
    std::vector<PIDGeneration> GetPIDGenerations();
    
    // Get process info from beacon data
    bool GetProcessInfo(uint32_t pid, BeaconProcessInfo& info);
    
    // Get all processes with details from beacons
    std::map<uint32_t, BeaconProcessInfo> GetAllProcessInfo();
    
    // Write to camera control page
    bool SetCameraFocus(int cameraId, uint32_t pid);
    
    // Get current camera focus
    uint32_t GetCameraFocus(int cameraId);
    
    // Get process sections from camera data
    bool GetCameraProcessSections(int cameraId, uint32_t pid, std::vector<SectionEntry>& sections);
    
    // Get process PTEs from camera data (for crunched view)
    bool GetCameraPTEs(int cameraId, uint32_t pid, std::unordered_map<uint64_t, uint64_t>& ptes);
    
    // Companion management
    bool StartCompanion(class GuestAgent* agent);
    bool RefreshCompanion(class GuestAgent* agent, uint32_t focusPid = 0);
    bool IsCompanionRunning();
    bool StopCompanion(class GuestAgent* agent);
    
    // Get the decoder for external use
    std::shared_ptr<BeaconDecoder> GetDecoder() const { 
        return decoder; 
    }
    
    // Get direct memory pointer for a given guest physical address
    const uint8_t* GetMemoryPointer(uint64_t gpa) const {
        if (!memBase || gpa >= memSize) {
            return nullptr;
        }
        return static_cast<const uint8_t*>(memBase) + gpa;
    }
    
    // Get the size of the memory-backend-file
    size_t GetMemorySize() const { return memSize; }
    
private:
    // Memory mapping
    int memFd;
    void* memBase;
    size_t memSize;
    
    // Companion management
    uint32_t companionPid;
    uint32_t lastCompanionCheck;
    
    // New beacon decoder
    std::shared_ptr<BeaconDecoder> decoder;
    
    // Discovery information
    struct DiscoveryInfo {
        uint64_t offset;
        uint32_t version;
        uint32_t pid;
        uint32_t timestamp;
        
        struct CategoryInfo {
            uint32_t base_offset;
            uint32_t page_count;
            uint32_t write_index;
            uint32_t sequence;
        } categories[4];  // MASTER, PID, CAMERA1, CAMERA2
        
        bool valid;
        bool allPagesFound;  // True when we've found all expected beacon pages
    };
    DiscoveryInfo discovery;
    
    // Category receiving arrays - ordered copies of beacon pages
    struct CategoryArray {
        std::vector<uint8_t> data;           // Contiguous array of all pages for this category
        std::vector<bool> pageValid;         // [index] -> true if page is valid (not torn)
        std::vector<uint32_t> pageVersions;  // [index] -> version number of page
        size_t pageCount;                    // Number of pages
        size_t validPages;                   // Number of valid (non-torn) pages
        bool initialized;                    // True if array has been allocated
        
        // Helper to get a page pointer
        void* getPage(size_t index) {
            if (index >= pageCount || !pageValid[index]) return nullptr;
            return &data[index * PAGE_SIZE];
        }
        
        // Helper to check if page is valid
        bool isPageValid(size_t index) const {
            return index < pageCount && pageValid[index];
        }
    };
    CategoryArray categoryArrays[4];
    
    // Mapping from memory file to receiving arrays
    struct CategoryMapping {
        std::vector<size_t> sourceOffsets;   // [index] -> offset in memory file (0 if missing)
        std::vector<bool> sourcePresent;     // [index] -> true if page exists in memory file
        size_t expectedCount;                 // How many pages we expect from discovery
        size_t foundCount;                    // How many pages found in memory file
        bool valid;                          // True if we found enough pages to be useful
    };
    CategoryMapping categoryMappings[4];
    
    // Constants from shared protocol
    static constexpr size_t PAGE_SIZE = BEACON_PAGE_SIZE;
    
    // Use the shared protocol structures
    using PIDListPage = BeaconPIDListPage;
    using BeaconPage = ::BeaconPage;
    using CameraControlPage = BeaconCameraControlPage;
    using DiscoveryPage = BeaconDiscoveryPage;
    
    // Helper functions
    bool IsPageValid(const void* page);
    bool ScanForDiscovery();
    void BuildCategoryMappings();
    void AllocateCategoryArrays();
    void CopyPagesToArrays();
    bool RefreshCategoryPages();  // Re-copy pages and check for tears
    bool ReadPIDGeneration(uint32_t generation, PIDGeneration& gen);
};

} // namespace Haywire