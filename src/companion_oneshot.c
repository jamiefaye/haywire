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
#include <stdint.h>
#include <getopt.h>
#include "beacon_protocol.h"

#define CAMERA_ID 1  // Camera 1 or 2
#define SCAN_INTERVAL_MS 100  // 10Hz scanning

// Command line options
static int run_once = 0;           // Run single cycle then exit
static uint32_t request_id = 0;    // Optional request ID for tracking
static int need_keeper = 0;        // Whether we need to spawn keeper daemon

// Control pages removed - all pages are data pages now

// Four separate memory allocations (no round-robin)
static void* master_page = NULL;           // 1 page for discovery/master
static void* pids_ptr = NULL;              // 16 pages for PID lists  
static void* camera1_ptr = NULL;           // 200 data pages (no control page)
static void* camera2_ptr = NULL;           // 200 data pages (no control page)

// Current target PID for this camera
static uint32_t target_pid = 0;

// Camera data writing state
static uint32_t camera_sequence = 0;  // Sequence number for camera data pages
static uint32_t camera_write_index = 0;  // Which data page to write next (0-199, no control page)

// Signal handler for clean shutdown
static volatile int keep_running = 1;
void sig_handler(int sig) {
    keep_running = 0;
}

// Initialize the 5 separate beacon memory areas
int init_memory() {
    // When in single-shot mode, use POSIX shared memory for persistence
    if (run_once) {
        // Try to open existing shared memory first
        int fd = shm_open("/haywire_beacon", O_RDWR, 0666);

        if (fd < 0) {
            // Doesn't exist, create it
            fd = shm_open("/haywire_beacon", O_CREAT | O_RDWR, 0666);
            if (fd < 0) {
                perror("Failed to create shared memory");
                return -1;
            }
            printf("Created new shared memory segment /dev/shm/haywire_beacon\n");
            need_keeper = 1;  // First time, need keeper daemon
        } else {
            printf("Using existing shared memory segment /dev/shm/haywire_beacon\n");
            need_keeper = 0;  // Already exists, keeper must be running
        }

        // Calculate total size needed
        size_t total_size = (BEACON_MASTER_PAGES + 16 + BEACON_CAMERA1_PAGES + BEACON_CAMERA2_PAGES) * 4096;

        // Extend shared memory to required size
        if (ftruncate(fd, total_size) < 0) {
            perror("Failed to resize shared memory");
            close(fd);
            return -1;
        }

        // Map the shared memory
        void* mapped = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) {
            perror("Failed to mmap shared memory");
            close(fd);
            return -1;
        }

        // Close fd - shared memory persists in /dev/shm
        close(fd);

        // Assign regions within the mapped area
        uint8_t* ptr = (uint8_t*)mapped;
        master_page = ptr;
        ptr += BEACON_MASTER_PAGES * 4096;
        pids_ptr = ptr;
        ptr += 16 * 4096;
        camera1_ptr = ptr;
        ptr += BEACON_CAMERA1_PAGES * 4096;
        camera2_ptr = ptr;

        // Clear all pages
        memset(mapped, 0, total_size);

        printf("Shared memory segment size: %zu KB\n", total_size / 1024);
    } else {
        // Original malloc-based allocation for continuous mode
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
    }
    
    // Initialize master/discovery page
    BeaconDiscoveryPage* discovery = (BeaconDiscoveryPage*)master_page;
    discovery->magic = BEACON_MAGIC;
    discovery->version_top = 1;
    discovery->version_bottom = 1;
    discovery->session_id = request_id ? request_id : getpid();
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
        pid_page->session_id = request_id ? request_id : getpid();
        pid_page->category = BEACON_CATEGORY_PID;
        pid_page->category_index = i;
        pid_page->timestamp = discovery->timestamp;
        pid_page->generation = 0;
        pid_page->total_pids = 0;
        pid_page->pids_in_page = 0;
    }
    
    // Initialize BOTH camera beacon pages (even though we only use camera1 for data)
    // This ensures Haywire can find all expected beacon pages

    // Initialize Camera1 pages - ALL as data pages now (no control page)
    // We keep page 0 as a placeholder but don't use it as a control page
    for (int i = 0; i < BEACON_CAMERA1_PAGES; i++) {
        BeaconPage* data_page = (BeaconPage*)((uint8_t*)camera1_ptr + i * 4096);
        data_page->magic = BEACON_MAGIC;
        data_page->version_top = 1;
        data_page->version_bottom = 1;
        data_page->session_id = request_id ? request_id : getpid();
        data_page->category = BEACON_CATEGORY_CAMERA1;
        data_page->category_index = i;
        data_page->timestamp = discovery->timestamp;
        data_page->sequence = 0;
        data_page->data_size = 0;
    }

    // Initialize Camera2 pages - ALL as data pages now (no control page)
    for (int i = 0; i < BEACON_CAMERA2_PAGES; i++) {
        BeaconPage* data_page = (BeaconPage*)((uint8_t*)camera2_ptr + i * 4096);
        data_page->magic = BEACON_MAGIC;
        data_page->version_top = 1;
        data_page->version_bottom = 1;
        data_page->session_id = request_id ? request_id : getpid();
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

// Read page table entries from /proc/pid/pagemap
int read_ptes_for_region(uint32_t pid, uint64_t start_va, uint64_t end_va, 
                         uint8_t** write_ptr, size_t* bytes_used, uint16_t* entry_count,
                         size_t max_bytes) {
    char pagemap_path[256];
    snprintf(pagemap_path, sizeof(pagemap_path), "/proc/%u/pagemap", pid);
    
    int pagemap_fd = open(pagemap_path, O_RDONLY);
    if (pagemap_fd < 0) {
        // Can't read pagemap (might need root)
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "Cannot open %s: %s (using fake PTEs)\n", pagemap_path, strerror(errno));
            warned = 1;
        }
        return 0;
    }
    
    int ptes_written = 0;
    uint64_t page_size = 4096;
    
    // Iterate through pages in the region
    for (uint64_t va = start_va; va < end_va; va += page_size) {
        // Check if we have room for a PTE entry (24 bytes)
        if (*bytes_used + sizeof(BeaconPTEEntry) > max_bytes) {
            break;  // No room in this page
        }
        
        // Calculate pagemap offset for this virtual address
        uint64_t pagemap_offset = (va / page_size) * sizeof(uint64_t);
        
        // Seek to the pagemap entry
        if (lseek(pagemap_fd, pagemap_offset, SEEK_SET) < 0) {
            continue;
        }
        
        // Read the pagemap entry
        uint64_t pagemap_entry;
        if (read(pagemap_fd, &pagemap_entry, sizeof(pagemap_entry)) != sizeof(pagemap_entry)) {
            continue;
        }
        
        // Parse pagemap entry
        // Bit 63: page present
        // Bits 0-54: page frame number (if present)
        // Bit 62: page swapped
        // Bit 61: page exclusively mapped
        uint64_t present = (pagemap_entry >> 63) & 1;
        uint64_t pfn = pagemap_entry & ((1ULL << 55) - 1);
        
        // Debug: Print what we're seeing in pagemap
        static int debug_count = 0;
        if (debug_count < 10) {
            fprintf(stderr, "VA 0x%lx: pagemap_entry=0x%lx, present=%lu, pfn=0x%lx\n", 
                    va, pagemap_entry, present, pfn);
            debug_count++;
        }
        
        // Only add PTE if page is present (allocated)
        if (present && pfn != 0) {
            uint64_t pa = pfn * page_size;
            
            BeaconPTEEntry* pte = (BeaconPTEEntry*)*write_ptr;
            pte->type = BEACON_ENTRY_TYPE_PTE;
            pte->reserved[0] = pte->reserved[1] = pte->reserved[2] = 0;
            pte->flags = 0x1;  // Present flag
            pte->va = va;
            pte->pa = pa;
            
            *write_ptr += sizeof(BeaconPTEEntry);
            *bytes_used += sizeof(BeaconPTEEntry);
            (*entry_count)++;
            ptes_written++;
            
            // No limit - send all present PTEs for the section
        }
    }
    
    close(pagemap_fd);
    return ptes_written;
}

