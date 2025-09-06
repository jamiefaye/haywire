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
#define MAX_BEACONS 2048  // 8MB total

// Compile-time assertion macro
#define STATIC_ASSERT(cond, msg) typedef char static_assertion_##msg[(cond)?1:-1]

// Beacon types
enum BeaconType {
    BEACON_TYPE_CONTROL = 1,    // Control/heartbeat 
    BEACON_TYPE_PROCLIST = 2,   // Process list
    BEACON_TYPE_SECTIONS = 3,   // Memory sections
    BEACON_TYPE_STATUS = 4,     // Companion status
};

// 16-byte beacon header
typedef struct {
    uint32_t magic;
    uint32_t session_id;
    uint32_t beacon_type;
    uint32_t type_index;
    uint8_t data[4080];
} __attribute__((packed)) BeaconPage;

// Compile-time check that BeaconPage is exactly one page
STATIC_ASSERT(sizeof(BeaconPage) == PAGE_SIZE, BeaconPage_must_be_4096_bytes);

// Control beacon - always at index 0
typedef struct {
    uint32_t heartbeat;         // Incremented each cycle
    uint32_t companion_status;  // RUNNING, STOPPING, etc
    uint32_t generation;        // Data version
    uint32_t beacon_count;      // Total beacons in use
    uint32_t process_count;     // Number of processes
    uint32_t section_count;     // Number of sections
    uint64_t last_update;       // Timestamp
    uint32_t update_interval_ms; // How often we update
    char message[64];           // Status message
} ControlBeacon;

// Process entry - packed array
typedef struct {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    uint64_t vsize_kb;
    uint64_t rss_kb;
    uint64_t cpu_time;
    char name[64];
    char state;
    uint8_t padding[3];
} __attribute__((packed)) ProcessEntry;

// Ensure ProcessEntry has expected size (108 bytes on this platform)
STATIC_ASSERT(sizeof(ProcessEntry) == 108, ProcessEntry_must_be_108_bytes);

// Process list beacon - can hold many processes
typedef struct {
    uint32_t count;             // Number of processes in this beacon
    uint32_t total_processes;   // Total across all process beacons
    uint32_t continuation;      // Next beacon index if more
    ProcessEntry processes[37]; // 108 bytes each, 37 fit in 4080 bytes (37*108=3996)
} ProcessListBeacon;

// Check it fits in beacon data area
STATIC_ASSERT(sizeof(ProcessListBeacon) <= 4080, ProcessListBeacon_must_fit_in_beacon);

// Memory section entry
typedef struct {
    uint32_t pid;               // Which process owns this
    uint64_t va_start;
    uint64_t va_end;
    uint32_t perms;             // R=1, W=2, X=4, P=8
    uint32_t offset;
    uint32_t major;
    uint32_t minor;
    uint32_t inode;
    char path[128];
} __attribute__((packed)) SectionEntry;

// Section list beacon
typedef struct {
    uint32_t count;             // Sections in this beacon
    uint32_t total_sections;    // Total across all section beacons
    uint32_t continuation;      // Next beacon index if more
    SectionEntry sections[22];  // ~180 bytes each, 22 fit in 4080
} SectionListBeacon;

// Check section entry size (168 bytes on this platform)
STATIC_ASSERT(sizeof(SectionEntry) == 168, SectionEntry_must_be_168_bytes);
// 24 sections * 168 bytes = 4032 bytes + 12 header bytes = 4044 bytes (fits!)
STATIC_ASSERT(sizeof(SectionListBeacon) <= 4080, SectionListBeacon_must_fit_in_beacon);

// Global state
static BeaconPage* beacon_array = NULL;
static uint32_t session_id = 0;
static volatile int running = 1;
static uint32_t next_beacon = 0;

