#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#define PAGE_SIZE 4096
#define BEACON_MAGIC 0x3142FACE
#define MAX_BEACONS 2048

// Discovery header - Page 0
typedef struct {
    uint32_t beacon_magic;  // 0x3142FACE at page boundary like all beacons
    uint32_t discovery_magic;  // 4-byte RIFF code "H a y D"
    uint32_t version;
    uint32_t pid;
    uint32_t beacon_count;
    uint32_t reserved[10];  // Future use
    uint8_t padding[4048];
} __attribute__((packed)) DiscoveryPage;

// Regular beacon page
typedef struct {
    uint32_t magic;
    uint32_t session_id;
    uint32_t beacon_type;
    uint32_t type_index;
    uint8_t data[4080];
} __attribute__((packed)) BeaconPage;

// Global state
static DiscoveryPage* discovery = NULL;
static BeaconPage* beacon_array = NULL;
static uint32_t session_id = 0;
static volatile int running = 1;
static uint32_t next_beacon = 0;

uint32_t allocate_beacon(uint32_t type) {
    if (next_beacon >= MAX_BEACONS - 1) {
        return 0xFFFFFFFF;
    }
    
    uint32_t index = next_beacon++;
    beacon_array[index].magic = BEACON_MAGIC;
    beacon_array[index].session_id = session_id;
    beacon_array[index].beacon_type = type;
    beacon_array[index].type_index = index;
    
    return index;
}

void sighandler(int sig) {
    printf("\nShutting down...\n");
    running = 0;
}

int main() {
    printf("=== Haywire Companion with RIFF codes ===\n");
    
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    
    session_id = getpid();
    
    // Allocate beacon array
    size_t total_size = MAX_BEACONS * PAGE_SIZE;
    void* raw = malloc(total_size + PAGE_SIZE);
    if (!raw) {
        perror("malloc");
        return 1;
    }
    
    // Align to page boundary
    uintptr_t addr = (uintptr_t)raw;
    void* aligned = (void*)((addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    
    // First page is discovery, rest are beacons
    discovery = (DiscoveryPage*)aligned;
    beacon_array = (BeaconPage*)((uint8_t*)aligned + PAGE_SIZE);
    
    printf("Allocated %zu MB at %p\n", total_size/(1024*1024), aligned);
    memset(aligned, 0, total_size);
    
    // Initialize discovery page - beacon magic at boundary, then "H a y D"
    discovery->beacon_magic = BEACON_MAGIC;  // Same as all beacon pages
    uint8_t disc_bytes[4] = {0x48, 0x61, 0x79, 0x44};  // 'H', 'a', 'y', 'D'
    memcpy(&discovery->discovery_magic, disc_bytes, 4);
    discovery->version = 1;
    discovery->pid = session_id;
    
    printf("Discovery page initialized with beacon magic at boundary\n");
    
    // Initialize control beacon
    uint32_t control_idx = allocate_beacon(1);
    BeaconPage* control = &beacon_array[control_idx];
    
    // Add control marker in data area - "H a y C" 
    uint8_t ctrl_bytes[4] = {0x48, 0x61, 0x79, 0x43};  // 'H', 'a', 'y', 'C'
    memcpy(control->data, ctrl_bytes, 4);
    
    printf("Control beacon at index %u\n", control_idx);
    
    // Touch all pages to make resident - both discovery and beacons
    // Touch discovery page
    ((volatile char*)discovery)[0] = 0;
    
    // Touch only the first 10 beacon pages and write beacon magic to each
    for (int i = 0; i < 10; i++) {
        beacon_array[i].magic = BEACON_MAGIC;
        beacon_array[i].session_id = session_id;
        beacon_array[i].beacon_type = i + 1;
        beacon_array[i].type_index = i;
        // Force write
        ((volatile char*)&beacon_array[i])[0] = ((char*)&beacon_array[i])[0];
    }
    printf("Initialized 10 beacon pages with magic\n");
    
    // Simple main loop
    uint32_t cycle = 0;
    while (running && cycle < 20) {
        discovery->beacon_count = next_beacon;
        
        // Keep writing both magics
        discovery->beacon_magic = BEACON_MAGIC;
        uint8_t disc_bytes2[4] = {0x48, 0x61, 0x79, 0x44};  // 'H', 'a', 'y', 'D'
        memcpy(&discovery->discovery_magic, disc_bytes2, 4);
        
        printf("Cycle %u: %u beacons, discovery at %p\n", 
               cycle, discovery->beacon_count, discovery);
        
        cycle++;
        sleep(5);
    }
    
    // Cleanup
    printf("Cleaning up...\n");
    memset(aligned, 0, total_size);
    free(raw);
    
    return 0;
}