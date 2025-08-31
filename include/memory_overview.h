#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <chrono>
#include "imgui.h"
#include "mmap_reader.h"
#include "process_memory_map.h"
#include "address_space_flattener.h"

namespace Haywire {

class QemuConnection;
class GuestAgent;

enum class PageState : uint8_t {
    Unknown = 0,
    NotPresent,     // Unmapped/invalid
    Zero,           // All zeros
    Data,           // Has data
    Changing,       // Changes between scans
    Executable,     // Likely code
    VideoLike       // Looks like video data
};

struct MemoryRegion {
    uint64_t base;
    uint64_t size;
    std::string name;
    bool readable;
    bool writable;
    bool executable;
};

class MemoryOverview {
public:
    MemoryOverview();
    ~MemoryOverview();
    
    // Scan memory layout using QMP info
    void ScanMemoryLayout(QemuConnection& qemu);
    
    // Quick probe to check page states
    void UpdatePageStates(QemuConnection& qemu);
    
    // Update a specific region based on actual memory read
    void UpdateRegion(uint64_t address, size_t size, const uint8_t* data);
    
    // Draw the overview map
    void Draw();
    void DrawCompact();  // Compact view for side panel
    
    // Process memory map mode
    void SetProcessMode(bool enabled, int pid = -1);
    void LoadProcessMap(GuestAgent* agent);
    void SetFlattener(AddressSpaceFlattener* flattener) { this->flattener = flattener; }
    
    // Get address from pixel position
    uint64_t GetAddressAt(int x, int y) const;
    
    // Configuration
    void SetAddressRange(uint64_t start, uint64_t end);
    void SetGranularity(size_t pageSize, size_t chunkSize);
    
    // Navigation callback
    using NavigationCallback = std::function<void(uint64_t)>;
    void SetNavigationCallback(NavigationCallback cb) { navCallback = cb; }
    
private:
    // Memory layout from QEMU
    std::vector<MemoryRegion> regions;
    
    // Page state bitmap
    std::vector<PageState> pageStates;
    
    // Display configuration  
    uint64_t startAddress;
    uint64_t endAddress;
    size_t pageSize;        // 4KB typically
    size_t chunkSize;       // 64KB for coarse scanning
    
    // Display settings
    int pixelsPerRow;
    int bytesPerPixel;     // How many bytes each pixel represents
    
    // Scan state
    bool scanning;
    float scanProgress;
    std::chrono::steady_clock::time_point lastScan;
    
    // Texture for overview
    unsigned int textureID;
    std::vector<uint32_t> pixelBuffer;
    
    // Fast memory reading
    MMapReader mmapReader;
    
    // Process memory map mode
    bool processMode;
    int targetPid;
    ProcessMemoryMap processMap;
    AddressSpaceFlattener* flattener;
    NavigationCallback navCallback;
    
    // Convert page state to color
    uint32_t StateToColor(PageState state) const;
    
    // Probe a single page/chunk
    PageState ProbeMemory(QemuConnection& qemu, uint64_t address);
    
    // Draw process memory map
    void DrawProcessMap();
};

}