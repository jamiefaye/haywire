#include <iostream>
#include <iomanip>
#include <vector>
#include <set>
#include <cstring>
#include "memory_backend.h"
#include "guest_agent.h"

using namespace Haywire;

uint64_t virt_to_phys(uint64_t virt) {
    if ((virt & 0xffff800000000000) == 0xffff800000000000) {
        return virt - 0xffff800000000000;
    }
    return virt;
}

int main() {
    // Get init_task address
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        return 1;
    }
    
    std::string output;
    uint64_t init_task_virt = 0;
    
    if (agent.ExecuteCommand("grep ' init_task$' /proc/kallsyms", output)) {
        std::stringstream ss(output);
        std::string addr_str;
        ss >> addr_str;
        init_task_virt = std::stoull(addr_str, nullptr, 16);
    }
    
    uint64_t init_task_phys = virt_to_phys(init_task_virt);
    
    std::cout << "init_task at 0x" << std::hex << init_task_virt 
              << " (phys: 0x" << init_task_phys << ")" << std::dec << std::endl;
    
    // Connect to memory
    MemoryBackend mem;
    if (!mem.AutoDetect()) {
        return 1;
    }
    
    // Read init_task
    std::vector<uint8_t> task_data;
    size_t task_size = 0x2000;  // Read 8KB
    
    if (!mem.Read(init_task_phys, task_size, task_data)) {
        std::cerr << "Failed to read init_task" << std::endl;
        return 1;
    }
    
    std::cout << "\nSearching for adjacent pointer pairs (next/prev pattern)..." << std::endl;
    std::cout << "Looking for pointers that:" << std::endl;
    std::cout << "  1. Are adjacent (8 bytes apart)" << std::endl;
    std::cout << "  2. Look like kernel addresses (0xffff...)" << std::endl;
    std::cout << "  3. Point to similar addresses (likely same list)" << std::endl;
    std::cout << std::endl;
    
    // Look for adjacent pointer pairs
    for (size_t offset = 0; offset < task_size - 16; offset += 8) {
        uint64_t ptr1, ptr2;
        memcpy(&ptr1, task_data.data() + offset, 8);
        memcpy(&ptr2, task_data.data() + offset + 8, 8);
        
        // Check if both look like kernel pointers
        if ((ptr1 & 0xffff000000000000) == 0xffff000000000000 &&
            (ptr2 & 0xffff000000000000) == 0xffff000000000000) {
            
            // Check if they're reasonably close to each other (same list)
            int64_t diff = (int64_t)(ptr1 - ptr2);
            
            // List nodes are typically within a few MB of each other
            if (abs(diff) < 0x10000000) {  // Within 256MB
                std::cout << "Found pointer pair at offset 0x" << std::hex << offset << ":" << std::endl;
                std::cout << "  ptr1: 0x" << ptr1 << std::endl;
                std::cout << "  ptr2: 0x" << ptr2 << std::endl;
                std::cout << "  diff: " << std::dec << (diff/1024) << " KB" << std::endl;
                
                // Extra validation: for a circular list, following the pointers should loop back
                // Let's check if these could be list pointers
                uint64_t ptr1_phys = virt_to_phys(ptr1);
                uint64_t ptr2_phys = virt_to_phys(ptr2);
                
                // Try to read what ptr1 points to
                std::vector<uint8_t> target_data;
                if (mem.Read(ptr1_phys, 16, target_data) && target_data.size() == 16) {
                    uint64_t target_next, target_prev;
                    memcpy(&target_next, target_data.data(), 8);
                    memcpy(&target_prev, target_data.data() + 8, 8);
                    
                    // If this is a doubly-linked list, target_prev should point back near init_task
                    int64_t back_diff = (int64_t)(target_prev - init_task_virt);
                    
                    if (abs(back_diff) < 0x10000) {  // Within 64KB of init_task
                        std::cout << "  *** LIKELY LIST POINTERS! ***" << std::endl;
                        std::cout << "  Target's prev points back near init_task (diff: " 
                                  << std::hex << back_diff << ")" << std::endl;
                        
                        // This offset is probably tasks.next/tasks.prev!
                        std::cout << "  ==> tasks list offset: 0x" << std::hex << offset << std::dec << std::endl;
                    }
                }
                
                std::cout << std::endl;
            }
        }
    }
    
    // Also look for the special case where next == prev (single item list)
    std::cout << "\nLooking for self-referential pointers (single item list)..." << std::endl;
    for (size_t offset = 0; offset < task_size - 16; offset += 8) {
        uint64_t ptr1, ptr2;
        memcpy(&ptr1, task_data.data() + offset, 8);
        memcpy(&ptr2, task_data.data() + offset + 8, 8);
        
        // Check if both point to near init_task (self-referential)
        if ((ptr1 & 0xffff000000000000) == 0xffff000000000000) {
            int64_t diff1 = (int64_t)(ptr1 - init_task_virt);
            int64_t diff2 = (int64_t)(ptr2 - init_task_virt);
            
            // If both point within the task_struct itself
            if (abs(diff1) < 0x2000 && abs(diff2) < 0x2000 && 
                std::abs(diff1 - (int64_t)offset) < 16 && std::abs(diff2 - (int64_t)offset - 8) < 16) {
                std::cout << "Found self-referential pointers at offset 0x" << std::hex << offset << std::endl;
                std::cout << "  ptr1 offset from init_task: 0x" << diff1 << std::endl;
                std::cout << "  ptr2 offset from init_task: 0x" << diff2 << std::endl;
                std::cout << "  ==> Possible empty list at offset 0x" << offset << std::dec << std::endl;
            }
        }
    }
    
    agent.Disconnect();
    return 0;
}