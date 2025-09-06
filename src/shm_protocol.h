#ifndef SHM_PROTOCOL_H
#define SHM_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

// Protocol constants
#define SHM_PROTOCOL_VERSION 1
#define MAX_REQUEST_SLOTS 16
#define MAX_ITERATORS 8
#define MAX_PROCS_PER_CHUNK 50
#define MAX_PATH_LENGTH 256

// Request types
enum RequestType {
    REQ_NONE = 0,
    REQ_LIST_PROCESSES = 1,
    REQ_GET_PROCESS_INFO = 2,
    REQ_CONTINUE_ITERATION = 3,
    REQ_CANCEL_ITERATION = 4,
    REQ_GET_MEMORY_MAP = 5,
    REQ_READ_MEMORY = 6
};

// Response status
enum ResponseStatus {
    RESP_PENDING = 0,
    RESP_SUCCESS = 1,
    RESP_ERROR = 2,
    RESP_MORE_DATA = 3,  // Iterator has more data
    RESP_COMPLETE = 4    // Iterator complete
};

// Process info structure
struct ProcessInfo {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    uint64_t start_time;
    uint64_t cpu_time;
    uint64_t memory_kb;
    char name[64];
    char exe_path[MAX_PATH_LENGTH];
} __attribute__((packed));

// Iterator state
struct IteratorState {
    uint32_t iterator_id;
    uint32_t owner_pid;      // Haywire PID that owns this iterator
    uint32_t request_type;   // What kind of iteration
    uint32_t position;       // Current position
    uint32_t total_items;    // Total items to iterate
    uint64_t last_access;    // For LRU replacement
    uint8_t data[256];       // Iterator-specific state
} __attribute__((packed));

// Request structure (cache line aligned)
struct Request {
    // Header (64 bytes)
    uint32_t magic;          // 0x3142FACE for valid request
    uint32_t owner_pid;      // Haywire PID (for claiming)
    uint32_t sequence;       // Request sequence number
    uint32_t type;           // RequestType
    uint32_t iterator_id;    // For continue/cancel requests
    uint32_t target_pid;     // For specific process queries
    uint64_t timestamp;      // When request was made
    uint8_t padding1[32];    // Pad to 64 bytes
    
    // Request data (192 bytes)
    union {
        struct {
            uint32_t flags;
            uint32_t max_results;
        } list_processes;
        
        struct {
            uint64_t address;
            uint32_t size;
        } read_memory;
        
        uint8_t raw[192];
    } data;
    
    uint8_t padding2[64];    // Pad to 256 bytes total
} __attribute__((packed));

// Response structure (fits in remaining page space)
struct Response {
    // Header (64 bytes)
    uint32_t magic;          // 0x3142FACE for valid response
    uint32_t sequence;       // Matches request sequence
    uint32_t status;         // ResponseStatus
    uint32_t error_code;     // If status == RESP_ERROR
    uint32_t iterator_id;    // For chunked responses
    uint32_t items_count;    // Number of items in this response
    uint32_t items_remaining;// Items left in iterator
    uint32_t reserved;
    uint8_t padding1[32];    // Pad to 64 bytes
    
    // Response data (3776 bytes - to fit with Request in one page)
    union {
        ProcessInfo processes[MAX_PROCS_PER_CHUNK];
        uint8_t memory_data[3776];
        char error_message[256];
        uint8_t raw[3776];
    } data;
    
    uint8_t padding2[256];   // Pad to 4096 bytes total
} __attribute__((packed));

// Beacon page placeholder (defined elsewhere)
struct PageBeacon {
    uint8_t data[4096];
} __attribute__((packed));

// Complete shared memory layout
struct SharedMemoryLayout {
    // Page 0: Beacon (4096 bytes)
    struct PageBeacon beacon;
    
    // Pages 1-16: Request slots (256 bytes each, 16 per page)
    Request requests[MAX_REQUEST_SLOTS];
    
    // Pages 17-32: Response slots (4096 bytes each)
    Response responses[MAX_REQUEST_SLOTS];
    
    // Page 33: Iterator table
    struct {
        uint32_t magic;      // 0x3142FACE
        uint32_t version;    // Protocol version
        uint32_t active_count;
        uint32_t reserved;
        IteratorState iterators[MAX_ITERATORS];
    } iterator_table;
    
    // Remaining pages: Reserved for future use
} __attribute__((packed));

// Helper functions
static inline int claim_request_slot(Request* slots, uint32_t my_pid) {
    for (int i = 0; i < MAX_REQUEST_SLOTS; i++) {
        if (__sync_bool_compare_and_swap(&slots[i].owner_pid, 0, my_pid)) {
            slots[i].magic = 0x3142FACE;
            return i;
        }
    }
    return -1;
}

static inline void release_request_slot(Request* slots, int slot, uint32_t my_pid) {
    if (slot >= 0 && slot < MAX_REQUEST_SLOTS) {
        if (slots[slot].owner_pid == my_pid) {
            slots[slot].magic = 0;
            __sync_synchronize();
            slots[slot].owner_pid = 0;
        }
    }
}

#endif // SHM_PROTOCOL_H