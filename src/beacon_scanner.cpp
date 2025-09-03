#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include "beacon_map.h"
#include "beacon_types.h"

class BeaconScanner {
private:
    int fd;
    void* mapped_mem;
    size_t file_size;
    BeaconMap map;
    
public:
    BeaconScanner() : fd(-1), mapped_mem(nullptr), file_size(0) {}
    
    ~BeaconScanner() {
        if (mapped_mem) {
            munmap(mapped_mem, file_size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    
    bool open_memory_file(const char* path) {
        fd = open(path, O_RDWR);
        if (fd < 0) {
            perror("open memory file");
            return false;
        }
        
        struct stat st;
        if (fstat(fd, &st) < 0) {
            perror("fstat");
            return false;
        }
        
        file_size = st.st_size;
        mapped_mem = mmap(nullptr, file_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        if (mapped_mem == MAP_FAILED) {
            perror("mmap");
            return false;
        }
        
        printf("Mapped %zu MB of memory\n", file_size / 1024 / 1024);
        return true;
    }
    
    void scan_for_beacons() {
        printf("Scanning for beacons on page boundaries...\n");
        
        uint8_t* mem = (uint8_t*)mapped_mem;
        size_t pages_scanned = 0;
        size_t beacons_found = 0;
        size_t suspicious_beacons = 0;
        size_t valid_beacons = 0;
        
        // Scan only on page boundaries
        for (size_t offset = 0; offset < file_size; offset += PAGE_SIZE) {
            pages_scanned++;
            
            if (pages_scanned % 100000 == 0) {
                printf("  Scanned %zu pages, found %zu beacons...\r", 
                       pages_scanned, beacons_found);
                fflush(stdout);
            }
            
            // Check for beacon magic
            uint32_t* page = (uint32_t*)(mem + offset);
            if (page[0] == BEACON_MAGIC1 && page[1] == BEACON_MAGIC2) {
                // Found a beacon!
                uint32_t session_id = page[2];
                uint32_t beacon_class = page[3];  // For v4+, this is beacon_class
                uint32_t page_index = page[4];
                uint32_t total_pages = page[5];
                uint32_t protocol_ver = page[6];  // Protocol version is at offset 24
                
                // Validate beacon - v4 only
                bool is_valid = true;
                const char* issue = nullptr;
                
                if (session_id == 0 || session_id == 0xFFFFFFFF) {
                    is_valid = false;
                    issue = "invalid session_id";
                } else if (protocol_ver != 4) {
                    is_valid = false;
                    issue = "not protocol v4";
                } else if (beacon_class < 1 || beacon_class > 10) {
                    is_valid = false;
                    issue = "invalid beacon class";
                } else if (page_index >= total_pages || total_pages > 10000) {
                    is_valid = false;
                    issue = "invalid page index/total";
                }
                
                if (!is_valid) {
                    suspicious_beacons++;
                    printf("\nSuspicious beacon at 0x%08zX: %s (session=0x%08X, proto=%u)\n",
                           offset, issue, session_id, protocol_ver);
                } else {
                    valid_beacons++;
                }
                
                // Add beacon to map (use actual page_index from structure)
                map.add_beacon(offset, session_id, protocol_ver, page_index);
                beacons_found++;
            }
        }
        
        printf("\nScan complete: %zu pages, %zu beacons found (%zu valid, %zu suspicious)\n", 
               pages_scanned, beacons_found, valid_beacons, suspicious_beacons);
    }
    
    void print_summary() {
        printf("\n=== Beacon Map Summary ===\n");
        printf("Total beacons: %zu\n", map.total_beacons());
        printf("Active beacons: %zu\n", map.active_beacons());
        
        // Group by session
        std::unordered_map<uint32_t, size_t> session_counts;
        std::unordered_map<uint32_t, std::unordered_map<uint32_t, size_t>> class_counts;
        
        for (size_t i = 0; i < map.total_beacons(); i++) {
            auto* beacon = map.get_by_index(i);
            if (beacon && beacon->is_active) {
                session_counts[beacon->session_id]++;
                
                // Check if this beacon has class info (protocol v4)
                if (beacon->protocol_ver == 4) {
                    uint8_t* mem = (uint8_t*)mapped_mem;
                    uint32_t* page = (uint32_t*)(mem + beacon->phys_addr);
                    uint32_t beacon_class = page[3];  // beacon_class at offset 12
                    class_counts[beacon->session_id][beacon_class]++;
                }
            }
        }
        
        printf("\nSessions found:\n");
        for (const auto& pair : session_counts) {
            printf("  Session 0x%08X: %zu beacons\n", pair.first, pair.second);
            
            // Show class breakdown if available
            auto class_it = class_counts.find(pair.first);
            if (class_it != class_counts.end() && !class_it->second.empty()) {
                printf("    By class:\n");
                for (const auto& cls : class_it->second) {
                    const char* class_name = "Unknown";
                    switch (cls.first) {
                        case BEACON_CLASS_INDEX: class_name = "INDEX"; break;
                        case BEACON_CLASS_REQUEST_RING: class_name = "REQUEST_RING"; break;
                        case BEACON_CLASS_RESPONSE_RING: class_name = "RESPONSE_RING"; break;
                        case BEACON_CLASS_REQUEST_DATA: class_name = "REQUEST_DATA"; break;
                        case BEACON_CLASS_RESPONSE_DATA: class_name = "RESPONSE_DATA"; break;
                        case BEACON_CLASS_BULK_DATA: class_name = "BULK_DATA"; break;
                        case BEACON_CLASS_STATISTICS: class_name = "STATISTICS"; break;
                    }
                    printf("      %s: %zu pages\n", class_name, cls.second);
                }
            }
            
            // Find regions for this session
            auto regions = map.find_regions(pair.first);
            for (const auto& region : regions) {
                printf("    Region at 0x%08llX: %zu contiguous pages (protocol v%u)\n",
                       region.base_addr, region.page_count, region.protocol_ver);
                
                // Show where request/response areas are
                if (region.page_count >= 9) {
                    printf("      Requests:  0x%08llX\n", region.base_addr + PAGE_SIZE);
                    printf("      Responses: 0x%08llX\n", region.base_addr + 5 * PAGE_SIZE);
                    printf("      Data area: 0x%08llX\n", region.base_addr + 9 * PAGE_SIZE);
                }
            }
        }
    }
    
    // Test lookup performance
    void test_lookups() {
        printf("\n=== Testing Lookups ===\n");
        
        // Test address lookup
        if (map.total_beacons() > 0) {
            auto* first = map.get_by_index(0);
            if (first) {
                auto start = std::chrono::high_resolution_clock::now();
                for (int i = 0; i < 100000; i++) {
                    map.find_by_addr(first->phys_addr);
                }
                auto end = std::chrono::high_resolution_clock::now();
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
                printf("Address lookup: %.2f ns per lookup\n", dur.count() * 10.0);
            }
        }
    }
    
    BeaconMap& get_map() { return map; }
    
    // Get pointer to specific offset in memory
    void* get_memory_at(uint64_t offset) {
        if (offset < file_size) {
            return (uint8_t*)mapped_mem + offset;
        }
        return nullptr;
    }
};

int main() {
    BeaconScanner scanner;
    
    if (!scanner.open_memory_file("/tmp/haywire-vm-mem")) {
        fprintf(stderr, "Failed to open memory file\n");
        return 1;
    }
    
    scanner.scan_for_beacons();
    scanner.print_summary();
    scanner.test_lookups();
    
    // Now we have bidirectional mappings ready for use!
    printf("\nBeacon map ready for request/response protocol\n");
    
    return 0;
}