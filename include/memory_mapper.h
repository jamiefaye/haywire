/*
 * memory_mapper.h - QEMU memory region discovery and mapping
 * 
 * Queries QEMU to discover how guest physical addresses map to
 * the memory-backend-file offsets, handling multiple RAM regions.
 */

#ifndef MEMORY_MAPPER_H
#define MEMORY_MAPPER_H

#include <cstdint>
#include <vector>
#include <string>
#include <map>

namespace Haywire {

class MemoryMapper {
public:
    struct MemoryRegion {
        uint64_t gpa_start;     // Guest physical address start
        uint64_t gpa_end;       // Guest physical address end
        uint64_t file_offset;   // Offset in memory-backend-file
        uint64_t size;          // Size of region
        std::string name;       // Region name from QEMU
    };

    MemoryMapper();
    ~MemoryMapper();

    // Query QEMU and build the memory map
    bool DiscoverMemoryMap(const std::string& monitor_host = "localhost", 
                           int monitor_port = 4444);

    // Translate guest physical address to file offset
    // Returns -1 if address not found in any region
    int64_t TranslateGPAToFileOffset(uint64_t gpa) const;

    // Get all discovered memory regions
    const std::vector<MemoryRegion>& GetRegions() const { return regions_; }

    // Log all discovered regions
    void LogRegions() const;

private:
    std::vector<MemoryRegion> regions_;
    bool discovered_;

    // Parse QEMU monitor output
    bool ParseMtreeOutput(const std::string& output);
    
    // Send command to QEMU monitor and get response
    std::string QueryMonitor(const std::string& host, int port, const std::string& command);
};

} // namespace Haywire

#endif // MEMORY_MAPPER_H