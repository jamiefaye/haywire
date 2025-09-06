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
#include <ctype.h>
#include <sys/mman.h>
#include "beacon_protocol.h"

// Additional defines not in shared protocol
#define PAGE_SIZE BEACON_PAGE_SIZE
#define MAX_SECTIONS_PER_PROCESS BEACON_MAX_SECTIONS
#define PAGEMAP_ENTRY_SIZE 8
#define PFN_MASK ((1ULL << 55) - 1)
#define PAGE_PRESENT (1ULL << 63)
#define MAX_HINTS_PER_CATEGORY 0  // Hints system removed

// Type aliases for compatibility with existing code
typedef BeaconProcessEntry ProcessEntry;
typedef BeaconSectionEntry SectionEntry;
typedef BeaconPIDListPage PIDListPage;
typedef BeaconCameraControlPage CameraControlPage;
typedef BeaconDiscoveryPage DiscoveryPage;

// Camera configuration
#define CAMERA_CONTROL_PAGE  0  // First page of camera category is control
#define PIDS_PER_BATCH      5  // Process 5 PIDs per round-robin cycle

// PTE scanning configuration
#define PTES_PER_PAGE  512      // 512 8-byte PTEs per 4KB page
#define MAX_RLE_ENTRIES 1000    // Max RLE entries we'll store per beacon page

// PTE page with RLE compression (not in shared protocol yet)
typedef struct {
    uint32_t magic;             // BEACON_MAGIC
    uint32_t version_top;
    uint32_t pid;               // Process ID
    uint32_t section_index;     // Which memory section
    uint64_t start_vaddr;       // Starting virtual address
    uint32_t entry_count;       // Number of RLE entries
    
    // RLE format: [zero_count:u32][pte:u64] pairs
    // zero_count==0 means single non-zero PTE
    uint8_t rle_data[4040];     
    
    uint32_t version_bottom;
} __attribute__((packed)) PTEPage;

// Category arrays - each is a contiguous block of beacon pages
typedef struct {
    BeaconPage* pages;       // Pointer to page array
    uint32_t page_count;     // Number of pages
    uint32_t write_index;    // Current write position
    uint32_t sequence;       // Global sequence counter
} CategoryArray;

// Forward declarations
BeaconPage* write_to_category(uint32_t category_id, void* data, size_t size);

// Global state
static DiscoveryPage* discovery = NULL;
static CategoryArray categories[BEACON_NUM_CATEGORIES];
static uint32_t session_id = 0;
static uint32_t session_timestamp = 0;  // Set once at startup
static volatile int running = 1;
static uint32_t current_generation = 0;
static uint32_t pid_write_offset = 0;  // Current write position in PID category
static uint32_t roundrobin_index = 0;  // Current position in PID list for round-robin

// Physical hints system removed - keeping functions for potential future use
#if 0
// Read physical address from /proc/self/pagemap for a virtual address
uint64_t get_physical_addr(void* virtual_addr) {
    static int pagemap_fd = -1;
    
    // Open pagemap on first call
    if (pagemap_fd < 0) {
        pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
        if (pagemap_fd < 0) {
            perror("open /proc/self/pagemap");
            return 0;
        }
    }
    
    // Calculate page offset in pagemap file
    uintptr_t vaddr = (uintptr_t)virtual_addr;
    off_t offset = (vaddr / PAGE_SIZE) * PAGEMAP_ENTRY_SIZE;
    
    // Seek to the right position
    if (lseek(pagemap_fd, offset, SEEK_SET) != offset) {
        perror("lseek pagemap");
        return 0;
    }
    
    // Read the 64-bit entry
    uint64_t entry;
    if (read(pagemap_fd, &entry, sizeof(entry)) != sizeof(entry)) {
        perror("read pagemap");
        return 0;
    }
    
    // Check if page is present
    if (!(entry & PAGE_PRESENT)) {
        return 0;  // Page not present in memory
    }
    
    // Extract PFN and convert to physical address
    uint64_t pfn = entry & PFN_MASK;
    uint64_t phys_addr = pfn * PAGE_SIZE;
    
    return phys_addr;
}

