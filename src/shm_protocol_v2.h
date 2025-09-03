#ifndef SHM_PROTOCOL_V2_H
#define SHM_PROTOCOL_V2_H

#include <stdint.h>
#include <stddef.h>

// Protocol constants
#define SHM_PROTOCOL_VERSION 2
#define MAX_REQUEST_SLOTS 16
#define MAX_ITERATORS 8
#define MAX_PROCS_PER_CHUNK 50
#define MAX_PATH_LENGTH 256
#define RESPONSE_BUFFER_SIZE (4 * 1024 * 1024)  // 4MB circular buffer

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
    RESP_MORE_DATA = 3,
    RESP_COMPLETE = 4
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

// Request structure - kept small (256 bytes)
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
} __attribute__((packed));

// Response header - points into circular buffer
struct ResponseHeader {
    uint32_t magic;          // 0x3142FACE when ready
    uint32_t sequence;       // Matches request sequence
    uint32_t status;         // ResponseStatus
    uint32_t error_code;     // If status == RESP_ERROR
    uint32_t buffer_offset;  // Offset in circular buffer
    uint32_t buffer_size;    // Size of response data
    uint32_t iterator_id;    // For chunked responses
    uint32_t items_count;    // Number of items
    uint32_t items_remaining;// Items left in iterator
    uint32_t reserved[7];    // Pad to 64 bytes
} __attribute__((packed));

// Circular buffer manager
struct CircularBuffer {
    uint32_t magic;          // 0x3142FACE
    uint32_t write_offset;   // Where companion writes next
    uint32_t wrap_counter;   // Increments on wrap
    uint32_t lock;           // Simple spinlock for companion
    uint8_t padding[48];     // Pad to 64 bytes
    uint8_t data[RESPONSE_BUFFER_SIZE - 64];
} __attribute__((packed));

// Iterator state (companion-side only)
struct IteratorState {
    uint32_t iterator_id;
    uint32_t owner_pid;
    uint32_t request_type;
    uint32_t position;
    uint32_t total_items;
    uint64_t last_access;
    uint8_t data[256];       // Iterator-specific state
} __attribute__((packed));

// Complete shared memory layout v2
struct SharedMemoryLayoutV2 {
    // Page 0: Beacon (4096 bytes)
    struct PageBeacon {
        uint32_t magic1;        // 0x3142FACE
        uint32_t magic2;        // 0xCAFEBABE
        uint32_t session_id;
        uint32_t protocol_version;  // = 2
        uint64_t timestamp;
        uint32_t process_count;
        uint32_t update_counter;
        uint32_t magic3;        // 0xFEEDFACE
        uint32_t magic4;        // 0xBAADF00D
        char hostname[64];
        uint8_t padding[3960];  // Rest of page
    } beacon;
    
    // Pages 1-4: Request slots (16 slots × 256 bytes = 4096 bytes)
    Request requests[MAX_REQUEST_SLOTS];
    
    // Pages 5-8: Response headers (16 slots × 64 bytes = 1024 bytes)
    ResponseHeader response_headers[MAX_REQUEST_SLOTS];
    uint8_t response_padding[3072];  // Pad to 4096
    
    // Pages 9-1032: Circular response buffer (4MB)
    CircularBuffer response_buffer;
    
    // Page 1033: Iterator table (companion internal use)
    struct {
        uint32_t magic;
        uint32_t active_count;
        uint32_t next_id;
        uint32_t reserved;
        IteratorState iterators[MAX_ITERATORS];
    } iterator_table;
    
} __attribute__((packed));

// Helper functions for circular buffer
static inline uint32_t circular_alloc(CircularBuffer* buf, uint32_t size) {
    // Simple allocation - companion is single-threaded
    uint32_t offset = buf->write_offset;
    uint32_t new_offset = offset + size;
    
    // Handle wrap
    if (new_offset >= RESPONSE_BUFFER_SIZE - 64) {
        buf->wrap_counter++;
        buf->write_offset = size;  // Skip header area
        return 0;  // Start of data area
    }
    
    buf->write_offset = new_offset;
    return offset;
}

static inline void* circular_ptr(CircularBuffer* buf, uint32_t offset) {
    return &buf->data[offset];
}

// Client-side helpers
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

// Companion-side lock helpers (single-threaded, but for safety)
static inline void circular_lock(CircularBuffer* buf) {
    while (__sync_lock_test_and_set(&buf->lock, 1)) {
        // Spin
    }
}

static inline void circular_unlock(CircularBuffer* buf) {
    __sync_lock_release(&buf->lock);
}

#endif // SHM_PROTOCOL_V2_H