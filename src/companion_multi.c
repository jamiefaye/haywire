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

// Beacon categories
#define CATEGORY_MASTER      0
#define CATEGORY_ROUNDROBIN  1  
#define CATEGORY_PID         2
#define CATEGORY_CAMERA1     3
#define CATEGORY_CAMERA2     4
#define NUM_CATEGORIES       5

// Pages per category
#define MASTER_PAGES      10    // Discovery and control pages
#define ROUNDROBIN_PAGES  500   // Round-robin process scanning
#define PID_PAGES         100   // PID list snapshots
#define CAMERA1_PAGES     200   // Camera watching specific PID
#define CAMERA2_PAGES     200   // Camera watching another PID

// Discovery header - First page of MASTER category
typedef struct {
    uint32_t beacon_magic;      // 0x3142FACE at page boundary like all beacons
    uint32_t discovery_magic;   // 4-byte RIFF code "H a y D"
    uint32_t version;
    uint32_t pid;
    
    // Category information
    struct {
        uint32_t base_offset;    // Offset from discovery page to this category
        uint32_t page_count;     // Number of pages in this category
        uint32_t write_index;    // Current write position
        uint32_t sequence;       // Sequence number for tear detection
    } categories[NUM_CATEGORIES];
    
    uint8_t padding[4016];      // Pad to PAGE_SIZE
} __attribute__((packed)) DiscoveryPage;

// Regular beacon page with tear detection
typedef struct {
    // Header with version for tear detection
    uint32_t magic;          // BEACON_MAGIC
    uint32_t version_top;    // Version number at top (for tear detection)
    uint32_t session_id;     
    uint32_t category;       // Which category this belongs to
    uint32_t category_index; // Index within the category
    uint32_t sequence;       // Sequence number
    uint32_t data_size;      // Valid data size in this page
    uint32_t reserved;       // Padding/alignment
    
    // Data area
    uint8_t data[4064];      // Actual data (PAGE_SIZE - 32 - 4)
    
    // Footer with version for tear detection
    uint32_t version_bottom; // Must match version_top for valid page
} __attribute__((packed)) BeaconPage;

// Category arrays - each is a contiguous block of beacon pages
typedef struct {
    BeaconPage* pages;       // Pointer to page array
    uint32_t page_count;     // Number of pages
    uint32_t write_index;    // Current write position
    uint32_t sequence;       // Global sequence counter
} CategoryArray;

// Global state
static DiscoveryPage* discovery = NULL;
static CategoryArray categories[NUM_CATEGORIES];
static uint32_t session_id = 0;
static volatile int running = 1;

// Write data to a category, handling wraparound
BeaconPage* write_to_category(uint32_t category_id, void* data, size_t size) {
    if (category_id >= NUM_CATEGORIES) return NULL;
    
    CategoryArray* cat = &categories[category_id];
    if (!cat->pages) return NULL;
    
    // Get current page
    uint32_t idx = cat->write_index % cat->page_count;
    BeaconPage* page = &cat->pages[idx];
    
    // Initialize beacon header
    page->magic = BEACON_MAGIC;
    page->session_id = session_id;
    page->category = category_id;
    page->category_index = idx;
    page->sequence = cat->sequence++;
    page->data_size = (size > sizeof(page->data)) ? sizeof(page->data) : size;
    
    // Copy data if provided
    if (data && size > 0) {
        memcpy(page->data, data, page->data_size);
    }
    
    // Advance write index
    cat->write_index++;
    
    return page;
}

void sighandler(int sig) {
    printf("\nShutting down...\n");
    running = 0;
}

