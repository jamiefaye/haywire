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

#include "shm_protocol_v2.h"

#define MEMORY_FILE "/tmp/haywire-vm-mem"
#define PAGE_SIZE 4096

class HaywireClientV2 {
private:
    int fd;
    void* mapped_mem;
    size_t mapped_size;
    uint64_t beacon_offset;
    uint32_t my_pid;
    uint32_t sequence_number;
    
    Request* requests;
    ResponseHeader* response_headers;
    CircularBuffer* response_buffer;
    
public:
    HaywireClientV2() : fd(-1), mapped_mem(nullptr), mapped_size(0),
                        beacon_offset(0), sequence_number(1) {
        my_pid = getpid();
    }
    
    ~HaywireClientV2() {
        if (mapped_mem) {
            munmap(mapped_mem, mapped_size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    
    bool connect() {
        // First find beacon offset
        if (!findBeaconOffset()) {
            fprintf(stderr, "Failed to find beacon\n");
            return false;
        }
        
        // Map shared memory at beacon offset
        fd = open(MEMORY_FILE, O_RDWR);
        if (fd < 0) {
            perror("open memory file");
            return false;
        }
        
        // Map 8MB from beacon offset (smaller in v2)
        mapped_size = 8 * 1024 * 1024;
        mapped_mem = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, beacon_offset);
        if (mapped_mem == MAP_FAILED) {
            perror("mmap");
            return false;
        }
        
        // Set up pointers (matching v2 layout)
        requests = (Request*)((uint8_t*)mapped_mem + PAGE_SIZE);  // Page 1
        response_headers = (ResponseHeader*)((uint8_t*)mapped_mem + PAGE_SIZE * 5);  // Page 5
        response_buffer = (CircularBuffer*)((uint8_t*)mapped_mem + PAGE_SIZE * 9);  // Page 9
        
        printf("Connected to shared memory at offset 0x%lX\n", beacon_offset);
        printf("Haywire Client PID: %u\n", my_pid);
        printf("Protocol Version: 2 (circular buffer)\n");
        
        // Verify circular buffer is initialized
        if (response_buffer->magic != 0x3142FACE) {
            fprintf(stderr, "Warning: Circular buffer not initialized\n");
        }
        
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
                printf("Found beacon at offset 0x%lX\n", offset);
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
        
        printf("  Claimed slot %d for request\n", slot);
        
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
    
    ResponseHeader* waitForResponse(int slot, int timeout_ms = 5000) {
        if (slot < 0 || slot >= MAX_REQUEST_SLOTS) {
            return nullptr;
        }
        
        auto start = std::chrono::steady_clock::now();
        ResponseHeader* resp = &response_headers[slot];
        
        while (true) {
            if (resp->magic == 0x3142FACE && resp->sequence == requests[slot].sequence) {
                // Response ready!
                printf("  Got response in slot %d (offset %u, size %u)\n", 
                       slot, resp->buffer_offset, resp->buffer_size);
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
        response_headers[slot].magic = 0;  // Clear response header
    }
    
    void* getResponseData(ResponseHeader* resp) {
        if (!resp || resp->buffer_size == 0) {
            return nullptr;
        }
        return circular_ptr(response_buffer, resp->buffer_offset);
    }
    
    std::vector<ProcessInfo> listAllProcesses() {
        std::vector<ProcessInfo> all_processes;
        
        printf("\nRequesting process list...\n");
        int slot = sendRequest(REQ_LIST_PROCESSES);
        if (slot < 0) {
            return all_processes;
        }
        
        ResponseHeader* resp = waitForResponse(slot);
        if (!resp) {
            releaseSlot(slot);
            return all_processes;
        }
        
        uint32_t iterator_id = resp->iterator_id;
        uint32_t total_received = 0;
        
        while (true) {
            // Get data from circular buffer
            ProcessInfo* procs = (ProcessInfo*)getResponseData(resp);
            if (procs) {
                // Copy processes from circular buffer
                for (uint32_t i = 0; i < resp->items_count; i++) {
                    all_processes.push_back(procs[i]);
                }
                total_received += resp->items_count;
            }
            
            printf("  Received chunk: %u processes (total: %u), %u remaining\n", 
                   resp->items_count, total_received, resp->items_remaining);
            
            if (resp->status == RESP_COMPLETE) {
                printf("  List complete!\n");
                break;
            }
            
            if (resp->status == RESP_MORE_DATA) {
                // Release current slot
                releaseSlot(slot);
                
                // Continue iteration
                slot = sendRequest(REQ_CONTINUE_ITERATION, 0, iterator_id);
                if (slot < 0) {
                    fprintf(stderr, "Failed to continue iteration\n");
                    break;
                }
                
                resp = waitForResponse(slot);
                if (!resp) {
                    releaseSlot(slot);
                    break;
                }
            } else {
                fprintf(stderr, "Error status: %u\n", resp->status);
                break;
            }
        }
        
        releaseSlot(slot);
        return all_processes;
    }
};

int main(int argc, char** argv) {
    printf("=====================================\n");
    printf("Haywire Client V2 (PID %u)\n", getpid());
    printf("Using circular buffer for responses\n");
    printf("=====================================\n\n");
    
    HaywireClientV2 client;
    
    if (!client.connect()) {
        fprintf(stderr, "Failed to connect to shared memory\n");
        return 1;
    }
    
    // Test: List all processes
    auto processes = client.listAllProcesses();
    
    printf("\nProcess List Summary:\n");
    printf("Total processes found: %zu\n", processes.size());
    
    if (processes.size() > 0) {
        printf("\nFirst 10 processes:\n");
        printf("%-8s %-8s %-50s %s\n", "PID", "PPID", "NAME", "PATH");
        printf("--------------------------------------------------------------------\n");
        
        for (size_t i = 0; i < processes.size() && i < 10; i++) {
            const auto& proc = processes[i];
            printf("%-8u %-8u %-50s %s\n", 
                   proc.pid, proc.ppid, proc.name, proc.exe_path);
        }
        
        if (processes.size() > 10) {
            printf("... and %zu more processes\n", processes.size() - 10);
        }
    }
    
    // Stress test with multiple rapid requests
    if (argc > 1 && strcmp(argv[1], "--stress") == 0) {
        printf("\n=====================================\n");
        printf("Stress Test: Rapid Sequential Requests\n");
        printf("=====================================\n\n");
        
        for (int i = 0; i < 5; i++) {
            printf("Iteration %d: ", i+1);
            fflush(stdout);
            
            auto start = std::chrono::steady_clock::now();
            auto procs = client.listAllProcesses();
            auto elapsed = std::chrono::steady_clock::now() - start;
            
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            printf("Got %zu processes in %ld ms\n", procs.size(), ms);
            
            usleep(100000);  // 100ms between iterations
        }
    }
    
    return 0;
}