// Populate physical address hints for all categories
void populate_physical_hints(DiscoveryPage* discovery, CategoryArray* categories) {
    printf("Collecting physical address hints...\n");
    
    for (int cat = 0; cat < BEACON_NUM_CATEGORIES; cat++) {
        discovery->hints[cat].hint_count = 0;
        
        if (!categories[cat].pages) continue;
        
        // Sample some pages from each category (not all, to save space)
        uint32_t pages_to_sample = categories[cat].page_count;
        if (pages_to_sample > MAX_HINTS_PER_CATEGORY) {
            pages_to_sample = MAX_HINTS_PER_CATEGORY;
        }
        
        // Sample evenly across the category
        uint32_t step = categories[cat].page_count / pages_to_sample;
        if (step == 0) step = 1;
        
        uint32_t hint_count = 0;
        for (uint32_t i = 0; i < categories[cat].page_count && hint_count < MAX_HINTS_PER_CATEGORY; i += step) {
            void* page_addr = &categories[cat].pages[i];
            uint64_t phys_addr = get_physical_addr(page_addr);
            
            if (phys_addr != 0) {
                discovery->hints[cat].physical_pages[hint_count] = phys_addr;
                hint_count++;
                
                if (hint_count <= 3) {  // Show first few
                    printf("  Cat %d page %u: virt %p -> phys 0x%llx\n", 
                           cat, i, page_addr, (unsigned long long)phys_addr);
                }
            }
        }
        
        discovery->hints[cat].hint_count = hint_count;
        if (hint_count > 3) {
            printf("  Cat %d: %u physical hints collected\n", cat, hint_count);
        }
    }
}
#endif

// Read process information from /proc/[pid]/stat
int read_process_stat(uint32_t pid, ProcessEntry* entry) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;
    
    entry->pid = pid;
    
    // Read the stat file - format: pid (comm) state ppid ...
    char comm[256];
    char state;
    int ppid, pgrp, session, tty_nr, tpgid;
    unsigned long flags, minflt, cminflt, majflt, cmajflt;
    unsigned long utime, stime, cutime, cstime;
    long priority, nice, num_threads, itrealvalue;
    unsigned long long starttime;
    unsigned long vsize_tmp;
    long rss_tmp;
    
    // Skip to after the comm field which can contain spaces/parens
    fscanf(fp, "%*d (");
    
    // Read comm until the last )
    int i = 0;
    int paren_count = 1;
    int c;
    while ((c = fgetc(fp)) != EOF && i < sizeof(comm)-1) {
        if (c == '(') paren_count++;
        else if (c == ')') {
            paren_count--;
            if (paren_count == 0) break;
        }
        comm[i++] = c;
    }
    comm[i] = '\0';
    strncpy(entry->comm, comm, BEACON_PROCESS_NAME_LEN-1);
    entry->comm[BEACON_PROCESS_NAME_LEN-1] = '\0';
    
    // Read the rest of the fields
    fscanf(fp, " %c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %llu %lu %ld",
           &state, &ppid, &pgrp, &session, &tty_nr, &tpgid,
           &flags, &minflt, &cminflt, &majflt, &cmajflt,
           &utime, &stime, &cutime, &cstime,
           &priority, &nice, &num_threads, &itrealvalue,
           &starttime, &vsize_tmp, &rss_tmp);
    
    entry->state = state;
    entry->ppid = ppid;
    entry->nice = nice;
    entry->num_threads = num_threads;
    entry->vsize = vsize_tmp;
    entry->rss = rss_tmp;
    entry->start_time = starttime;
    entry->utime = utime;
    entry->stime = stime;
    
    fclose(fp);
    
    // Try to read exe path
    snprintf(path, sizeof(path), "/proc/%u/exe", pid);
    ssize_t len = readlink(path, entry->exe_path, BEACON_PATH_MAX_STORED-1);
    if (len > 0) {
        entry->exe_path[len] = '\0';
    } else {
        entry->exe_path[0] = '\0';
    }
    
    // Try to read uid/gid from status
    snprintf(path, sizeof(path), "/proc/%u/status", pid);
    fp = fopen(path, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "Uid:", 4) == 0) {
                sscanf(line, "Uid:\t%u", &entry->uid);
            } else if (strncmp(line, "Gid:", 4) == 0) {
                sscanf(line, "Gid:\t%u", &entry->gid);
                break;
            }
        }
        fclose(fp);
    }
    
    return 0;
}

