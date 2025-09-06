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
#define PID_PAGES         100   // PID list snapshots (multiple generations)
#define CAMERA1_PAGES     200   // Camera watching specific PID
#define CAMERA2_PAGES     200   // Camera watching another PID

// PID list configuration
#define MAX_PIDS_PER_PAGE  ((PAGE_SIZE - 48) / sizeof(uint32_t))  // ~1000 PIDs per page minus headers
#define PID_GENERATIONS    10   // Keep 10 generations of PID lists

// Physical address hints configuration
#define MAX_HINTS_PER_CATEGORY  100  // Max physical page hints per category
#define PAGEMAP_ENTRY_SIZE      8    // 64-bit entries in /proc/self/pagemap
#define PFN_MASK               ((1ULL << 55) - 1)  // Bits 0-54 are PFN
#define PAGE_PRESENT           (1ULL << 63)        // Bit 63 is present flag

// Process and memory section structures
#define PROCESS_NAME_LEN    16   // Same as kernel TASK_COMM_LEN
#define PATH_MAX_STORED     256  // Truncated path length
#define MAX_SECTIONS_PER_PROCESS 100  // Max memory sections we'll store

// Fixed-size process entry
typedef struct {
    uint32_t pid;
    uint32_t ppid;              // Parent PID
    uint32_t uid;
    uint32_t gid;
    char comm[PROCESS_NAME_LEN];  // Process name
    char state;                 // R/S/D/Z/T
    int8_t nice;
    uint16_t num_threads;
    uint64_t vsize;            // Virtual memory size
    uint64_t rss;              // Resident set size in pages
    uint64_t start_time;       // Process start time
    uint64_t utime;            // User CPU time
    uint64_t stime;            // System CPU time
    uint32_t num_sections;     // Number of memory sections
    char exe_path[PATH_MAX_STORED];  // Truncated executable path
} __attribute__((packed)) ProcessEntry;

// Fixed-size memory section entry (from /proc/pid/maps)
typedef struct {
    uint32_t pid;              // Which process this belongs to
    uint64_t start_addr;       // Start of memory region
    uint64_t end_addr;         // End of memory region
    uint32_t permissions;      // rwxp as bitfield
    uint64_t offset;           // File offset
    uint32_t major;            // Device major
    uint32_t minor;            // Device minor
    uint64_t inode;
    char pathname[PATH_MAX_STORED];  // Mapped file or [heap], [stack], etc.
} __attribute__((packed)) SectionEntry;

// Round-robin batch size
#define PIDS_PER_BATCH  5      // Process 5 PIDs per round-robin cycle

// Discovery header - First page of MASTER category with physical hints
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
    
    // Physical address hints for each category
    struct {
        uint32_t hint_count;     // Number of valid hints for this category
        uint64_t physical_pages[MAX_HINTS_PER_CATEGORY]; // Physical page addresses
    } hints[NUM_CATEGORIES];
    
    uint8_t padding[1016];      // Pad to PAGE_SIZE (reduced due to hints)
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

// PID list page - specialized beacon page for PID lists
typedef struct {
    // Header with version for tear detection
    uint32_t magic;          // BEACON_MAGIC
    uint32_t version_top;    // Version number at top (for tear detection)
    uint32_t session_id;     
    uint32_t category;       // CATEGORY_PID
    uint32_t generation;     // Which generation of PID list
    uint32_t total_pids;     // Total PIDs in this generation
    uint32_t page_number;    // Page N of M in this generation
    uint32_t pids_in_page;   // Number of PIDs in this page
    
    // PID array
    uint32_t pids[MAX_PIDS_PER_PAGE];  // Array of process IDs
    
    // Footer with version for tear detection
    uint32_t version_bottom; // Must match version_top for valid page
} __attribute__((packed)) PIDListPage;

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
static CategoryArray categories[NUM_CATEGORIES];
static uint32_t session_id = 0;
static volatile int running = 1;
static uint32_t current_generation = 0;
static uint32_t pid_write_offset = 0;  // Current write position in PID category
static uint32_t roundrobin_index = 0;  // Current position in PID list for round-robin

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
    
    for (int cat = 0; cat < NUM_CATEGORIES; cat++) {
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
    strncpy(entry->comm, comm, PROCESS_NAME_LEN-1);
    entry->comm[PROCESS_NAME_LEN-1] = '\0';
    
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
    ssize_t len = readlink(path, entry->exe_path, PATH_MAX_STORED-1);
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
        strncpy(sec->pathname, p, PATH_MAX_STORED-1);
        sec->pathname[PATH_MAX_STORED-1] = '\0';
        
        (*count)++;
    }
    
    fclose(fp);
    return 0;
}

