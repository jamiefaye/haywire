/*
 * beacon_protocol.h - Shared protocol definitions for Haywire beacon system
 * 
 * This header is shared between:
 * - Companion process (C code running in VM)
 * - Haywire reader (C++ code on host)
 * 
 * CRITICAL: All structures must be exactly sized for page alignment
 * DO NOT MODIFY without updating both sides!
 */

#ifndef BEACON_PROTOCOL_H
#define BEACON_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Core constants
#define BEACON_PAGE_SIZE        4096
#define BEACON_MAGIC            0x3142FACE
#define BEACON_DISCOVERY_MAGIC  0x44796148  // "HayD"

// Beacon categories
#define BEACON_CATEGORY_MASTER      0
#define BEACON_CATEGORY_ROUNDROBIN  1  
#define BEACON_CATEGORY_PID         2
#define BEACON_CATEGORY_CAMERA1     3
#define BEACON_CATEGORY_CAMERA2     4
#define BEACON_NUM_CATEGORIES       5

// Pages per category (must match companion allocation)
#define BEACON_MASTER_PAGES      1     // Just the discovery page
#define BEACON_ROUNDROBIN_PAGES  500   // Round-robin process scanning
#define BEACON_PID_PAGES         32    // PID list snapshots (32KB, ~32k PIDs)
#define BEACON_CAMERA1_PAGES     200   // Camera watching specific PID
#define BEACON_CAMERA2_PAGES     200   // Camera watching another PID

// Process and path limits
#define BEACON_PROCESS_NAME_LEN  16    // Same as kernel TASK_COMM_LEN
#define BEACON_PATH_MAX_STORED   256   // Truncated path length
#define BEACON_MAX_SECTIONS      100   // Max memory sections per process

// PID list configuration
#define BEACON_MAX_PIDS_PER_PAGE ((BEACON_PAGE_SIZE - 32 - 4) / 4)  // 1010 PIDs
#define BEACON_PID_GENERATIONS   10    // Keep 10 generations of PID lists


// Camera commands
#define BEACON_CAMERA_CMD_NONE          0
#define BEACON_CAMERA_CMD_CHANGE_FOCUS  1

// Camera status
#define BEACON_CAMERA_STATUS_IDLE       0
#define BEACON_CAMERA_STATUS_SWITCHING  1
#define BEACON_CAMERA_STATUS_ACTIVE     2

/*
 * All structures are packed to ensure exact layout.
 * Every structure that represents a beacon page MUST be exactly 4096 bytes.
 */

#pragma pack(push, 1)

// Fixed-size process entry (344 bytes packed)
typedef struct BeaconProcessEntry {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    char comm[BEACON_PROCESS_NAME_LEN];     // Process name
    char state;                              // R/S/D/Z/T
    int8_t nice;
    uint16_t num_threads;
    uint64_t vsize;                          // Virtual memory size
    uint64_t rss;                            // Resident set size in pages
    uint64_t start_time;                     // Process start time
    uint64_t utime;                          // User CPU time
    uint64_t stime;                          // System CPU time
    uint32_t num_sections;                   // Number of memory sections
    char exe_path[BEACON_PATH_MAX_STORED];   // Executable path
} BeaconProcessEntry;

// Fixed-size memory section entry
typedef struct BeaconSectionEntry {
    uint32_t pid;                            // Which process this belongs to
    uint64_t start_addr;                     // Start of memory region
    uint64_t end_addr;                       // End of memory region
    uint32_t permissions;                    // rwxp as bitfield
    uint64_t offset;                         // File offset
    uint32_t major;                          // Device major
    uint32_t minor;                          // Device minor
    uint64_t inode;
    char pathname[BEACON_PATH_MAX_STORED];   // Mapped file or [heap], [stack], etc.
} BeaconSectionEntry;

// Regular beacon page with tear detection (exactly 4096 bytes)
typedef struct BeaconPage {
    uint32_t magic;                          // BEACON_MAGIC
    uint32_t version_top;                    // Version number at top (for tear detection)
    uint32_t session_id;
    uint32_t category;                       // Which category this belongs to
    uint32_t category_index;                 // Index within the category
    uint32_t sequence;                       // Sequence number
    uint32_t data_size;                      // Valid data size in this page
    uint32_t reserved;                       // Padding/alignment
    uint8_t data[4060];                      // Actual data (4096 - 32 - 4 = 4060)
    uint32_t version_bottom;                 // Must match version_top for valid page
} BeaconPage;

// PID list page - specialized beacon page (exactly 4096 bytes)
typedef struct BeaconPIDListPage {
    uint32_t magic;                          // BEACON_MAGIC
    uint32_t version_top;                    // Version number at top
    uint32_t session_id;     
    uint32_t category;                       // BEACON_CATEGORY_PID
    uint32_t generation;                     // Which generation of PID list
    uint32_t total_pids;                     // Total PIDs in this generation
    uint32_t page_number;                    // Page N of M in this generation
    uint32_t pids_in_page;                   // Number of PIDs in this page
    uint32_t pids[BEACON_MAX_PIDS_PER_PAGE]; // Array of process IDs (1010 entries)
    uint32_t version_bottom;                 // Must match version_top
} BeaconPIDListPage;

// Camera control page (exactly 4096 bytes)
typedef struct BeaconCameraControlPage {
    uint32_t magic;                          // BEACON_MAGIC
    uint32_t version_top;
    uint32_t command;                        // BEACON_CAMERA_CMD_*
    uint32_t target_pid;                     // PID to focus on
    uint32_t status;                         // BEACON_CAMERA_STATUS_*
    uint32_t current_pid;                    // Currently watching PID
    uint8_t padding[4068];                   // Pad to 4096 bytes
    uint32_t version_bottom;
} BeaconCameraControlPage;

// Discovery page - first page of MASTER category (exactly 4096 bytes)
typedef struct BeaconDiscoveryPage {
    uint32_t beacon_magic;                   // BEACON_MAGIC at page boundary
    uint32_t discovery_magic;                // BEACON_DISCOVERY_MAGIC ("HayD")
    uint32_t version;
    uint32_t pid;
    
    // Category information
    struct {
        uint32_t base_offset;                // Offset from discovery page to this category
        uint32_t page_count;                 // Number of pages in this category
        uint32_t write_index;                // Current write position
        uint32_t sequence;                   // Sequence number for tear detection
    } categories[BEACON_NUM_CATEGORIES];
    
    uint8_t padding[4000];                   // Pad to 4096 bytes (4096 - 16 - 80)
} BeaconDiscoveryPage;

#pragma pack(pop)

// Compile-time size verification
#define BEACON_STATIC_ASSERT(cond, msg) typedef char static_assertion_##msg[(cond)?1:-1]

BEACON_STATIC_ASSERT(sizeof(BeaconPage) == 4096, BeaconPage_size);
BEACON_STATIC_ASSERT(sizeof(BeaconPIDListPage) == 4096, BeaconPIDListPage_size);
BEACON_STATIC_ASSERT(sizeof(BeaconCameraControlPage) == 4096, BeaconCameraControlPage_size);
BEACON_STATIC_ASSERT(sizeof(BeaconDiscoveryPage) == 4096, BeaconDiscoveryPage_size);
BEACON_STATIC_ASSERT(sizeof(BeaconProcessEntry) == 336, BeaconProcessEntry_size);

#ifdef __cplusplus
}
#endif

#endif // BEACON_PROTOCOL_H