// Write camera data using stream format
void scan_process_memory(uint32_t pid) {
    char maps_path[256];
    snprintf(maps_path, sizeof(maps_path), "/proc/%u/maps", pid);

    fprintf(stderr, "scan_process_memory: Scanning PID %u from %s\n", pid, maps_path);

    FILE* fp = fopen(maps_path, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open %s: %s\n", maps_path, strerror(errno));
        return;
    }
    
    // Track if we've preloaded libraries for this PID
    static uint32_t last_preload_pid = 0;
    static int libraries_preloaded = 0;
    
    // Reset preload flag when PID changes
    if (pid != last_preload_pid) {
        libraries_preloaded = 0;
        last_preload_pid = pid;
        fprintf(stderr, "Camera %d: Switched to PID %u, will preload libraries\n", CAMERA_ID, pid);
    }
    
    // Get the appropriate camera pointer
    void* camera_ptr = (CAMERA_ID == 1) ? camera1_ptr : camera2_ptr;
    
    // Start fresh from page 0 (all pages are data pages now)
    camera_write_index = 0;
    camera_sequence++;
    
    // Current page being written
    BeaconCameraDataPage* current_page = (BeaconCameraDataPage*)((uint8_t*)camera_ptr + camera_write_index * 4096);
    uint8_t* write_ptr = current_page->data;
    uint16_t entry_count = 0;
    size_t bytes_used = 0;
    
    // Update page header
    current_page->magic = BEACON_MAGIC;
    current_page->version_top = camera_sequence;
    current_page->session_id = request_id ? request_id : getpid();
    current_page->category = (CAMERA_ID == 1) ? BEACON_CATEGORY_CAMERA1 : BEACON_CATEGORY_CAMERA2;
    current_page->category_index = camera_write_index;
    current_page->timestamp = time(NULL);
    current_page->target_pid = pid;
    current_page->entry_count = 0;
    current_page->continuation = 0;
    current_page->version_bottom = camera_sequence;  // Match version_top to prevent torn reads
    
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
            // Check if we have room for a section entry (96 bytes)
            if (bytes_used + sizeof(BeaconSectionEntry) > 4060) {
                // Finish current page and start a new one
                current_page->entry_count = entry_count;
                current_page->continuation = 1;  // More pages follow
                current_page->version_bottom = current_page->version_top;
                
                // Move to next page
                camera_write_index++;
                if (camera_write_index >= BEACON_CAMERA1_PAGES) {
                    // Out of pages, stop scanning
                    break;
                }
                
                current_page = (BeaconCameraDataPage*)((uint8_t*)camera_ptr + camera_write_index * 4096);
                write_ptr = current_page->data;
                entry_count = 0;
                bytes_used = 0;
                
                // Initialize new page header
                current_page->magic = BEACON_MAGIC;
                current_page->version_top = camera_sequence;
                current_page->session_id = request_id ? request_id : getpid();
                current_page->category = (CAMERA_ID == 1) ? BEACON_CATEGORY_CAMERA1 : BEACON_CATEGORY_CAMERA2;
                current_page->category_index = camera_write_index;
                current_page->timestamp = time(NULL);
                current_page->target_pid = pid;
                current_page->entry_count = 0;
                current_page->continuation = 0;
                current_page->version_bottom = camera_sequence;  // Match version_top to prevent torn reads
            }
            
            // Write section entry
            BeaconSectionEntry* section = (BeaconSectionEntry*)write_ptr;
            section->type = BEACON_ENTRY_TYPE_SECTION;
            section->pid = pid;
            section->va_start = start;
            section->va_end = end;
            
            // Calculate flags from permissions
            section->perms = 0;
            if (perms[0] == 'r') section->perms |= 0x1;  // PROT_READ
            if (perms[1] == 'w') section->perms |= 0x2;  // PROT_WRITE
            if (perms[2] == 'x') section->perms |= 0x4;  // PROT_EXEC
            if (perms[3] == 'p') section->perms |= 0x8;  // MAP_PRIVATE
            else section->perms |= 0x10;  // MAP_SHARED
            
            strncpy(section->path, path, 63);
            section->path[63] = '\0';
            
            write_ptr += sizeof(BeaconSectionEntry);
            bytes_used += sizeof(BeaconSectionEntry);
            entry_count++;
            section_count++;
            
            // Preload shared library pages if not done yet
            // ONLY do this for our own process to avoid segfaults
            if (!libraries_preloaded && pid == getpid() && strstr(path, ".so")) {
                // Only preload executable sections (most useful for disassembly)
                if (perms[2] == 'x') {
                    int pages_touched = 0;
                    volatile uint8_t dummy;
                    
                    // Touch the first byte of each page to trigger page fault
                    for (uint64_t addr = start; addr < end && addr < start + (100 * 4096); addr += 4096) {
                        // Limit to first 100 pages (400KB) per library to avoid excessive delays
                        dummy = *(uint8_t*)addr;
                        pages_touched++;
                    }
                    
                    if (pages_touched > 0) {
                        fprintf(stderr, "Camera %d: Preloaded %d pages from %s\n", 
                                CAMERA_ID, pages_touched, path);
                    }
                }
            }
            
            // Add real PTEs for all sections with any permissions
            if (section->perms != 0) {  // Any section with permissions (readable, writable, or executable)
                // Try to read real PTEs from pagemap
                int ptes_added = read_ptes_for_region(pid, start, end, 
                                                      &write_ptr, &bytes_used, &entry_count,
                                                      4060 - bytes_used);
                if (ptes_added > 0) {
                    // Successfully read real PTEs
                    // write_ptr, bytes_used, and entry_count already updated by read_ptes_for_region
                } else {
                    // Fallback: Add a few fake PTEs if pagemap not available
                    static int fake_pte_count = 0;
                    for (int i = 0; i < 3 && (start + i * 0x1000) < end; i++) {
                        // Check if we have room for a PTE entry (24 bytes)
                        if (bytes_used + sizeof(BeaconPTEEntry) > 4060) {
                            break;  // No room in this page
                        }
                        
                        BeaconPTEEntry* pte = (BeaconPTEEntry*)write_ptr;
                        pte->type = BEACON_ENTRY_TYPE_PTE;
                        pte->reserved[0] = pte->reserved[1] = pte->reserved[2] = 0;
                        pte->flags = 0x1;  // Present flag
                        pte->va = start + i * 0x1000;
                        pte->pa = 0x40000000 + (pte->va & 0xFFFFF000);  // Fake physical address
                        
                        write_ptr += sizeof(BeaconPTEEntry);
                        bytes_used += sizeof(BeaconPTEEntry);
                        entry_count++;
                        fake_pte_count++;
                    }
                    if (fake_pte_count <= 10) {
                        fprintf(stderr, "Added fake PTEs for section at 0x%lx\n", start);
                    }
                }
            }
        }
    }
    
    fclose(fp);
    
    // Mark libraries as preloaded for this PID
    libraries_preloaded = 1;
    
    // Mark end of entries
    if (bytes_used + 1 <= 4060) {
        *write_ptr = BEACON_ENTRY_TYPE_END;
        bytes_used++;
    }
    
    // Finish the last page
    current_page->entry_count = entry_count;
    current_page->continuation = 0;  // Last page
    current_page->version_bottom = current_page->version_top;
    
    if (section_count > 0) {
        printf("Camera %d: Wrote %d sections for PID %u to pages 1-%u\n", 
               CAMERA_ID, section_count, pid, camera_write_index);
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

// check_camera_control removed - target PID now comes from command line

void print_usage(const char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  --once              Run one cycle and exit\n");
    printf("  --request=ID        Set request ID (for tracking)\n");
    printf("  --target=PID        Set target PID for camera\n");
    printf("  --help              Show this help\n");
    printf("\n");
    printf("Default: Run continuously (original behavior)\n");
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    static struct option long_options[] = {
        {"once",    no_argument,       0, 'o'},
        {"request", required_argument, 0, 'r'},
        {"target",  required_argument, 0, 't'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "hor:t:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'o':
                run_once = 1;
                printf("Single-shot mode enabled\n");
                break;
            case 'r':
                request_id = strtoul(optarg, NULL, 0);
                printf("Request ID: 0x%08x\n", request_id);
                break;
            case 't':
                target_pid = strtoul(optarg, NULL, 0);
                printf("Target PID: %u\n", target_pid);
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    // Set up signal handler (only for continuous mode)
    if (!run_once) {
        signal(SIGINT, sig_handler);
        signal(SIGTERM, sig_handler);
    }
    
    // Initialize the 5 separate memory areas
    if (init_memory() < 0) {
        return 1;
    }
    
    if (run_once) {
        printf("Running single beacon cycle...\n");

        // Single cycle operation
        scan_all_pids();

        if (target_pid > 0) {
            scan_process_memory(target_pid);
        }

        printf("Beacon written to /dev/shm/haywire_beacon\n");
        printf("  Master: offset 0x%lx\n", (unsigned long)((uint8_t*)master_page - (uint8_t*)master_page));
        printf("  PIDs:   offset 0x%lx\n", (unsigned long)((uint8_t*)pids_ptr - (uint8_t*)master_page));
        printf("  Camera1: offset 0x%lx\n", (unsigned long)((uint8_t*)camera1_ptr - (uint8_t*)master_page));
        printf("  Camera2: offset 0x%lx\n", (unsigned long)((uint8_t*)camera2_ptr - (uint8_t*)master_page));

        // Ensure all data is written
        msync(master_page, (BEACON_MASTER_PAGES + 16 + BEACON_CAMERA1_PAGES + BEACON_CAMERA2_PAGES) * 4096, MS_SYNC);

        if (need_keeper) {
            // First run - need to spawn keeper daemon
            pid_t pid = fork();
            if (pid == 0) {
                // Child: become the keeper daemon
                setsid();

                // Close stdio
                close(STDIN_FILENO);
                close(STDOUT_FILENO);
                close(STDERR_FILENO);

                // Keep the shared memory mapped forever
                while (1) {
                    sleep(86400);  // Sleep for a day
                }
            } else {
                // Parent: report and exit
                printf("Single cycle complete. Keeper daemon started (PID %d)\n", pid);
                printf("Beacon data at /dev/shm/haywire_beacon\n");
            }
        } else {
            // Keeper already running, just updated the beacon
            printf("Single cycle complete. Beacon updated at /dev/shm/haywire_beacon\n");
        }
    } else {
        printf("Camera %d started in continuous mode\n", CAMERA_ID);

        // Default to init process if no target set
        if (target_pid == 0) {
            const char* pid_env = getenv("HAYWIRE_TARGET_PID");
            if (pid_env) {
                target_pid = atoi(pid_env);
                printf("Camera %d: Target PID set to %u from environment\n", CAMERA_ID, target_pid);
            } else {
                target_pid = 1;
            }
        }

        // Main loop
        while (keep_running) {
            scan_all_pids();

            if (target_pid > 0) {
                scan_process_memory(target_pid);
            }

            sleep(1);
        }

        printf("Shutting down\n");
    }

    // Cleanup
    if (!run_once) {
        // Only free in continuous mode
        if (master_page) free(master_page);
        if (pids_ptr) free(pids_ptr);
        if (camera1_ptr) free(camera1_ptr);
        if (camera2_ptr) free(camera2_ptr);
    }
    // In single-shot mode, the memory-mapped file persists after exit
    
    return 0;
}