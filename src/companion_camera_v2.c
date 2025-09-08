#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "beacon_encoder.h"
#include "beacon_protocol.h"

#define CAMERA_ID 1  // Camera 1 or 2
#define SCAN_INTERVAL_MS 100  // 10Hz scanning

// Control page structure for h2g communication (must match beacon_protocol.h)
typedef struct {
    uint32_t magic;
    uint32_t version_top;
    uint32_t target_pid;
    uint32_t version_bottom;
    uint32_t current_pid;
} CameraControlPage;

// Four separate memory allocations (no round-robin)
static void* master_page = NULL;           // 1 page for discovery/master
static void* pids_ptr = NULL;              // 16 pages for PID lists  
static void* camera1_ptr = NULL;           // 1 control + 199 data pages
static void* camera2_ptr = NULL;           // 1 control + 199 data pages

// Beacon encoders for each category
static BeaconEncoder pid_encoder;
static BeaconEncoder camera_encoder;

// Current target PID for this camera
static uint32_t target_pid = 0;

// Circular buffer tracking for camera data pages
static uint32_t camera_write_index = 0;  // Which data page to write next (0-198)

// Signal handler for clean shutdown
static volatile int keep_running = 1;
void sig_handler(int sig) {
    keep_running = 0;
}

// Initialize the 5 separate beacon memory areas
int init_memory() {
    // 1. Master page - single page for discovery
    if (posix_memalign(&master_page, 4096, BEACON_MASTER_PAGES * 4096) != 0) {
        perror("Failed to allocate master page");
        return -1;
    }
    memset(master_page, 0, BEACON_MASTER_PAGES * 4096);
    
    // 2. PID pages - 16 pages for PID lists (note: using 16 instead of 32 for now)
    if (posix_memalign(&pids_ptr, 4096, 16 * 4096) != 0) {
        perror("Failed to allocate PID pages");
        return -1;
    }
    memset(pids_ptr, 0, 16 * 4096);
    
    // No round-robin pages - obsolete
    
    // 4. Camera1 pages - 1 control + 199 data pages
    if (posix_memalign(&camera1_ptr, 4096, BEACON_CAMERA1_PAGES * 4096) != 0) {
        perror("Failed to allocate camera1 pages");
        return -1;
    }
    memset(camera1_ptr, 0, BEACON_CAMERA1_PAGES * 4096);
    
    // 5. Camera2 pages - 1 control + 199 data pages  
    if (posix_memalign(&camera2_ptr, 4096, BEACON_CAMERA2_PAGES * 4096) != 0) {
        perror("Failed to allocate camera2 pages");
        return -1;
    }
    memset(camera2_ptr, 0, BEACON_CAMERA2_PAGES * 4096);
    
    // Initialize master/discovery page
    BeaconDiscoveryPage* discovery = (BeaconDiscoveryPage*)master_page;
    discovery->magic = BEACON_MAGIC;
    discovery->version_top = 1;
    discovery->version_bottom = 1;
    discovery->session_id = getpid();
    discovery->category = BEACON_CATEGORY_MASTER;
    discovery->category_index = 0;
    discovery->timestamp = time(NULL);
    
    // Fill in category information (offsets will be determined by Haywire's scan)
    // We just report how many pages each category has
    discovery->categories[BEACON_CATEGORY_MASTER].page_count = BEACON_MASTER_PAGES;
    discovery->categories[BEACON_CATEGORY_PID].page_count = 16;  // Using 16 for now
    discovery->categories[BEACON_CATEGORY_CAMERA1].page_count = BEACON_CAMERA1_PAGES;
    discovery->categories[BEACON_CATEGORY_CAMERA2].page_count = BEACON_CAMERA2_PAGES;
    
    // Initialize all PID beacon pages
    for (int i = 0; i < 16; i++) {
        BeaconPIDListPage* pid_page = (BeaconPIDListPage*)((uint8_t*)pids_ptr + i * 4096);
        pid_page->magic = BEACON_MAGIC;
        pid_page->version_top = 1;
        pid_page->version_bottom = 1;
        pid_page->session_id = getpid();
        pid_page->category = BEACON_CATEGORY_PID;
        pid_page->category_index = i;
        pid_page->timestamp = discovery->timestamp;
        pid_page->generation = 0;
        pid_page->total_pids = 0;
        pid_page->pids_in_page = 0;
    }
    
    // Initialize BOTH camera beacon pages (even though we only use camera1 for data)
    // This ensures Haywire can find all expected beacon pages
    
    // Initialize Camera1 pages
    BeaconCameraControlPage* control1 = (BeaconCameraControlPage*)camera1_ptr;
    control1->magic = BEACON_MAGIC;
    control1->version_top = 1;
    control1->version_bottom = 1;
    control1->session_id = getpid();
    control1->category = BEACON_CATEGORY_CAMERA1;
    control1->category_index = 0;  // Control page is always index 0
    control1->timestamp = discovery->timestamp;
    control1->target_pid = 1;  // Default to init
    control1->status = BEACON_CAMERA_STATUS_IDLE;
    control1->current_pid = 0;
    
    // Initialize camera1 data pages (pages 1-199)
    for (int i = 1; i < BEACON_CAMERA1_PAGES; i++) {
        BeaconPage* data_page = (BeaconPage*)((uint8_t*)camera1_ptr + i * 4096);
        data_page->magic = BEACON_MAGIC;
        data_page->version_top = 1;
        data_page->version_bottom = 1;
        data_page->session_id = getpid();
        data_page->category = BEACON_CATEGORY_CAMERA1;
        data_page->category_index = i;  // Data pages are indices 1-199
        data_page->timestamp = discovery->timestamp;
        data_page->sequence = 0;
        data_page->data_size = 0;
    }
    
    // Initialize Camera2 pages (even though we only actively use camera1)
    BeaconCameraControlPage* control2 = (BeaconCameraControlPage*)camera2_ptr;
    control2->magic = BEACON_MAGIC;
    control2->version_top = 1;
    control2->version_bottom = 1;
    control2->session_id = getpid();
    control2->category = BEACON_CATEGORY_CAMERA2;
    control2->category_index = 0;
    control2->timestamp = discovery->timestamp;
    control2->target_pid = 0;  // Not active
    control2->status = BEACON_CAMERA_STATUS_IDLE;
    control2->current_pid = 0;
    
    // Initialize camera2 data pages
    for (int i = 1; i < BEACON_CAMERA2_PAGES; i++) {
        BeaconPage* data_page = (BeaconPage*)((uint8_t*)camera2_ptr + i * 4096);
        data_page->magic = BEACON_MAGIC;
        data_page->version_top = 1;
        data_page->version_bottom = 1;
        data_page->session_id = getpid();
        data_page->category = BEACON_CATEGORY_CAMERA2;
        data_page->category_index = i;
        data_page->timestamp = discovery->timestamp;
        data_page->sequence = 0;
        data_page->data_size = 0;
    }
    
    printf("Camera %d: Initialized 4 beacon memory areas (total %d pages):\n", 
           CAMERA_ID, 1 + 16 + BEACON_CAMERA1_PAGES + BEACON_CAMERA2_PAGES);
    printf("  - Master page: 1 page\n");
    printf("  - PID pages: 16 pages\n");
    printf("  - Camera1: %d pages (1 control + %d data)\n", 
           BEACON_CAMERA1_PAGES, BEACON_CAMERA1_PAGES - 1);
    printf("  - Camera2: %d pages (1 control + %d data)\n", 
           BEACON_CAMERA2_PAGES, BEACON_CAMERA2_PAGES - 1);
    
    return 0;
}

