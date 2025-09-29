/**
 * Simplified Triggered Companion for Haywire
 *
 * Instead of continuous updates, this version:
 * - Runs once when triggered via QGA
 * - No control pages or h2g communication
 * - No tear detection needed (single-shot write)
 * - Simplified beacon format (header + data, no footer)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

// Beacon magic numbers (compatible with original)
#define BEACON_MAGIC1 0x3142FACE
#define BEACON_MAGIC2 0xCAFEBABE

// Observer types
#define OBSERVER_TRIGGERED 0x10  // New type for triggered mode

// Entry types
#define ENTRY_TYPE_PID 0x01
#define ENTRY_TYPE_MAPS_HEADER 0x10
#define ENTRY_TYPE_MAPS_DATA 0x11

#pragma pack(push, 1)

// Simplified beacon header (no write_seq or footer needed)
typedef struct {
    uint32_t magic1;
    uint32_t magic2;
    uint16_t observer_type;
    uint16_t page_count;      // Total pages in this beacon
    uint32_t request_id;       // Unique ID from QGA request
    uint32_t timestamp;
    uint32_t entry_count;
    uint32_t focus_pid;        // PID to get detailed info for (0 = none)
    uint32_t data_offset;      // Offset to start of entries
    uint8_t reserved[8];
} beacon_header_t;

// PID entry format
typedef struct {
    uint8_t entry_type;        // ENTRY_TYPE_PID
    uint8_t name_len;
    uint16_t entry_size;
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t vsize;           // Virtual memory size
    uint32_t rss;             // Resident set size
    char name[32];            // Process name
} pid_entry_t;

// Maps entry format
typedef struct {
    uint8_t entry_type;        // ENTRY_TYPE_MAPS_DATA
    uint8_t reserved;
    uint16_t entry_size;
    uint32_t pid;
    uint32_t data_len;
    // Followed by maps text data
} maps_entry_t;

#pragma pack(pop)

// Global beacon memory
static void* beacon_memory = NULL;
static size_t beacon_size = 0;

/**
 * Count processes to determine beacon size needed
 */
static int count_processes() {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return 0;

    int count = 0;
    struct dirent* entry;

    while ((entry = readdir(proc_dir)) != NULL) {
        int pid = atoi(entry->d_name);
        if (pid > 0) count++;
    }

    closedir(proc_dir);
    return count;
}

/**
 * Read process info from /proc
 */
static int read_process_info(int pid, pid_entry_t* entry) {
    char path[256];
    FILE* fp;

    entry->entry_type = ENTRY_TYPE_PID;
    entry->pid = pid;
    entry->ppid = 0;
    entry->uid = 0;
    entry->vsize = 0;
    entry->rss = 0;

    // Read comm (process name)
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    fp = fopen(path, "r");
    if (fp) {
        if (fgets(entry->name, sizeof(entry->name), fp)) {
            // Remove newline
            char* nl = strchr(entry->name, '\n');
            if (nl) *nl = '\0';
        }
        fclose(fp);
    } else {
        snprintf(entry->name, sizeof(entry->name), "[pid:%d]", pid);
    }

    entry->name_len = strlen(entry->name);

    // Read stat for ppid, vsize, rss
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (fp) {
        char stat_line[1024];
        if (fgets(stat_line, sizeof(stat_line), fp)) {
            // Parse stat line (simplified - just get ppid for now)
            char* p = strrchr(stat_line, ')');
            if (p) {
                int ppid;
                unsigned long vsize, rss;
                // Skip to fields after comm
                if (sscanf(p + 2, "%*c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                          "%*u %*u %*u %*u %*u %*u %*u %*u %*u %lu %lu",
                          &ppid, &vsize, &rss) >= 1) {
                    entry->ppid = ppid;
                    entry->vsize = vsize / 1024;  // Convert to KB
                    entry->rss = rss * 4;  // Pages to KB
                }
            }
        }
        fclose(fp);
    }

    entry->entry_size = sizeof(pid_entry_t);
    return 0;
}

/**
 * Write all PIDs to beacon
 */
static uint8_t* write_pid_list(uint8_t* ptr, int* count) {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return ptr;

    struct dirent* entry;
    *count = 0;

    while ((entry = readdir(proc_dir)) != NULL) {
        int pid = atoi(entry->d_name);
        if (pid > 0) {
            pid_entry_t* pid_entry = (pid_entry_t*)ptr;
            if (read_process_info(pid, pid_entry) == 0) {
                ptr += sizeof(pid_entry_t);
                (*count)++;
            }
        }
    }

    closedir(proc_dir);
    return ptr;
}

