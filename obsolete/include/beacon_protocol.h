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

// Beacon categories
#define BEACON_CATEGORY_MASTER      0
#define BEACON_CATEGORY_PID         1
#define BEACON_CATEGORY_CAMERA1     2
#define BEACON_CATEGORY_CAMERA2     3
#define BEACON_NUM_CATEGORIES       4

// Pages per category (must match companion allocation)
#define BEACON_MASTER_PAGES      1     // Just the discovery page
#define BEACON_PID_PAGES         32    // PID list snapshots (32KB, ~32k PIDs)
#define BEACON_CAMERA1_PAGES     200   // Camera watching specific PID
#define BEACON_CAMERA2_PAGES     200   // Camera watching another PID

// Process and path limits
#define BEACON_PROCESS_NAME_LEN  16    // Same as kernel TASK_COMM_LEN
#define BEACON_PATH_MAX_STORED   256   // Truncated path length

// PID entry structure (48 bytes)
typedef struct BeaconPIDEntry {
    uint32_t type;      // ENTRY_PID (0)
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    uint64_t rss_kb;
    char comm[16];
    char state;
    uint8_t padding[3];
} __attribute__((packed)) BeaconPIDEntry;

// PID list configuration  
#define BEACON_MAX_PIDS_PER_PAGE 84  // Exactly fits: 36 header + (84 * 48) entries + 4 version_bottom = 4096
#define BEACON_PID_GENERATIONS   10    // Keep 10 generations of PID lists


// Camera status
#define BEACON_CAMERA_STATUS_IDLE       0
#define BEACON_CAMERA_STATUS_SWITCHING  1
#define BEACON_CAMERA_STATUS_ACTIVE     2

// Entry types for camera data stream
#define BEACON_ENTRY_TYPE_SECTION  0x01  // Section entry (96 bytes)
#define BEACON_ENTRY_TYPE_PTE      0x02  // PTE entry (24 bytes)  
#define BEACON_ENTRY_TYPE_END      0xFF  // End of entries marker

// Section entry for camera data (96 bytes)
typedef struct BeaconSectionEntry {
    uint8_t type;           // BEACON_ENTRY_TYPE_SECTION
    uint8_t reserved[3];
    uint32_t pid;
    uint64_t va_start;      // Start of valid VA range
    uint64_t va_end;        // End of valid VA range
    uint32_t perms;         // r/w/x/p flags
    uint8_t padding[4];
    char path[64];          // File path or [heap], [stack], etc.
} __attribute__((packed)) BeaconSectionEntry;

// PTE entry for camera data - only allocated pages (24 bytes)
typedef struct BeaconPTEEntry {
    uint8_t type;           // BEACON_ENTRY_TYPE_PTE
    uint8_t reserved[3];
    uint32_t flags;         // Page flags
    uint64_t va;            // Virtual address (page-aligned)
    uint64_t pa;            // Physical address (non-zero)
} __attribute__((packed)) BeaconPTEEntry;

/*
 * All structures are packed to ensure exact layout.
 * Every structure that represents a beacon page MUST be exactly 4096 bytes.
 */

#pragma pack(push, 1)

// Regular beacon page with tear detection (exactly 4096 bytes)
typedef struct BeaconPage {
    uint32_t magic;                          // BEACON_MAGIC
    uint32_t version_top;                    // Version number at top (for tear detection)
    uint32_t session_id;
    uint32_t category;                       // Which category this belongs to
    uint32_t category_index;                 // Index within the category
    uint32_t timestamp;                      // Unix timestamp from discovery page
    uint32_t sequence;                       // Sequence number
    uint32_t data_size;                      // Valid data size in this page
    uint8_t data[4060];                      // Actual data (4096 - 32 - 4 = 4060)
    uint32_t version_bottom;                 // Must match version_top for valid page
} BeaconPage;