// Read memory sections from /proc/[pid]/maps
int read_process_maps(uint32_t pid, SectionEntry* sections, uint32_t max_sections, uint32_t* count) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/maps", pid);
    
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;
    
    char line[1024];
    *count = 0;
    
    while (fgets(line, sizeof(line), fp) && *count < max_sections) {
        SectionEntry* sec = &sections[*count];
        sec->pid = pid;
        
        char perms[5];
        char pathname[512] = "";
        
        // Parse: address perms offset dev inode pathname
        unsigned long start, end, offset, inode;
        int ret = sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %511[^\n]",
                        &start, &end,
                        perms, &offset,
                        &sec->major, &sec->minor,
                        &inode, pathname);
        
        sec->start_addr = start;
        sec->end_addr = end;
        sec->offset = offset;
        sec->inode = inode;
        
        // Convert permissions to bitfield
        sec->permissions = 0;
        if (perms[0] == 'r') sec->permissions |= 0x4;
        if (perms[1] == 'w') sec->permissions |= 0x2;
        if (perms[2] == 'x') sec->permissions |= 0x1;
        if (perms[3] == 'p') sec->permissions |= 0x8;  // Private
        if (perms[3] == 's') sec->permissions |= 0x10; // Shared
        
        // Copy pathname, removing leading spaces
        char* p = pathname;
        while (*p == ' ' || *p == '\t') p++;
        strncpy(sec->pathname, p, BEACON_PATH_MAX_STORED-1);
        sec->pathname[BEACON_PATH_MAX_STORED-1] = '\0';
        
        (*count)++;
    }
    
    fclose(fp);
    return 0;
}

// Read and RLE-compress PTEs for a memory section
int read_process_ptes_rle(uint32_t pid, SectionEntry* section, uint32_t camera_id) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/pagemap", pid);
    
    int pagemap_fd = open(path, O_RDONLY);
    if (pagemap_fd < 0) {
        return -1;  // Can't read pagemap (need root)
    }
    
    // Calculate page range for this section
    uint64_t start_page = section->start_addr / PAGE_SIZE;
    uint64_t end_page = (section->end_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t num_pages = end_page - start_page;
    
    // Process in chunks that fit in a beacon page
    uint64_t pages_processed = 0;
    
    while (pages_processed < num_pages) {
        PTEPage* pte_page = (PTEPage*)write_to_category(camera_id, NULL, 0);
        if (!pte_page) break;
        
        pte_page->magic = BEACON_MAGIC;
        pte_page->version_top = camera_id * 10000 + pages_processed;
        pte_page->pid = pid;
        pte_page->section_index = 0;  // Would track section index
        pte_page->start_vaddr = section->start_addr + (pages_processed * PAGE_SIZE);
        
        // RLE compression
        uint32_t rle_offset = 0;
        uint32_t entry_count = 0;
        uint32_t zero_run = 0;
        
        // Read PTEs for this chunk
        uint64_t chunk_size = 500;  // Process 500 pages at a time
        if (pages_processed + chunk_size > num_pages) {
            chunk_size = num_pages - pages_processed;
        }
        
        for (uint64_t i = 0; i < chunk_size && rle_offset < sizeof(pte_page->rle_data) - 12; i++) {
            // Seek to PTE location
            off_t offset = (start_page + pages_processed + i) * 8;
            if (lseek(pagemap_fd, offset, SEEK_SET) != offset) {
                break;
            }
            
            uint64_t pte;
            if (read(pagemap_fd, &pte, 8) != 8) {
                break;
            }
            
            if (pte == 0) {
                // Count zeros
                zero_run++;
            } else {
                // Write zero run if any
                if (zero_run > 0) {
                    memcpy(&pte_page->rle_data[rle_offset], &zero_run, 4);
                    rle_offset += 4;
                    entry_count++;
                    zero_run = 0;
                }
                
                // Write single non-zero PTE (zero_count=0 marker)
                uint32_t zero_marker = 0;
                memcpy(&pte_page->rle_data[rle_offset], &zero_marker, 4);
                rle_offset += 4;
                memcpy(&pte_page->rle_data[rle_offset], &pte, 8);
                rle_offset += 8;
                entry_count++;
            }
        }
        
        // Write final zero run if any
        if (zero_run > 0 && rle_offset < sizeof(pte_page->rle_data) - 4) {
            memcpy(&pte_page->rle_data[rle_offset], &zero_run, 4);
            rle_offset += 4;
            entry_count++;
        }
        
        pte_page->entry_count = entry_count;
        pte_page->version_bottom = pte_page->version_top;
        
        pages_processed += chunk_size;
    }
    
    close(pagemap_fd);
    return 0;
}

