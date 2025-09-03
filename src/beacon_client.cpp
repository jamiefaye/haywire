#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <chrono>
#include <thread>
#include "beacon_map.h"

#define MAX_REQUEST_SLOTS 16
#define MAX_PROCS_PER_CHUNK 50

// Request types
enum RequestType {
    REQ_NONE = 0,
    REQ_LIST_PROCESSES = 1,
    REQ_GET_PROCESS_INFO = 2,
    REQ_CONTINUE_ITERATION = 3,
    REQ_CANCEL_ITERATION = 4
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
    char exe_path[256];
} __attribute__((packed));

// Request structure (256 bytes)
struct Request {
    uint32_t magic;
    uint32_t owner_pid;
    uint32_t sequence;
    uint32_t type;
    uint32_t iterator_id;
    uint32_t target_pid;
    uint64_t timestamp;
    uint8_t padding[232];
} __attribute__((packed));

// Response header (256 bytes)
struct ResponseHeader {
    uint32_t magic;
    uint32_t sequence;
    uint32_t status;
    uint32_t error_code;
    uint32_t items_count;
    uint32_t items_remaining;
    uint32_t iterator_id;
    uint8_t padding[228];
} __attribute__((packed));

class BeaconClient {
private:
    int fd;
    void* mapped_mem;
    size_t file_size;
    BeaconMap map;
    uint32_t sequence;
    
public:
    BeaconClient() : fd(-1), mapped_mem(nullptr), file_size(0), sequence(1) {}
    
    ~BeaconClient() {
        if (mapped_mem) munmap(mapped_mem, file_size);
        if (fd >= 0) close(fd);
    }
    
    bool open_memory_file(const char* path) {
        fd = open(path, O_RDWR);
        if (fd < 0) {
            perror("open memory file");
            return false;
        }
        
        struct stat st;
        if (fstat(fd, &st) < 0) {
            perror("fstat");
            return false;
        }
        
        file_size = st.st_size;
        mapped_mem = mmap(nullptr, file_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        if (mapped_mem == MAP_FAILED) {
            perror("mmap");
            return false;
        }
        
        printf("Mapped %zu MB of memory\n", file_size / 1024 / 1024);
        return true;
    }
    
    void scan_beacons() {
        printf("Scanning for beacons...\n");
        uint8_t* mem = (uint8_t*)mapped_mem;
        size_t beacons_found = 0;
        
        for (size_t offset = 0; offset < file_size; offset += PAGE_SIZE) {
            uint32_t* page = (uint32_t*)(mem + offset);
            if (page[0] == BEACON_MAGIC1 && page[1] == BEACON_MAGIC2) {
                uint32_t session_id = page[2];
                uint32_t protocol_ver = page[3];
                
                // Only add protocol v3 beacons (our new handler)
                if (protocol_ver == 3) {
                    map.add_beacon(offset, session_id, protocol_ver, 0);
                    beacons_found++;
                }
            }
        }
        
        printf("Found %zu protocol v3 beacons\n", beacons_found);
    }
    
    bool send_request(uint32_t session_id, RequestType type) {
        // Find beacon for this session
        auto beacons = map.find_by_session(session_id);
        if (beacons.empty()) {
            printf("No beacon found for session 0x%08X\n", session_id);
            return false;
        }
        
        auto* beacon = beacons[0];
        printf("Using beacon at 0x%08llX\n", beacon->phys_addr);
        
        // Find request and response areas
        uint8_t* mem = (uint8_t*)mapped_mem;
        Request* requests = (Request*)(mem + beacon->request_addr());
        ResponseHeader* responses = (ResponseHeader*)(mem + beacon->response_addr());
        
        // Find free slot
        int slot = -1;
        for (int i = 0; i < MAX_REQUEST_SLOTS; i++) {
            if (requests[i].magic == 0) {
                slot = i;
                break;
            }
        }
        
        if (slot < 0) {
            printf("No free request slots\n");
            return false;
        }
        
        // Send request
        memset(&requests[slot], 0, sizeof(Request));
        requests[slot].magic = 0x3142FACE;
        requests[slot].owner_pid = getpid();
        requests[slot].sequence = sequence++;
        requests[slot].type = type;
        requests[slot].timestamp = time(nullptr);
        
        printf("Sent request %u to slot %d\n", requests[slot].sequence, slot);
        
        // Wait for response
        for (int retry = 0; retry < 500; retry++) {
            if (responses[slot].magic == 0x3142FACE &&
                responses[slot].sequence == requests[slot].sequence - 1) {
                
                printf("Got response! Status: %u, Items: %u, Remaining: %u\n",
                       responses[slot].status, responses[slot].items_count,
                       responses[slot].items_remaining);
                
                if (responses[slot].items_count > 0) {
                    // Read process data
                    ProcessInfo* procs = (ProcessInfo*)(mem + beacon->data_addr() + slot * PAGE_SIZE * 16);
                    
                    printf("\nProcesses:\n");
                    for (uint32_t i = 0; i < responses[slot].items_count && i < 10; i++) {
                        printf("  PID %5u: %-16s (PPID %u)\n",
                               procs[i].pid, procs[i].name, procs[i].ppid);
                    }
                    
                    if (responses[slot].items_count > 10) {
                        printf("  ... and %u more\n", responses[slot].items_count - 10);
                    }
                }
                
                // Clear request to free slot
                requests[slot].magic = 0;
                return true;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        printf("Timeout waiting for response\n");
        requests[slot].magic = 0;
        return false;
    }
    
    void list_sessions() {
        printf("\n=== Active Sessions ===\n");
        std::unordered_map<uint32_t, size_t> session_counts;
        
        for (size_t i = 0; i < map.total_beacons(); i++) {
            auto* beacon = map.get_by_index(i);
            if (beacon && beacon->is_active) {
                session_counts[beacon->session_id]++;
            }
        }
        
        for (const auto& pair : session_counts) {
            printf("Session 0x%08X: %zu beacons\n", pair.first, pair.second);
        }
    }
    
    uint32_t find_v3_session() {
        for (size_t i = 0; i < map.total_beacons(); i++) {
            auto* beacon = map.get_by_index(i);
            if (beacon && beacon->is_active && beacon->protocol_ver == 3) {
                return beacon->session_id;
            }
        }
        return 0;
    }
};

int main() {
    BeaconClient client;
    
    if (!client.open_memory_file("/tmp/haywire-vm-mem")) {
        fprintf(stderr, "Failed to open memory file\n");
        return 1;
    }
    
    client.scan_beacons();
    client.list_sessions();
    
    // Try to list processes from first protocol v3 session
    printf("\n=== Testing Process List Request ===\n");
    
    uint32_t target_session = client.find_v3_session();
    
    if (target_session) {
        printf("Requesting process list from session 0x%08X\n", target_session);
        client.send_request(target_session, REQ_LIST_PROCESSES);
    } else {
        printf("No protocol v3 sessions found\n");
    }
    
    return 0;
}