// Read process memory maps and encode as sections
void scan_process_memory(uint32_t pid) {
    char maps_path[256];
    snprintf(maps_path, sizeof(maps_path), "/proc/%u/maps", pid);
    
    FILE* fp = fopen(maps_path, "r");
    if (!fp) {
        // Process might have exited
        return;
    }
    
    // First add a camera header to identify this scan
    beacon_encoder_add_camera_header(&camera_encoder, CAMERA_ID, pid, time(NULL));
    
    char line[512];
    int section_count = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        uint64_t start, end;
        char perms[5];
        uint64_t offset;
        char dev[16];
        uint64_t inode;
        char path[256] = "";
        
        int n = sscanf(line, "%lx-%lx %4s %lx %s %lu %255[^\n]",
                       &start, &end, perms, &offset, dev, &inode, path);
        
        if (n >= 6) {
            // Calculate flags from permissions
            uint32_t flags = 0;
            if (perms[0] == 'r') flags |= 0x1;  // PROT_READ
            if (perms[1] == 'w') flags |= 0x2;  // PROT_WRITE
            if (perms[2] == 'x') flags |= 0x4;  // PROT_EXEC
            if (perms[3] == 'p') flags |= 0x8;  // MAP_PRIVATE
            else flags |= 0x10;  // MAP_SHARED
            
            // Add section entry
            beacon_encoder_add_section(&camera_encoder, pid, start, end - start, flags, path);
            section_count++;
            
            // Also scan for page table entries if this is a data section
            if ((flags & 0x2) && !(flags & 0x4)) {  // Writable, not executable
                // For now, just add a few sample PTEs
                // In real implementation, would read /proc/pid/pagemap
                for (int i = 0; i < 5 && (start + i * 0x1000) < end; i++) {
                    uint64_t va = start + i * 0x1000;
                    uint64_t pa = 0x40000000 + (va & 0xFFFFF000);  // Fake physical address
                    beacon_encoder_add_pte(&camera_encoder, pid, va, pa);
                }
            }
        }
    }
    
    fclose(fp);
    
    if (section_count > 0) {
        printf("Camera %d: Scanned PID %u - found %d sections\n", 
               CAMERA_ID, pid, section_count);
    }
}