// Process a single PID for camera monitoring
void process_camera_pid(uint32_t pid, uint32_t camera_id) {
    // Read process info (same as round-robin)
    ProcessEntry proc_entry;
    if (read_process_stat(pid, &proc_entry) < 0) {
        printf("  Camera %u: PID %u disappeared\n", camera_id - BEACON_CATEGORY_CAMERA1 + 1, pid);
        return;
    }
    
    // Read memory sections
    SectionEntry sections[MAX_SECTIONS_PER_PROCESS];
    uint32_t section_count = 0;
    read_process_maps(pid, sections, MAX_SECTIONS_PER_PROCESS, &section_count);
    proc_entry.num_sections = section_count;
    
    // Write ProcessEntry to camera category
    write_to_category(camera_id, &proc_entry, sizeof(proc_entry));
    
    // Write SectionEntries
    for (uint32_t j = 0; j < section_count; j++) {
        write_to_category(camera_id, &sections[j], sizeof(SectionEntry));
        
        // For cameras, also scan PTEs with RLE compression
        read_process_ptes_rle(pid, &sections[j], camera_id);
    }
    
    printf("  Camera %u: PID %u (%s) - %u sections with PTEs\n", 
           camera_id - BEACON_CATEGORY_CAMERA1 + 1, pid, proc_entry.comm, section_count);
}

// Check camera control page for focus changes
uint32_t check_camera_control(uint32_t camera_id) {
    CameraControlPage* control = (CameraControlPage*)&categories[camera_id].pages[CAMERA_CONTROL_PAGE];
    
    // Check if control page has valid command
    if (control->magic == BEACON_MAGIC && 
        control->version_top == control->version_bottom &&
        control->command == 1) {  // change_focus command
        
        uint32_t new_pid = control->target_pid;
        control->command = 0;  // Clear command
        control->current_pid = new_pid;
        control->status = 2;  // Active
        
        printf("  Camera %u: Switching focus to PID %u\n", 
               camera_id - BEACON_CATEGORY_CAMERA1 + 1, new_pid);
        return new_pid;
    }
    
    // Return current PID or default
    if (control->current_pid > 0) {
        return control->current_pid;
    }
    
    // Default PIDs if not set
    return (camera_id == BEACON_CATEGORY_CAMERA1) ? 1 : 2;
}