// Allocate next available beacon
uint32_t allocate_beacon(uint32_t type) {
    if (next_beacon >= MAX_BEACONS) {
        fprintf(stderr, "Out of beacons!\n");
        return 0xFFFFFFFF;
    }
    
    uint32_t index = next_beacon++;
    beacon_array[index].magic = BEACON_MAGIC;
    beacon_array[index].session_id = session_id;
    beacon_array[index].beacon_type = type;
    beacon_array[index].type_index = index;
    
    return index;
}

// Read process info from /proc/PID/stat
int read_process_info(uint32_t pid, ProcessEntry* entry) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    
    char comm[256];
    char state;
    int ppid;
    unsigned long vsize, rss, utime, stime;
    
    // Parse stat file (simplified - real format is complex)
    fscanf(f, "%*d (%255[^)]) %c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %lu %lu",
           comm, &state, &ppid, &utime, &stime, &vsize, &rss);
    
    fclose(f);
    
    entry->pid = pid;
    entry->ppid = ppid;
    entry->state = state;
    entry->vsize_kb = vsize / 1024;
    entry->rss_kb = rss * (PAGE_SIZE / 1024);  // rss is in pages
    entry->cpu_time = utime + stime;
    strncpy(entry->name, comm, 63);
    entry->name[63] = 0;
    
    // Get UID/GID from /proc/PID/status
    snprintf(path, sizeof(path), "/proc/%u/status", pid);
    f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "Uid:", 4) == 0) {
                sscanf(line, "Uid:\t%u", &entry->uid);
            } else if (strncmp(line, "Gid:", 4) == 0) {
                sscanf(line, "Gid:\t%u", &entry->gid);
                break;
            }
        }
        fclose(f);
    }
    
    return 1;
}

// Scan all processes
void update_process_list() {
    DIR* proc = opendir("/proc");
    if (!proc) return;
    
    // Start with first process beacon
    uint32_t beacon_idx = allocate_beacon(BEACON_TYPE_PROCLIST);
    ProcessListBeacon* beacon = (ProcessListBeacon*)beacon_array[beacon_idx].data;
    beacon->count = 0;
    beacon->total_processes = 0;
    beacon->continuation = 0xFFFFFFFF;
    
    struct dirent* entry;
    uint32_t total = 0;
    
    while ((entry = readdir(proc)) != NULL) {
        // Skip non-numeric entries
        uint32_t pid = atoi(entry->d_name);
        if (pid == 0) continue;
        
        ProcessEntry proc_entry;
        if (read_process_info(pid, &proc_entry)) {
            // Add to current beacon
            if (beacon->count >= 37) {  // Max processes per beacon
                // Need new beacon
                beacon->total_processes = total;
                uint32_t new_idx = allocate_beacon(BEACON_TYPE_PROCLIST);
                beacon->continuation = new_idx;
                
                beacon = (ProcessListBeacon*)beacon_array[new_idx].data;
                beacon->count = 0;
                beacon->continuation = 0xFFFFFFFF;
            }
            
            beacon->processes[beacon->count++] = proc_entry;
            total++;
        }
    }
    
    beacon->total_processes = total;
    closedir(proc);
    
    printf("Updated process list: %u processes\n", total);
}

// Read memory sections from /proc/PID/maps
int read_sections(uint32_t pid, SectionEntry* sections, int max_sections) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%u/maps", pid);
    
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    
    int count = 0;
    char line[512];
    
    while (fgets(line, sizeof(line), f) && count < max_sections) {
        SectionEntry* s = &sections[count];
        char perms[5];
        
        int n = sscanf(line, "%llx-%llx %4s %x %x:%x %u %511s",
                       &s->va_start, &s->va_end, perms,
                       &s->offset, &s->major, &s->minor,
                       &s->inode, s->path);
        
        if (n < 7) continue;
        if (n < 8) s->path[0] = 0;  // No path
        
        s->pid = pid;
        s->perms = 0;
        if (perms[0] == 'r') s->perms |= 1;
        if (perms[1] == 'w') s->perms |= 2;
        if (perms[2] == 'x') s->perms |= 4;
        if (perms[3] == 'p') s->perms |= 8;
        
        count++;
    }
    
    fclose(f);
    return count;
}