// Read process details from /proc/[pid]/stat
void read_process_details(uint32_t pid, BeaconPIDEntry* entry) {
    char stat_path[256];
    snprintf(stat_path, sizeof(stat_path), "/proc/%u/stat", pid);
    
    FILE* fp = fopen(stat_path, "r");
    if (!fp) {
        // Process might have exited
        entry->type = 0;  // ENTRY_PID
        entry->pid = pid;
        entry->ppid = 0;
        entry->uid = 0;
        entry->gid = 0;
        entry->rss_kb = 0;
        snprintf(entry->comm, 16, "PID %u", pid);
        entry->state = '?';
        return;
    }
    
    // Parse /proc/[pid]/stat
    char comm[256];
    char state;
    int ppid;
    unsigned long rss_pages;
    
    // Format: pid (comm) state ppid ...
    fscanf(fp, "%*d (%255[^)]) %c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %*u %ld",
           comm, &state, &ppid, &rss_pages);
    fclose(fp);
    
    // Fill entry
    entry->type = 0;  // ENTRY_PID
    entry->pid = pid;
    entry->ppid = ppid;
    entry->uid = 0;  // Would need to read /proc/[pid]/status for this
    entry->gid = 0;
    entry->rss_kb = (rss_pages * 4096) / 1024;  // Convert pages to KB
    strncpy(entry->comm, comm, 15);
    entry->comm[15] = '\0';
    entry->state = state;
}

// Scan all PIDs and write to PID beacon pages
void scan_all_pids() {
    static uint32_t generation = 0;
    generation++;
    
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) {
        perror("opendir /proc");
        return;
    }
    
    // Collect all PID entries with details
    BeaconPIDEntry all_entries[1344];  // 16 pages * 84 entries per page
    int total_pids = 0;
    
    struct dirent* entry;
    while ((entry = readdir(proc_dir)) != NULL && total_pids < 1344) {
        char* endptr;
        uint32_t pid = strtoul(entry->d_name, &endptr, 10);
        if (*endptr == '\0' && pid > 0) {
            read_process_details(pid, &all_entries[total_pids]);
            total_pids++;
        }
    }
    closedir(proc_dir);
    
    // Now write PID entries to beacon pages
    int pids_written = 0;
    for (int page_idx = 0; page_idx < 16 && pids_written < total_pids; page_idx++) {
        BeaconPIDListPage* pid_page = (BeaconPIDListPage*)((uint8_t*)pids_ptr + page_idx * 4096);
        
        // Calculate how many PIDs go in this page
        int pids_in_page = total_pids - pids_written;
        if (pids_in_page > BEACON_MAX_PIDS_PER_PAGE) {
            pids_in_page = BEACON_MAX_PIDS_PER_PAGE;
        }
        
        // Update page header
        pid_page->generation = generation;
        pid_page->total_pids = total_pids;
        pid_page->pids_in_page = pids_in_page;
        
        // Copy PID entries
        memcpy(pid_page->entries, &all_entries[pids_written], pids_in_page * sizeof(BeaconPIDEntry));
        pids_written += pids_in_page;
        
        // Update version for tear detection
        pid_page->version_top++;
        pid_page->version_bottom = pid_page->version_top;
    }
    
    printf("Camera %d: Wrote %d PIDs to PID beacon pages (generation %u)\n", 
           CAMERA_ID, total_pids, generation);
}

