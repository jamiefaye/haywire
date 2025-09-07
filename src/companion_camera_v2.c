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
#include "beacon_encoder.h"

#define CAMERA_ID 1  // Camera 1 or 2
#define SCAN_INTERVAL_MS 100  // 10Hz scanning

// Memory for beacons - just a page-aligned buffer
static void* mem_base = NULL;
static size_t mem_size = 512 * 4096;  // 512 pages for camera beacons

// Beacon encoder
static BeaconEncoder encoder;

// Current target PID
static uint32_t target_pid = 0;

// Signal handler for clean shutdown
static volatile int keep_running = 1;
void sig_handler(int sig) {
    keep_running = 0;
}

// Initialize memory - just allocate a page-aligned buffer
int init_memory() {
    // Allocate page-aligned memory for beacons
    if (posix_memalign(&mem_base, 4096, mem_size) != 0) {
        perror("Failed to allocate page-aligned memory");
        return -1;
    }
    
    // Clear it
    memset(mem_base, 0, mem_size);
    
    printf("Camera %d: Allocated %zu KB of beacon memory\n", CAMERA_ID, mem_size / 1024);
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
    beacon_encoder_add_camera_header(&encoder, CAMERA_ID, pid, time(NULL));
    
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
            beacon_encoder_add_section(&encoder, pid, start, end - start, flags, path);
            section_count++;
            
            // Also scan for page table entries if this is a data section
            if ((flags & 0x2) && !(flags & 0x4)) {  // Writable, not executable
                // For now, just add a few sample PTEs
                // In real implementation, would read /proc/pid/pagemap
                for (int i = 0; i < 5 && (start + i * 0x1000) < end; i++) {
                    uint64_t va = start + i * 0x1000;
                    uint64_t pa = 0x40000000 + (va & 0xFFFFF000);  // Fake physical address
                    beacon_encoder_add_pte(&encoder, pid, va, pa);
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

// Check for camera control updates
void check_camera_control() {
    // For now, just stick with the default target PID
    // Control mechanism can be added later
    // (Would need separate control memory or file)
}

int main() {
    // Set up signal handler
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    // Initialize memory
    if (init_memory() < 0) {
        return 1;
    }
    
    // Initialize encoder as camera (does both PID scanning and memory maps)
    uint32_t observer_type = OBSERVER_CAMERA;
    beacon_encoder_init(&encoder, observer_type, 512, mem_base, mem_size);
    
    printf("Camera %d started (type=%u)\n", CAMERA_ID, observer_type);
    
    // Default to init process if no target set
    target_pid = 1;
    
    // Main loop - scan PIDs and camera target
    while (keep_running) {
        // First, scan all PIDs
        DIR* proc_dir = opendir("/proc");
        if (!proc_dir) {
            perror("opendir /proc");
            sleep(1);
            continue;
        }
        
        struct dirent* entry;
        int pid_count = 0;
        
        while ((entry = readdir(proc_dir)) != NULL) {
            // Check if it's a PID directory
            char* endptr;
            uint32_t pid = strtoul(entry->d_name, &endptr, 10);
            if (*endptr == '\0' && pid > 0) {
                // Try to get process state from /proc/[pid]/stat
                char stat_path[256];
                char comm[256] = "";
                char state = 'R';
                uint32_t ppid = 0;
                
                snprintf(stat_path, sizeof(stat_path), "/proc/%u/stat", pid);
                FILE* stat_file = fopen(stat_path, "r");
                if (stat_file) {
                    // Parse: pid (comm) state ppid ...
                    fscanf(stat_file, "%*d (%255[^)]) %c %u", comm, &state, &ppid);
                    fclose(stat_file);
                } else {
                    // Use PID as comm if can't read stat
                    snprintf(comm, sizeof(comm), "%u", pid);
                }
                
                // Add PID to beacon with actual state and ppid
                beacon_encoder_add_pid(&encoder, pid, ppid, 0, 0, 0, comm, state);
                pid_count++;
            }
        }
        
        closedir(proc_dir);
        
        // Then scan target process memory maps (camera functionality)
        if (target_pid > 0) {
            scan_process_memory(target_pid);
        }
        
        // Flush to ensure data is written
        beacon_encoder_flush(&encoder);
        
        printf("Scanned %d PIDs, camera focused on PID %u\n", pid_count, target_pid);
        
        // Sleep before next scan
        sleep(1);
    }
    
    // Cleanup
    printf("Camera %d: Shutting down\n", CAMERA_ID);
    beacon_encoder_flush(&encoder);
    
    if (mem_base) {
        free(mem_base);
    }
    
    return 0;
}