// PID list page - specialized beacon page (exactly 4096 bytes)
typedef struct BeaconPIDListPage {
    uint32_t magic;                          // BEACON_MAGIC
    uint32_t version_top;                    // Version number at top
    uint32_t session_id;     
    uint32_t category;                       // BEACON_CATEGORY_PID
    uint32_t category_index;                 // Page index within PID category (was page_number)
    uint32_t timestamp;                      // Unix timestamp from discovery page
    uint32_t generation;                     // Which generation of PID list
    uint32_t total_pids;                     // Total PIDs in this generation
    uint32_t pids_in_page;                   // Number of PIDs in this page
    BeaconPIDEntry entries[BEACON_MAX_PIDS_PER_PAGE]; // Array of PID entries (84 entries)
    uint8_t padding[24];                     // Padding: 36 + 4032 + 24 + 4 = 4096
    uint32_t version_bottom;                 // Must match version_top
} BeaconPIDListPage;

// Camera control page (exactly 4096 bytes)
typedef struct BeaconCameraControlPage {
    uint32_t magic;                          // BEACON_MAGIC
    uint32_t version_top;                    // Version number at top
    uint32_t session_id;                     // Session ID (companion PID)
    uint32_t category;                       // BEACON_CATEGORY_CAMERA1 or CAMERA2
    uint32_t category_index;                 // Always 0 (control page)
    uint32_t timestamp;                      // Unix timestamp from discovery page
    uint32_t target_pid;                     // PID to focus on
    uint32_t status;                         // BEACON_CAMERA_STATUS_*
    uint32_t current_pid;                    // Currently watching PID
    uint8_t padding[4056];                   // Pad to 4096 bytes
    uint32_t version_bottom;                 // Must match version_top
} BeaconCameraControlPage;

// Camera data page - stream format (exactly 4096 bytes)
typedef struct BeaconCameraDataPage {
    uint32_t magic;                          // BEACON_MAGIC
    uint32_t version_top;                    // Version number at top
    uint32_t session_id;                     // Session ID (companion PID)
    uint32_t category;                       // BEACON_CATEGORY_CAMERA1 or CAMERA2
    uint32_t category_index;                 // 1-199 for data pages
    uint32_t timestamp;                      // Unix timestamp
    uint32_t target_pid;                     // Which PID this data is for
    uint16_t entry_count;                    // Number of entries in this page
    uint16_t continuation;                   // 0=last page, 1=more pages follow
    uint8_t data[4060];                      // Stream of mixed section/PTE entries (4096 - 32 - 4 = 4060)
    uint32_t version_bottom;                 // Must match version_top
} BeaconCameraDataPage;

// Discovery page - first page of MASTER category (exactly 4096 bytes)
typedef struct BeaconDiscoveryPage {
    uint32_t magic;                          // BEACON_MAGIC
    uint32_t version_top;                    // Version number at top
    uint32_t session_id;                     // Session ID (companion PID)
    uint32_t category;                       // BEACON_CATEGORY_MASTER (0)
    uint32_t category_index;                 // Always 0 (discovery page)
    uint32_t timestamp;                      // Unix timestamp when created
    
    // Category information
    struct {
        uint32_t base_offset;                // Offset from discovery page to this category
        uint32_t page_count;                 // Number of pages in this category
        uint32_t write_index;                // Current write position
        uint32_t sequence;                   // Sequence number for tear detection
    } categories[BEACON_NUM_CATEGORIES];
    
    uint8_t padding[4004];                   // Pad to 4096 bytes (4096 - 24 - 64 - 4)
    uint32_t version_bottom;                 // Must match version_top
} BeaconDiscoveryPage;

#pragma pack(pop)

// Compile-time size verification
#define BEACON_STATIC_ASSERT(cond, msg) typedef char static_assertion_##msg[(cond)?1:-1]

BEACON_STATIC_ASSERT(sizeof(BeaconPage) == 4096, BeaconPage_size);
BEACON_STATIC_ASSERT(sizeof(BeaconPIDListPage) == 4096, BeaconPIDListPage_size);
BEACON_STATIC_ASSERT(sizeof(BeaconCameraControlPage) == 4096, BeaconCameraControlPage_size);
BEACON_STATIC_ASSERT(sizeof(BeaconCameraDataPage) == 4096, BeaconCameraDataPage_size);
BEACON_STATIC_ASSERT(sizeof(BeaconDiscoveryPage) == 4096, BeaconDiscoveryPage_size);

#ifdef __cplusplus
}
#endif

#endif // BEACON_PROTOCOL_H