// Update memory sections for all processes
void update_sections() {
    // Start with first section beacon
    uint32_t beacon_idx = allocate_beacon(BEACON_TYPE_SECTIONS);
    SectionListBeacon* beacon = (SectionListBeacon*)beacon_array[beacon_idx].data;
    beacon->count = 0;
    beacon->total_sections = 0;
    beacon->continuation = 0xFFFFFFFF;
    
    uint32_t total = 0;
    
    // Walk through process list to get PIDs
    DIR* proc = opendir("/proc");
    if (!proc) return;
    
    struct dirent* entry;
    while ((entry = readdir(proc)) != NULL) {
        uint32_t pid = atoi(entry->d_name);
        if (pid == 0) continue;
        
        SectionEntry temp_sections[100];
        int n = read_sections(pid, temp_sections, 100);
        
        for (int i = 0; i < n; i++) {
            if (beacon->count >= 22) {
                // Need new beacon
                beacon->total_sections = total;
                uint32_t new_idx = allocate_beacon(BEACON_TYPE_SECTIONS);
                beacon->continuation = new_idx;
                
                beacon = (SectionListBeacon*)beacon_array[new_idx].data;
                beacon->count = 0;
                beacon->continuation = 0xFFFFFFFF;
            }
            
            beacon->sections[beacon->count++] = temp_sections[i];
            total++;
        }
    }
    
    beacon->total_sections = total;
    closedir(proc);
    
    printf("Updated sections: %u total\n", total);
}

// Signal handler
void sighandler(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    running = 0;
}

int main(int argc, char* argv[]) {
    printf("=== Haywire Companion Starting ===\n");
    
    // Setup signal handlers
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    
    // Allocate beacon array (page-aligned)
    size_t total_size = MAX_BEACONS * PAGE_SIZE;
    void* raw = malloc(total_size + PAGE_SIZE);
    if (!raw) {
        perror("malloc");
        return 1;
    }
    
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    beacon_array = (BeaconPage*)aligned;
    
    printf("Allocated %zu MB at %p\n", total_size/(1024*1024), beacon_array);
    
    // Initialize session
    session_id = getpid();
    memset(beacon_array, 0, total_size);
    
    // Setup control beacon
    uint32_t control_idx = allocate_beacon(BEACON_TYPE_CONTROL);
    ControlBeacon* control = (ControlBeacon*)beacon_array[control_idx].data;
    control->companion_status = 1;  // RUNNING
    control->update_interval_ms = 1000;
    snprintf(control->message, sizeof(control->message), "Companion initialized");
    
    printf("Session ID: 0x%08X\n", session_id);
    printf("Control beacon at index %u\n", control_idx);
    
    // Main loop
    uint32_t cycle = 0;
    while (running) {
        printf("\nCycle %u...\n", cycle);
        
        // Update heartbeat
        control->heartbeat = cycle++;
        control->last_update = time(NULL);
        control->generation++;
        
        // Update process list
        next_beacon = 1;  // Reset allocation (keep control)
        update_process_list();
        control->process_count = ((ProcessListBeacon*)beacon_array[1].data)->total_processes;
        
        // Update sections
        update_sections();
        control->section_count = 0;  // Count from section beacons
        
        control->beacon_count = next_beacon;
        
        printf("Using %u beacons\n", next_beacon);
        
        // Sleep
        sleep(control->update_interval_ms / 1000);
        
        // Check for Haywire presence (could check control beacon for commands)
    }
    
    // Cleanup
    printf("Cleaning up...\n");
    control->companion_status = 0;  // STOPPED
    snprintf(control->message, sizeof(control->message), "Companion stopped");
    
    // Clear beacons
    memset(beacon_array, 0, total_size);
    free(raw);
    
    return 0;
}