// Process a batch of PIDs for round-robin (no PTEs)
void process_roundrobin_batch(uint32_t* pids, uint32_t pid_count, uint32_t start_idx) {
    for (uint32_t i = 0; i < PIDS_PER_BATCH && start_idx + i < pid_count; i++) {
        uint32_t pid = pids[start_idx + i];
        
        // Read process info
        ProcessEntry proc_entry;
        if (read_process_stat(pid, &proc_entry) < 0) {
            continue;  // Process might have disappeared
        }
        
        // Read memory sections
        SectionEntry sections[MAX_SECTIONS_PER_PROCESS];
        uint32_t section_count = 0;
        read_process_maps(pid, sections, MAX_SECTIONS_PER_PROCESS, &section_count);
        proc_entry.num_sections = section_count;
        
        // Write ProcessEntry to ROUNDROBIN category
        write_to_category(BEACON_CATEGORY_ROUNDROBIN, &proc_entry, sizeof(proc_entry));
        
        // Write SectionEntries to ROUNDROBIN category
        for (uint32_t j = 0; j < section_count; j++) {
            write_to_category(BEACON_CATEGORY_ROUNDROBIN, &sections[j], sizeof(SectionEntry));
        }
        
        printf("  RR: PID %u (%s) - %u sections\n", pid, proc_entry.comm, section_count);
    }
}

// Scan /proc for PIDs and return count
uint32_t scan_pids(uint32_t* pid_array, uint32_t max_pids) {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) {
        perror("opendir /proc");
        return 0;
    }
    
    struct dirent* entry;
    uint32_t count = 0;
    
    while ((entry = readdir(proc_dir)) != NULL && count < max_pids) {
        // Check if the directory name is all digits (a PID)
        char* endptr;
        long pid = strtol(entry->d_name, &endptr, 10);
        
        if (*endptr == '\0' && pid > 0 && pid <= 999999) {
            // Valid PID
            pid_array[count++] = (uint32_t)pid;
        }
    }
    
    closedir(proc_dir);
    return count;
}

// Write a complete generation of PID list to the PID category
void write_pid_generation(uint32_t* all_pids, uint32_t total_pids) {
    // Calculate how many pages we need for this generation
    uint32_t pages_needed = (total_pids + BEACON_MAX_PIDS_PER_PAGE - 1) / BEACON_MAX_PIDS_PER_PAGE;
    if (pages_needed == 0) pages_needed = 1;  // At least one page even if no PIDs
    
    // Make sure we have enough space in the PID category
    if (pages_needed > BEACON_PID_PAGES / BEACON_PID_GENERATIONS) {
        printf("Warning: PID list too large for allocated space\n");
        pages_needed = BEACON_PID_PAGES / BEACON_PID_GENERATIONS;
    }
    
    uint32_t pids_written = 0;
    uint32_t page_num = 0;
    uint32_t version = current_generation * 10000 + page_num;  // Unique version per page
    
    while (pids_written < total_pids && page_num < pages_needed) {
        // Get the next page in the circular buffer
        PIDListPage* page = (PIDListPage*)&categories[BEACON_CATEGORY_PID].pages[pid_write_offset];
        
        // Write version at top for tear detection
        page->magic = BEACON_MAGIC;
        page->version_top = version;
        page->session_id = session_id;
        page->category = BEACON_CATEGORY_PID;
        page->category_index = page_num;  // Now in standard position!
        page->timestamp = discovery->timestamp;
        page->generation = current_generation;
        page->total_pids = total_pids;
        
        // Fill in PIDs for this page
        uint32_t pids_this_page = total_pids - pids_written;
        if (pids_this_page > BEACON_MAX_PIDS_PER_PAGE) {
            pids_this_page = BEACON_MAX_PIDS_PER_PAGE;
        }
        page->pids_in_page = pids_this_page;
        
        // Copy PIDs
        memcpy(page->pids, &all_pids[pids_written], pids_this_page * sizeof(uint32_t));
        
        // Clear unused PIDs in the page
        if (pids_this_page < BEACON_MAX_PIDS_PER_PAGE) {
            memset(&page->pids[pids_this_page], 0, 
                   (BEACON_MAX_PIDS_PER_PAGE - pids_this_page) * sizeof(uint32_t));
        }
        
        // Write version at bottom for tear detection
        page->version_bottom = version;
        
        // Move to next page
        pids_written += pids_this_page;
        page_num++;
        version++;
        pid_write_offset = (pid_write_offset + 1) % BEACON_PID_PAGES;
    }
    
    printf("Generation %u: %u PIDs in %u pages\n", 
           current_generation, total_pids, page_num);
    
    current_generation++;
}