// Check for camera control updates
void check_camera_control() {
    static uint32_t last_version = 0;
    
    // Get the appropriate camera pointer
    void* camera_ptr = (CAMERA_ID == 1) ? camera1_ptr : camera2_ptr;
    
    // Control page is always at page 0
    BeaconCameraControlPage* control = (BeaconCameraControlPage*)camera_ptr;
    
    // Check if control page was updated by Haywire
    if (control->version_top == control->version_bottom && 
        control->version_top > last_version) {
        
        // Check if target PID changed
        if (control->target_pid != target_pid && control->target_pid > 0) {
            printf("Camera %d: Switching focus from PID %u to %u (version %u)\n", 
                   CAMERA_ID, target_pid, control->target_pid, control->version_top);
            target_pid = control->target_pid;
            
            // Update status
            control->status = BEACON_CAMERA_STATUS_SWITCHING;
            control->current_pid = target_pid;
            
            // After switching, mark as active
            control->status = BEACON_CAMERA_STATUS_ACTIVE;
        }
        
        last_version = control->version_top;
    }
}

int main() {
    // Set up signal handler
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    // Initialize the 5 separate memory areas
    if (init_memory() < 0) {
        return 1;
    }
    
    // Get the appropriate camera pointer
    void* camera_ptr = (CAMERA_ID == 1) ? camera1_ptr : camera2_ptr;
    
    // Initialize PID encoder (writes to PID pages)
    beacon_encoder_init(&pid_encoder, OBSERVER_PID_SCANNER, 512, pids_ptr, 16 * 4096);
    
    // Initialize camera encoder (writes to camera data pages, skipping control page)
    // Start from page 1 since page 0 is the control page
    void* camera_data_base = (uint8_t*)camera_ptr + 4096;
    size_t camera_data_size = (BEACON_CAMERA1_PAGES - 1) * 4096;
    beacon_encoder_init(&camera_encoder, OBSERVER_CAMERA, 512, camera_data_base, camera_data_size);
    
    printf("Camera %d started with 4 beacon areas\n", CAMERA_ID);
    
    // Default to init process if no target set
    const char* pid_env = getenv("HAYWIRE_TARGET_PID");
    if (pid_env) {
        target_pid = atoi(pid_env);
        printf("Camera %d: Target PID set to %u from environment\n", CAMERA_ID, target_pid);
    } else {
        target_pid = 1;
    }
    
    // Main loop
    while (keep_running) {
        // Check for camera control updates from Haywire
        check_camera_control();
        
        // Scan all PIDs and write to PID beacon pages
        scan_all_pids();
        
        // Scan target process memory maps (camera functionality)
        if (target_pid > 0) {
            scan_process_memory(target_pid);
        }
        
        // Flush encoders to ensure data is written
        beacon_encoder_flush(&pid_encoder);
        beacon_encoder_flush(&camera_encoder);
        
        // Sleep before next scan
        sleep(1);
    }
    
    // Cleanup
    printf("Camera %d: Shutting down\n", CAMERA_ID);
    beacon_encoder_flush(&pid_encoder);
    beacon_encoder_flush(&camera_encoder);
    
    // Free all 4 memory areas
    if (master_page) free(master_page);
    if (pids_ptr) free(pids_ptr);
    if (camera1_ptr) free(camera1_ptr);
    if (camera2_ptr) free(camera2_ptr);
    
    return 0;
}