// Process a batch of PIDs for round-robin
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
        write_to_category(CATEGORY_ROUNDROBIN, &proc_entry, sizeof(proc_entry));
        
        // Write SectionEntries to ROUNDROBIN category
        for (uint32_t j = 0; j < section_count; j++) {
            write_to_category(CATEGORY_ROUNDROBIN, &sections[j], sizeof(SectionEntry));
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
    uint32_t pages_needed = (total_pids + MAX_PIDS_PER_PAGE - 1) / MAX_PIDS_PER_PAGE;
    if (pages_needed == 0) pages_needed = 1;  // At least one page even if no PIDs
    
    // Make sure we have enough space in the PID category
    if (pages_needed > PID_PAGES / PID_GENERATIONS) {
        printf("Warning: PID list too large for allocated space\n");
        pages_needed = PID_PAGES / PID_GENERATIONS;
    }
    
    uint32_t pids_written = 0;
    uint32_t page_num = 0;
    uint32_t version = current_generation * 10000 + page_num;  // Unique version per page
    
    while (pids_written < total_pids && page_num < pages_needed) {
        // Get the next page in the circular buffer
        PIDListPage* page = (PIDListPage*)&categories[CATEGORY_PID].pages[pid_write_offset];
        
        // Write version at top for tear detection
        page->magic = BEACON_MAGIC;
        page->version_top = version;
        page->session_id = session_id;
        page->category = CATEGORY_PID;
        page->generation = current_generation;
        page->total_pids = total_pids;
        page->page_number = page_num;
        
        // Fill in PIDs for this page
        uint32_t pids_this_page = total_pids - pids_written;
        if (pids_this_page > MAX_PIDS_PER_PAGE) {
            pids_this_page = MAX_PIDS_PER_PAGE;
        }
        page->pids_in_page = pids_this_page;
        
        // Copy PIDs
        memcpy(page->pids, &all_pids[pids_written], pids_this_page * sizeof(uint32_t));
        
        // Clear unused PIDs in the page
        if (pids_this_page < MAX_PIDS_PER_PAGE) {
            memset(&page->pids[pids_this_page], 0, 
                   (MAX_PIDS_PER_PAGE - pids_this_page) * sizeof(uint32_t));
        }
        
        // Write version at bottom for tear detection
        page->version_bottom = version;
        
        // Move to next page
        pids_written += pids_this_page;
        page_num++;
        version++;
        pid_write_offset = (pid_write_offset + 1) % PID_PAGES;
    }
    
    printf("Generation %u: %u PIDs in %u pages\n", 
           current_generation, total_pids, page_num);
    
    current_generation++;
}

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
    
    // Populate physical address hints (once at startup)
    populate_physical_hints(discovery, categories);
    
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
        discovery->beacon_magic = BEACON_MAGIC;
        uint8_t disc_bytes2[4] = {0x48, 0x61, 0x79, 0x44};  // 'H', 'a', 'y', 'D'
        memcpy(&discovery->discovery_magic, disc_bytes2, 4);
        
        // Update category info in discovery
        for (int i = 0; i < NUM_CATEGORIES; i++) {
            discovery->categories[i].write_index = categories[i].write_index;
            discovery->categories[i].sequence = categories[i].sequence;
        }
        
        // Don't refresh hints - they're static after allocation
        
        // EVERY CYCLE: Scan and write complete PID list
        uint32_t pid_count = scan_pids(pid_buffer, 10000);
        write_pid_generation(pid_buffer, pid_count);
        
        // EVERY CYCLE: Simulate camera updates (would track specific PIDs)
        char test_data[100];
        snprintf(test_data, sizeof(test_data), "Camera1 tracking PID 1 at cycle %u", cycle);
        write_to_category(CATEGORY_CAMERA1, test_data, strlen(test_data));
        
        snprintf(test_data, sizeof(test_data), "Camera2 tracking PID 2 at cycle %u", cycle);
        write_to_category(CATEGORY_CAMERA2, test_data, strlen(test_data));
        
        // EVERY CYCLE: Round-robin processes PIDS_PER_BATCH PIDs
        process_roundrobin_batch(pid_buffer, pid_count, roundrobin_index);
        roundrobin_index += PIDS_PER_BATCH;
        if (roundrobin_index >= pid_count) {
            roundrobin_index = 0;  // Wrap around to beginning
            printf("  Round-robin: Completed full cycle through %u PIDs\n", pid_count);
        }
        
        printf("Cycle %u: Gen[%u] PIDOffset[%u] RR[%u] CAM1[%u] CAM2[%u]\n", 
               cycle, 
               current_generation - 1,  // Last written generation
               pid_write_offset,
               categories[CATEGORY_ROUNDROBIN].write_index,
               categories[CATEGORY_CAMERA1].write_index,
               categories[CATEGORY_CAMERA2].write_index);
        
        cycle++;
        sleep(1);
    }
    
    free(pid_buffer);
    
    // Cleanup
    printf("Cleaning up...\n");
    // Don't clear memory so Haywire can still see it
    // memset(aligned, 0, total_size);
    free(raw);
    
    return 0;
}