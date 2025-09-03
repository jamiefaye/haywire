#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <vector>
#include <chrono>

#include "shm_protocol.h"

class HaywireClient {
private:
    int fd;
    void* mapped_mem;
    size_t mapped_size;
    uint64_t beacon_offset;
    uint32_t my_pid;
    uint32_t sequence_number;
    
    Request* requests;
    Response* responses;
    
public:
    HaywireClient() : fd(-1), mapped_mem(nullptr), mapped_size(0),
                      beacon_offset(0), sequence_number(1) {
        my_pid = getpid();
    }
    
    ~HaywireClient() {
        if (mapped_mem) {
            munmap(mapped_mem, mapped_size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    
    bool connect() {
        // First find beacons
        if (!findBeaconOffset()) {
            return false;
        }
        
        // Map the shared memory at beacon offset
        fd = open(MEMORY_FILE, O_RDWR);
        if (fd < 0) {
            perror("open memory file");
            return false;
        }
        
        // Map 64MB from beacon offset
        mapped_size = 64 * 1024 * 1024;
        mapped_mem = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, beacon_offset);
        if (mapped_mem == MAP_FAILED) {
            perror("mmap");
            return false;
        }
        
        // Set up pointers
        requests = (Request*)((uint8_t*)mapped_mem + PAGE_SIZE);
        responses = (Response*)((uint8_t*)mapped_mem + PAGE_SIZE * 17);
        
        printf("Connected to shared memory at offset 0x%lX\n", beacon_offset);
        printf("Haywire PID: %u\n", my_pid);
        return true;
    }
    
    bool findBeaconOffset() {
        int fd = open(MEMORY_FILE, O_RDONLY);
        if (fd < 0) {
            return false;
        }
        
        struct stat st;
        fstat(fd, &st);
        size_t file_size = st.st_size;
        
        void* mem = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mem == MAP_FAILED) {
            close(fd);
            return false;
        }
        
        // Scan for beacon on page boundaries
        uint8_t* ptr = (uint8_t*)mem;
        for (size_t offset = 0; offset < file_size; offset += PAGE_SIZE) {
            uint32_t* magic = (uint32_t*)(ptr + offset);
            if (magic[0] == 0x3142FACE && magic[1] == 0xCAFEBABE) {
                beacon_offset = offset;
                munmap(mem, file_size);
                close(fd);
                return true;
            }
        }
        
        munmap(mem, file_size);
        close(fd);
        return false;
    }
    
    int sendRequest(RequestType type, uint32_t target_pid = 0, uint32_t iterator_id = 0) {
        // Claim a slot
        int slot = claim_request_slot(requests, my_pid);
        if (slot < 0) {
            fprintf(stderr, "No request slots available\n");
            return -1;
        }
        
        // Fill request
        Request* req = &requests[slot];
        req->sequence = sequence_number++;
        req->type = type;
        req->target_pid = target_pid;
        req->iterator_id = iterator_id;
        
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        req->timestamp = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        
        // Set magic last to signal request is ready
        __sync_synchronize();
        req->magic = 0x3142FACE;
        
        return slot;
    }
    
    Response* waitForResponse(int slot, int timeout_ms = 5000) {
        if (slot < 0 || slot >= MAX_REQUEST_SLOTS) {
            return nullptr;
        }
        
        auto start = std::chrono::steady_clock::now();
        Response* resp = &responses[slot];
        
        while (true) {
            if (resp->magic == 0x3142FACE && resp->sequence == requests[slot].sequence) {
                // Response ready!
                return resp;
            }
            
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
                fprintf(stderr, "Request timeout\n");
                break;
            }
            
            usleep(1000);  // 1ms poll
        }
        
        // Release slot on timeout
        release_request_slot(requests, slot, my_pid);
        return nullptr;
    }
    
    void releaseSlot(int slot) {
        release_request_slot(requests, slot, my_pid);
        responses[slot].magic = 0;  // Clear response
    }
    
    std::vector<ProcessInfo> listAllProcesses() {
        std::vector<ProcessInfo> all_processes;
        
        printf("Requesting process list...\n");
        int slot = sendRequest(REQ_LIST_PROCESSES);
        if (slot < 0) {
            return all_processes;
        }
        
        Response* resp = waitForResponse(slot);
        if (!resp) {
            releaseSlot(slot);
            return all_processes;
        }
        
        uint32_t iterator_id = resp->iterator_id;
        
        while (true) {
            // Copy processes from response
            for (uint32_t i = 0; i < resp->items_count; i++) {
                all_processes.push_back(resp->data.processes[i]);
            }
            
            printf("Got %u processes, %u remaining\n", 
                   resp->items_count, resp->items_remaining);
            
            if (resp->status == RESP_COMPLETE) {
                break;
            }
            
            if (resp->status == RESP_MORE_DATA) {
                // Release current slot
                releaseSlot(slot);
                
                // Continue iteration
                slot = sendRequest(REQ_CONTINUE_ITERATION, 0, iterator_id);
                if (slot < 0) {
                    break;
                }
                
                resp = waitForResponse(slot);
                if (!resp) {
                    releaseSlot(slot);
                    break;
                }
            } else {
                fprintf(stderr, "Error: %s\n", resp->data.error_message);
                break;
            }
        }
        
        releaseSlot(slot);
        return all_processes;
    }
};

int main(int argc, char** argv) {
    printf("Haywire Client (PID %u)\n", getpid());
    printf("==================\n\n");
    
    HaywireClient client;
    
    if (!client.connect()) {
        fprintf(stderr, "Failed to connect to shared memory\n");
        return 1;
    }
    
    // Test: List all processes
    auto processes = client.listAllProcesses();
    
    printf("\nFound %zu processes:\n", processes.size());
    printf("%-8s %-8s %-50s %s\n", "PID", "PPID", "NAME", "PATH");
    printf("--------------------------------------------------------------------\n");
    
    for (const auto& proc : processes) {
        printf("%-8u %-8u %-50s %s\n", 
               proc.pid, proc.ppid, proc.name, proc.exe_path);
    }
    
    // Test multiple concurrent clients
    if (argc > 1 && strcmp(argv[1], "--stress") == 0) {
        printf("\n\nStress testing with rapid requests...\n");
        for (int i = 0; i < 10; i++) {
            auto procs = client.listAllProcesses();
            printf("Iteration %d: Got %zu processes\n", i+1, procs.size());
            usleep(100000);
        }
    }
    
    return 0;
}