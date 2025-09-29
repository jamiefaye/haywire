#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "beacon_protocol.h"

int main() {
    // Open memory file
    int fd = open("/tmp/haywire-vm-mem", O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open memory file\n";
        return 1;
    }
    
    struct stat st;
    fstat(fd, &st);
    size_t memSize = st.st_size;
    
    void* memBase = mmap(nullptr, memSize, PROT_READ, MAP_SHARED, fd, 0);
    if (memBase == MAP_FAILED) {
        std::cerr << "Failed to map memory\n";
        close(fd);
        return 1;
    }
    
    uint8_t* mem = static_cast<uint8_t*>(memBase);
    constexpr size_t PAGE_SIZE = 4096;
    
    // First find discovery page
    uint32_t session_id = 0;
    uint32_t timestamp = 0;
    bool found_discovery = false;
    
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        BeaconDiscoveryPage* page = reinterpret_cast<BeaconDiscoveryPage*>(mem + offset);
        
        if (page->magic == BEACON_MAGIC &&
            page->category == BEACON_CATEGORY_MASTER &&
            page->category_index == 0) {
            
            session_id = page->session_id;
            timestamp = page->timestamp;
            found_discovery = true;
            
            std::cout << "Found discovery page at 0x" << std::hex << offset << std::dec
                      << " (session=" << session_id << ", timestamp=" << timestamp << ")\n";
            
            std::cout << "Expected pages:\n";
            for (int i = 0; i < BEACON_NUM_CATEGORIES; i++) {
                const char* names[] = {"Master", "PID", "Camera1", "Camera2"};
                std::cout << "  " << names[i] << ": " << page->categories[i].page_count << " pages\n";
            }
            break;
        }
    }
    
    if (!found_discovery) {
        std::cout << "Discovery page not found\n";
        munmap(memBase, memSize);
        close(fd);
        return 1;
    }
    
    // Now count all beacon pages with matching session_id
    int beacon_count = 0;
    int category_counts[BEACON_NUM_CATEGORIES] = {0};
    
    for (size_t offset = 0; offset + PAGE_SIZE <= memSize; offset += PAGE_SIZE) {
        uint32_t* magic = reinterpret_cast<uint32_t*>(mem + offset);
        if (*magic == BEACON_MAGIC) {
            BeaconPage* page = reinterpret_cast<BeaconPage*>(mem + offset);
            
            if (page->session_id == session_id) {
                beacon_count++;
                
                if (page->category < BEACON_NUM_CATEGORIES) {
                    category_counts[page->category]++;
                }
            }
        }
    }
    
    std::cout << "\nFound " << beacon_count << " beacon pages for session " << session_id << ":\n";
    const char* names[] = {"Master", "PID", "Camera1", "Camera2"};
    for (int i = 0; i < BEACON_NUM_CATEGORIES; i++) {
        std::cout << "  " << names[i] << ": " << category_counts[i] << " pages\n";
    }
    
    munmap(memBase, memSize);
    close(fd);
    return 0;
}