// Write data to a category, handling wraparound
BeaconPage* write_to_category(uint32_t category_id, void* data, size_t size) {
    if (category_id >= BEACON_NUM_CATEGORIES) return NULL;
    
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
    page->timestamp = session_timestamp;  // Use session timestamp
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
    session_timestamp = (uint32_t)time(NULL);  // Set once at startup
    
    // Calculate total pages needed
    uint32_t page_counts[BEACON_NUM_CATEGORIES] = {
        BEACON_MASTER_PAGES, BEACON_ROUNDROBIN_PAGES, BEACON_PID_PAGES, 
        BEACON_CAMERA1_PAGES, BEACON_CAMERA2_PAGES
    };
    
    uint32_t total_pages = 0;
    for (int i = 0; i < BEACON_NUM_CATEGORIES; i++) {
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
    
    printf("Discovery page will be at %p\n", aligned);
    
    // Set up category arrays
    uint8_t* current = (uint8_t*)aligned;
    for (int i = 0; i < BEACON_NUM_CATEGORIES; i++) {
        categories[i].pages = (BeaconPage*)current;
        categories[i].page_count = page_counts[i];
        categories[i].write_index = 0;
        categories[i].sequence = 0;
        current += page_counts[i] * PAGE_SIZE;
    }
    
    // First page of MASTER category is discovery
    discovery = (DiscoveryPage*)categories[BEACON_CATEGORY_MASTER].pages;
    
    // Initialize discovery page with standard header
    discovery->magic = BEACON_MAGIC;
    discovery->version_top = 1;
    discovery->session_id = session_id;
    discovery->category = BEACON_CATEGORY_MASTER;  // Category 0
    discovery->category_index = 0;  // Discovery page
    discovery->timestamp = session_timestamp;  // Use session timestamp
    discovery->version_bottom = 1;  // Match version_top
    
    // Fill in category information in discovery
    uint32_t offset = 0;
    for (int i = 0; i < BEACON_NUM_CATEGORIES; i++) {
        discovery->categories[i].base_offset = offset;
        discovery->categories[i].page_count = categories[i].page_count;
        discovery->categories[i].write_index = 0;
        discovery->categories[i].sequence = 0;
        printf("  Discovery: Category %d - offset=%u, page_count=%u\n", 
               i, offset, categories[i].page_count);
        offset += categories[i].page_count * PAGE_SIZE;
    }
    
    printf("Discovery page initialized with %d categories\n", BEACON_NUM_CATEGORIES);
    
    // Initialize ALL pages in each category to force them into physical memory
    printf("Initializing all beacon pages to force physical allocation...\n");
    for (int cat = 0; cat < BEACON_NUM_CATEGORIES; cat++) {
        int pages_initialized = 0;
        for (int page = 0; page < categories[cat].page_count; page++) {
            BeaconPage* bp = &categories[cat].pages[page];
            
            // First, zero the entire page to force it into physical memory
            memset(bp, 0, PAGE_SIZE);
            
            // Now set up the beacon header
            bp->magic = BEACON_MAGIC;
            bp->session_id = session_id;
            bp->category = cat;
            bp->category_index = page;
            bp->timestamp = session_timestamp;  // Set session timestamp
            bp->sequence = 0;  // Will be updated when page is actually written
            bp->data_size = 0;
            bp->version_top = bp->version_bottom = 0;
            
            pages_initialized++;
        }
        printf("  Category %d: initialized %d pages\n", cat, pages_initialized);
    }
    
    printf("All beacon pages initialized and forced into physical memory\n");
    
    // Initialize camera control pages with standard header
    CameraControlPage* cam1_control = (CameraControlPage*)&categories[BEACON_CATEGORY_CAMERA1].pages[0];
    cam1_control->magic = BEACON_MAGIC;
    cam1_control->version_top = 1;
    cam1_control->session_id = session_id;
    cam1_control->category = BEACON_CATEGORY_CAMERA1;
    cam1_control->category_index = 0;
    cam1_control->command = 0;
    cam1_control->target_pid = 1;
    cam1_control->current_pid = 1;
    cam1_control->status = 2;  // Active
    cam1_control->version_bottom = 1;
    
    CameraControlPage* cam2_control = (CameraControlPage*)&categories[BEACON_CATEGORY_CAMERA2].pages[0];
    cam2_control->magic = BEACON_MAGIC;
    cam2_control->version_top = 1;
    cam2_control->session_id = session_id;
    cam2_control->category = BEACON_CATEGORY_CAMERA2;
    cam2_control->category_index = 0;
    cam2_control->command = 0;
    cam2_control->target_pid = 2;
    cam2_control->current_pid = 2;
    cam2_control->status = 2;  // Active
    cam2_control->version_bottom = 1;
    
    printf("Camera 1 initialized to monitor PID 1\n");
    printf("Camera 2 initialized to monitor PID 2\n");
    
    // Physical hints system removed for simplicity
    // populate_physical_hints(discovery, categories);
    
    // Allocate buffer for PID scanning
    uint32_t* pid_buffer = malloc(10000 * sizeof(uint32_t));  // Up to 10000 PIDs
    if (!pid_buffer) {
        perror("malloc pid_buffer");
        return 1;
    }
    
    // Main loop - scan PIDs every cycle (run continuously)
    uint32_t cycle = 0;
    while (running) {
        // Update discovery page
        discovery->magic = BEACON_MAGIC;
        discovery->version_top = 1;
        discovery->session_id = session_id;
        discovery->category = BEACON_CATEGORY_MASTER;
        discovery->category_index = 0;
        discovery->timestamp = session_timestamp;  // Keep using session timestamp
        discovery->version_bottom = 1;  // Match version_top
        
        // Update category info in discovery
        uint32_t cat_offset = 0;
        for (int i = 0; i < BEACON_NUM_CATEGORIES; i++) {
            discovery->categories[i].base_offset = cat_offset;
            discovery->categories[i].page_count = categories[i].page_count;
            discovery->categories[i].write_index = categories[i].write_index;
            discovery->categories[i].sequence = categories[i].sequence;
            cat_offset += categories[i].page_count * PAGE_SIZE;
        }
        
        // Don't refresh hints - they're static after allocation
        
        // EVERY CYCLE: Scan and write complete PID list
        uint32_t pid_count = scan_pids(pid_buffer, 10000);
        write_pid_generation(pid_buffer, pid_count);
        
        // EVERY CYCLE: Camera 1 - check control and monitor PID
        uint32_t camera1_pid = check_camera_control(BEACON_CATEGORY_CAMERA1);
        process_camera_pid(camera1_pid, BEACON_CATEGORY_CAMERA1);
        
        // EVERY CYCLE: Camera 2 - check control and monitor PID
        uint32_t camera2_pid = check_camera_control(BEACON_CATEGORY_CAMERA2);
        process_camera_pid(camera2_pid, BEACON_CATEGORY_CAMERA2);
        
        // EVERY CYCLE: Round-robin processes 5 PIDs (no PTEs)
        process_roundrobin_batch(pid_buffer, pid_count, roundrobin_index);
        roundrobin_index += 5;
        if (roundrobin_index >= pid_count) {
            roundrobin_index = 0;  // Wrap around to beginning
            printf("  Round-robin: Completed full cycle through %u PIDs\n", pid_count);
        }
        
        printf("Cycle %u: Gen[%u] PIDOffset[%u] RR[%u] CAM1[%u] CAM2[%u]\n", 
               cycle, 
               current_generation - 1,  // Last written generation
               pid_write_offset,
               categories[BEACON_CATEGORY_ROUNDROBIN].write_index,
               categories[BEACON_CATEGORY_CAMERA1].write_index,
               categories[BEACON_CATEGORY_CAMERA2].write_index);
        
        cycle++;
        sleep(1);
    }
    
    free(pid_buffer);
    
    // Cleanup
    printf("Cleaning up...\n");
    // Clear all beacon pages to be tidy
    memset(aligned, 0, total_size);
    free(raw);
    
    return 0;
}