/**
 * Write memory maps for a specific PID
 */
static uint8_t* write_memory_maps(uint8_t* ptr, int pid) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE* fp = fopen(path, "r");
    if (!fp) return ptr;

    // Get file size
    struct stat st;
    if (fstat(fileno(fp), &st) != 0) {
        fclose(fp);
        return ptr;
    }

    maps_entry_t* maps_hdr = (maps_entry_t*)ptr;
    maps_hdr->entry_type = ENTRY_TYPE_MAPS_DATA;
    maps_hdr->reserved = 0;
    maps_hdr->pid = pid;
    maps_hdr->data_len = 0;

    ptr += sizeof(maps_entry_t);

    // Read maps data
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        int len = strlen(line);
        memcpy(ptr, line, len);
        ptr += len;
        maps_hdr->data_len += len;
    }

    maps_hdr->entry_size = sizeof(maps_entry_t) + maps_hdr->data_len;

    fclose(fp);
    return ptr;
}

/**
 * Create triggered beacon snapshot
 */
static int create_beacon(uint32_t request_id, int focus_pid) {
    // Calculate size needed
    int process_count = count_processes();
    size_t size_needed = sizeof(beacon_header_t) +
                        (process_count * sizeof(pid_entry_t));

    // Add space for maps if focus PID specified
    if (focus_pid > 0) {
        size_needed += 65536;  // Reserve 64KB for maps data
    }

    // Round up to page size
    int pages = (size_needed / 4096) + 1;
    beacon_size = pages * 4096;

    // Allocate beacon memory (visible to QEMU)
    beacon_memory = mmap(NULL, beacon_size,
                        PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_SHARED, -1, 0);

    if (beacon_memory == MAP_FAILED) {
        fprintf(stderr, "Failed to allocate beacon memory: %s\n", strerror(errno));
        return -1;
    }

    // Clear memory
    memset(beacon_memory, 0, beacon_size);

    // Write header
    beacon_header_t* hdr = (beacon_header_t*)beacon_memory;
    hdr->magic1 = BEACON_MAGIC1;
    hdr->magic2 = BEACON_MAGIC2;
    hdr->observer_type = OBSERVER_TRIGGERED;
    hdr->page_count = pages;
    hdr->request_id = request_id;
    hdr->timestamp = time(NULL);
    hdr->focus_pid = focus_pid;
    hdr->data_offset = sizeof(beacon_header_t);

    // Write process list
    uint8_t* ptr = (uint8_t*)beacon_memory + sizeof(beacon_header_t);
    int pid_count = 0;
    ptr = write_pid_list(ptr, &pid_count);
    hdr->entry_count = pid_count;

    // Write maps for focus PID if specified
    if (focus_pid > 0) {
        ptr = write_memory_maps(ptr, focus_pid);
    }

    // Output beacon location for QGA/scanner to find
    printf("BEACON_READY:%p:SIZE:%zu:MAGIC:%08x:PAGES:%d\n",
           beacon_memory, beacon_size, request_id, pages);
    fflush(stdout);

    return 0;
}

/**
 * Main entry point
 */
int main(int argc, char* argv[]) {
    uint32_t request_id = 0;
    int focus_pid = 0;
    int keep_alive = 0;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--request=", 10) == 0) {
            request_id = strtoul(argv[i] + 10, NULL, 0);
        } else if (strncmp(argv[i], "--focus=", 8) == 0) {
            focus_pid = atoi(argv[i] + 8);
        } else if (strcmp(argv[i], "--keep-alive") == 0) {
            keep_alive = 1;  // Keep memory mapped after exit
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --request=ID    Set request ID (magic number)\n");
            printf("  --focus=PID     Include detailed maps for PID\n");
            printf("  --keep-alive    Keep beacon memory after exit\n");
            printf("\nTriggered mode for Haywire - runs once and exits\n");
            return 0;
        }
    }

    // Generate request ID if not specified
    if (request_id == 0) {
        request_id = (uint32_t)time(NULL) ^ (uint32_t)getpid();
    }

    // Create beacon
    if (create_beacon(request_id, focus_pid) != 0) {
        return 1;
    }

    // If keep-alive, sleep to keep memory mapped
    if (keep_alive) {
        fprintf(stderr, "Beacon created. Keeping memory mapped...\n");
        fprintf(stderr, "Kill with: kill %d\n", getpid());
        while (1) {
            sleep(3600);
        }
    }

    // Otherwise exit immediately (memory stays mapped briefly)
    return 0;
}