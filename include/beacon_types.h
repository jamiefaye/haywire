#ifndef BEACON_TYPES_H
#define BEACON_TYPES_H

#include <stdint.h>

// Beacon class types - what kind of page this is
enum BeaconClass {
    BEACON_CLASS_INDEX        = 1,  // Discovery/index pages (read-only)
    BEACON_CLASS_REQUEST_RING = 2,  // Request circular buffer headers
    BEACON_CLASS_RESPONSE_RING = 3, // Response circular buffer headers  
    BEACON_CLASS_REQUEST_DATA = 4,  // Actual request message pages
    BEACON_CLASS_RESPONSE_DATA = 5, // Actual response message pages
    BEACON_CLASS_BULK_DATA    = 6,  // Large data transfers
    BEACON_CLASS_DIRTY_BITMAP = 7,  // Dirty page tracking
    BEACON_CLASS_STATISTICS   = 8,  // Performance counters
    BEACON_CLASS_LOG_BUFFER   = 9,  // Diagnostic logging
    BEACON_CLASS_MEMORY_MAP   = 10, // Guest physical memory map
};

// Standard beacon header - first 64 bytes of every beacon page
struct BeaconHeader {
    // Core identification (16 bytes)
    uint32_t magic1;         // 0x3142FACE
    uint32_t magic2;         // 0xCAFEBABE
    uint32_t session_id;     // Unique session identifier
    uint32_t beacon_class;   // BeaconClass enum value
    
    // Page information (16 bytes)
    uint32_t page_index;     // Index within this class (not global)
    uint32_t total_pages;    // Total pages in this class
    uint32_t protocol_ver;   // Protocol version
    uint32_t flags;          // Various flags
    
    // Timestamps (16 bytes)
    uint64_t created_time;   // When page was allocated
    uint64_t modified_time;  // Last modification
    
    // Extended info (16 bytes)
    uint32_t checksum;       // CRC32 of page content
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} __attribute__((packed));

// Master index beacon - special structure for BEACON_CLASS_INDEX
struct IndexBeacon {
    struct BeaconHeader header;
    
    // Index-specific data
    uint32_t num_classes;    // Number of different beacon classes
    uint32_t total_beacons;  // Total beacons across all classes
    
    // Class registry (up to 32 classes)
    struct {
        uint32_t beacon_class;
        uint32_t page_count;
        uint64_t first_page_addr;  // Physical address of first page
    } classes[32];
} __attribute__((packed));

#define BEACON_MAGIC1 0x3142FACE
#define BEACON_MAGIC2 0xCAFEBABE
#define PAGE_SIZE 4096

#endif // BEACON_TYPES_H