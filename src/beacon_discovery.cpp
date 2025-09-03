#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define MEMORY_FILE "/tmp/haywire-vm-mem"
#define PAGE_SIZE 4096
#define MAGIC1 0x3142FACE
#define MAGIC2 0xCAFEBABE
#define MAGIC3 0xFEEDFACE
#define MAGIC4 0xBAADF00D

struct PageBeacon {
    uint32_t magic1;
    uint32_t magic2;
    uint32_t session_id;
    uint32_t page_index;
    uint64_t timestamp;
    uint32_t process_count;
    uint32_t update_counter;
    uint32_t magic3;
    uint32_t magic4;
    uint8_t padding1[20];
    char hostname[64];
    uint32_t kernel_version[4];
    uint32_t cpu_count;
    uint32_t mem_total_mb;
    uint8_t padding2[168];
    uint8_t reserved[3776];
} __attribute__((packed));

static_assert(sizeof(PageBeacon) == PAGE_SIZE, "PageBeacon must be exactly 4096 bytes");

class BeaconDiscovery {
private:
    int fd;
    void* mapped_mem;
    size_t mapped_size;
    uint64_t beacon_offset;
    uint32_t session_id;
    int num_pages;
    
public:
    BeaconDiscovery() : fd(-1), mapped_mem(nullptr), mapped_size(0), 
                        beacon_offset(0), session_id(0), num_pages(0) {}
    
    ~BeaconDiscovery() {
        if (mapped_mem) {
            munmap(mapped_mem, mapped_size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    
    bool findBeacons() {
        // Open the memory file
        fd = open(MEMORY_FILE, O_RDONLY);
        if (fd < 0) {
            perror("open memory file");
            return false;
        }
        
        // Get file size
        struct stat st;
        if (fstat(fd, &st) < 0) {
            perror("fstat");
            return false;
        }
        
        size_t file_size = st.st_size;
        size_t total_pages = file_size / PAGE_SIZE;
        
        printf("Scanning %zu pages for beacon pattern 0x%08X...\n", total_pages, MAGIC1);
        
        // Map the entire file
        mapped_mem = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped_mem == MAP_FAILED) {
            perror("mmap");
            return false;
        }
        mapped_size = file_size;
        
        // Scan on page boundaries only
        uint8_t* mem = (uint8_t*)mapped_mem;
        for (size_t page = 0; page < total_pages; page++) {
            uint64_t offset = page * PAGE_SIZE;
            PageBeacon* beacon = (PageBeacon*)(mem + offset);
            
            // Check magic numbers
            if (beacon->magic1 == MAGIC1 && beacon->magic2 == MAGIC2 &&
                beacon->magic3 == MAGIC3 && beacon->magic4 == MAGIC4) {
                
                // Found first beacon!
                beacon_offset = offset;
                session_id = beacon->session_id;
                
                printf("Found beacon session 0x%08X at offset 0x%lX\n", 
                       session_id, offset);
                printf("  Hostname: %s\n", beacon->hostname);
                printf("  Page index: %u\n", beacon->page_index);
                printf("  Process count: %u\n", beacon->process_count);
                
                // Count contiguous pages
                num_pages = countContiguousPages(page, total_pages);
                printf("  Found %d contiguous beacon pages (%.1f MB)\n", 
                       num_pages, num_pages * PAGE_SIZE / 1024.0 / 1024.0);
                
                return true;
            }
        }
        
        printf("No beacons found in %zu pages\n", total_pages);
        return false;
    }
    
    int countContiguousPages(size_t start_page, size_t total_pages) {
        uint8_t* mem = (uint8_t*)mapped_mem;
        int count = 0;
        
        for (size_t page = start_page; page < total_pages; page++) {
            PageBeacon* beacon = (PageBeacon*)(mem + page * PAGE_SIZE);
            
            if (beacon->magic1 != MAGIC1 || beacon->magic2 != MAGIC2 ||
                beacon->session_id != session_id || beacon->page_index != count) {
                break;
            }
            count++;
        }
        
        return count;
    }
    
    PageBeacon* getBeaconPage(int page_index) {
        if (!mapped_mem || page_index >= num_pages) {
            return nullptr;
        }
        
        uint8_t* mem = (uint8_t*)mapped_mem;
        return (PageBeacon*)(mem + beacon_offset + page_index * PAGE_SIZE);
    }
    
    uint64_t getBeaconOffset() const { return beacon_offset; }
    uint32_t getSessionId() const { return session_id; }
    int getNumPages() const { return num_pages; }
};

// Test program
int main() {
    BeaconDiscovery discovery;
    
    if (!discovery.findBeacons()) {
        fprintf(stderr, "Failed to find beacons\n");
        return 1;
    }
    
    printf("\nMonitoring beacon updates...\n");
    
    uint32_t last_update = 0;
    while (true) {
        PageBeacon* beacon = discovery.getBeaconPage(0);
        if (beacon && beacon->update_counter != last_update) {
            last_update = beacon->update_counter;
            printf("Update #%u: %u processes | Session 0x%08X\n",
                   beacon->update_counter, beacon->process_count, beacon->session_id);
        }
        
        usleep(100000); // 100ms
    }
    
    return 0;
}