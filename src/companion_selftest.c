#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define PAGE_SIZE 4096
#define BEACON_MAGIC 0x3142FACE
#define BEACON_COUNT 16  // Small for testing

// Minimal 16-byte header as discussed
struct BeaconPage {
    uint32_t magic;          // 0x3142FACE
    uint32_t session_id;     // Unique session
    uint32_t beacon_type;    // Type of data
    uint32_t type_index;     // Instance within type
    uint8_t data[4080];      // Payload
} __attribute__((packed));

// Beacon types for testing
enum BeaconType {
    BEACON_TYPE_SELFTEST = 1,
    BEACON_TYPE_PROCESS = 2,
    BEACON_TYPE_MAPPING = 3,
};

int main() {
    printf("=== Companion Self-Test Starting ===\n");
    
    // 1. Allocate beacon array (page-aligned)
    size_t total_size = BEACON_COUNT * PAGE_SIZE;
    void* raw = malloc(total_size + PAGE_SIZE);
    if (!raw) {
        perror("malloc");
        return 1;
    }
    
    // Align to page boundary
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    struct BeaconPage* beacons = (struct BeaconPage*)aligned;
    
    printf("Allocated %zu KB at %p (aligned to %p)\n", 
           total_size/1024, raw, beacons);
    
    // 2. Initialize beacons
    uint32_t session_id = getpid();  // Use PID as session for now
    
    for (int i = 0; i < BEACON_COUNT; i++) {
        beacons[i].magic = BEACON_MAGIC;
        beacons[i].session_id = session_id;
        beacons[i].beacon_type = BEACON_TYPE_SELFTEST;
        beacons[i].type_index = i;
        
        // Write test pattern
        sprintf((char*)beacons[i].data, "SELFTEST_BEACON_%d", i);
    }
    
    printf("Initialized %d beacons with session 0x%08X\n", 
           BEACON_COUNT, session_id);
    
    // 3. Find ourselves in memory map
#ifdef __linux__
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        perror("fopen maps");
        return 1;
    }
#else
    // macOS - just test that beacons are initialized
    printf("\nNote: Running on macOS (no /proc/self/maps)\n");
    printf("Beacons initialized at %p\n", beacons);
    FILE* maps = NULL;
#endif
    
    char line[256];
    int found = 0;
    uint64_t beacon_va = (uint64_t)beacons;
    
    printf("\nSearching for beacon VA 0x%lx in memory map...\n", beacon_va);
    
    while (fgets(line, sizeof(line), maps)) {
        uint64_t start, end;
        char perms[5];
        
        sscanf(line, "%lx-%lx %4s", &start, &end, perms);
        
        if (beacon_va >= start && beacon_va < end) {
            printf("FOUND! Beacon in range: %s", line);
            found = 1;
            
            // Store this info in beacon
            sprintf((char*)beacons[0].data, 
                    "VA_RANGE: 0x%lx-0x%lx PERMS:%s", 
                    start, end, perms);
        }
    }
    fclose(maps);
    
    if (!found) {
        printf("ERROR: Beacon not found in memory map!\n");
        return 1;
    }
    
    // 4. Get physical address via pagemap
    int pagemap = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap < 0) {
        // Might need root
        printf("WARNING: Cannot open pagemap (need root?)\n");
    } else {
        uint64_t page_num = beacon_va / PAGE_SIZE;
        uint64_t entry;
        
        lseek(pagemap, page_num * 8, SEEK_SET);
        if (read(pagemap, &entry, 8) == 8) {
            if (entry & (1ULL << 63)) {  // Present bit
                uint64_t pfn = entry & ((1ULL << 55) - 1);
                uint64_t pa = pfn * PAGE_SIZE;
                
                printf("VA->PA: 0x%lx -> 0x%lx (PFN: 0x%lx)\n", 
                       beacon_va, pa, pfn);
                
                // Store PA in beacon for Haywire to verify
                sprintf((char*)beacons[1].data, 
                        "PHYSICAL_ADDR: 0x%lx", pa);
            } else {
                printf("Page not present in pagemap\n");
            }
        }
        close(pagemap);
    }
    
    // 5. Write process info
    beacons[2].beacon_type = BEACON_TYPE_PROCESS;
    beacons[2].type_index = 0;
    sprintf((char*)beacons[2].data, 
            "PID:%d PPID:%d", getpid(), getppid());
    
    // 6. Keep alive for Haywire to find us
    printf("\n=== Self-Test Complete ===\n");
    printf("Beacons ready at %p\n", beacons);
    printf("Session ID: 0x%08X\n", session_id);
    printf("Keeping beacons alive for 30 seconds...\n");
    printf("Run beacon scanner now!\n");
    
    for (int i = 30; i > 0; i--) {
        printf("\r%d seconds remaining... ", i);
        fflush(stdout);
        sleep(1);
    }
    
    printf("\nCleaning up...\n");
    
    // Clear beacons before exit
    memset(beacons, 0, total_size);
    free(raw);
    
    return 0;
}