int main() {
    printf("=== Haywire Companion with Multiple Categories ===\n");
    
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    
    session_id = getpid();
    
    // Calculate total pages needed
    uint32_t page_counts[NUM_CATEGORIES] = {
        MASTER_PAGES, ROUNDROBIN_PAGES, PID_PAGES, CAMERA1_PAGES, CAMERA2_PAGES
    };
    
    uint32_t total_pages = 0;
    for (int i = 0; i < NUM_CATEGORIES; i++) {
        total_pages += page_counts[i];
    }
    
    // Allocate all memory as one block
    size_t total_size = total_pages * PAGE_SIZE;
    void* raw = malloc(total_size + PAGE_SIZE);
    if (!raw) {
        perror("malloc");
        return 1;
    }
    
    // Align to page boundary
    uintptr_t addr = (uintptr_t)raw;
    void* aligned = (void*)((addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    
    printf("Allocated %u pages (%zu MB) at %p\n", 
           total_pages, total_size/(1024*1024), aligned);
    memset(aligned, 0, total_size);
    
    // Set up category arrays
    uint8_t* current = (uint8_t*)aligned;
    for (int i = 0; i < NUM_CATEGORIES; i++) {
        categories[i].pages = (BeaconPage*)current;
        categories[i].page_count = page_counts[i];
        categories[i].write_index = 0;
        categories[i].sequence = 0;
        current += page_counts[i] * PAGE_SIZE;
    }
    
    // First page of MASTER category is discovery
    discovery = (DiscoveryPage*)categories[CATEGORY_MASTER].pages;
    
    // Initialize discovery page - beacon magic at boundary, then "H a y D"
    discovery->beacon_magic = BEACON_MAGIC;  // Same as all beacon pages
    uint8_t disc_bytes[4] = {0x48, 0x61, 0x79, 0x44};  // 'H', 'a', 'y', 'D'
    memcpy(&discovery->discovery_magic, disc_bytes, 4);
    discovery->version = 1;
    discovery->pid = session_id;
    
    // Fill in category information in discovery
    uint32_t offset = 0;
    for (int i = 0; i < NUM_CATEGORIES; i++) {
        discovery->categories[i].base_offset = offset;
        discovery->categories[i].page_count = categories[i].page_count;
        discovery->categories[i].write_index = 0;
        discovery->categories[i].sequence = 0;
        offset += categories[i].page_count * PAGE_SIZE;
    }
    
    printf("Discovery page initialized with %d categories\n", NUM_CATEGORIES);
    
    // Initialize first few pages in each category
    for (int cat = 0; cat < NUM_CATEGORIES; cat++) {
        for (int page = 0; page < 5 && page < categories[cat].page_count; page++) {
            BeaconPage* bp = &categories[cat].pages[page];
            bp->magic = BEACON_MAGIC;
            bp->session_id = session_id;
            bp->category = cat;
            bp->category_index = page;
            bp->sequence = categories[cat].sequence++;
            bp->data_size = 0;
            
            // Force memory to be resident
            ((volatile char*)bp)[0] = ((char*)bp)[0];
        }
    }
    
    printf("Initialized first 5 pages in each category\n");
    
    // Simple main loop - demonstrate writing to different categories
    uint32_t cycle = 0;
    while (running && cycle < 20) {
        // Update discovery page
        discovery->beacon_magic = BEACON_MAGIC;
        uint8_t disc_bytes2[4] = {0x48, 0x61, 0x79, 0x44};  // 'H', 'a', 'y', 'D'
        memcpy(&discovery->discovery_magic, disc_bytes2, 4);
        
        // Update category info in discovery
        for (int i = 0; i < NUM_CATEGORIES; i++) {
            discovery->categories[i].write_index = categories[i].write_index;
            discovery->categories[i].sequence = categories[i].sequence;
        }
        
        // Simulate writing to different categories
        char test_data[100];
        
        // Write to ROUNDROBIN (simulating process scan)
        snprintf(test_data, sizeof(test_data), "RoundRobin cycle %u", cycle);
        write_to_category(CATEGORY_ROUNDROBIN, test_data, strlen(test_data));
        
        // Write to PID list every 3 cycles
        if (cycle % 3 == 0) {
            snprintf(test_data, sizeof(test_data), "PID snapshot %u", cycle);
            write_to_category(CATEGORY_PID, test_data, strlen(test_data));
        }
        
        // Camera 1 continuous updates
        snprintf(test_data, sizeof(test_data), "Camera1 frame %u", cycle);
        write_to_category(CATEGORY_CAMERA1, test_data, strlen(test_data));
        
        printf("Cycle %u: RR[%u] PID[%u] CAM1[%u]\n", 
               cycle, 
               categories[CATEGORY_ROUNDROBIN].write_index,
               categories[CATEGORY_PID].write_index,
               categories[CATEGORY_CAMERA1].write_index);
        
        cycle++;
        sleep(2);
    }
    
    // Cleanup
    printf("Cleaning up...\n");
    memset(aligned, 0, total_size);
    free(raw);
    
    return 0;
}