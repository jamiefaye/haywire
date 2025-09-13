#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>
#include "memory_backend.h"
#include "guest_agent.h"

using namespace Haywire;

// Convert kernel virtual address to physical
uint64_t virt_to_phys(uint64_t virt) {
    // ARM64 kernel virtual addresses: 0xffff800000000000 + physical
    if ((virt & 0xffff800000000000) == 0xffff800000000000) {
        return virt - 0xffff800000000000;
    }
    return virt;
}

int main() {
    // Step 1: Get init_task address from agent
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        std::cerr << "Failed to connect to agent" << std::endl;
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
    
    if (init_task_virt == 0) {
        std::cerr << "Could not find init_task" << std::endl;
        return 1;
    }
    
    uint64_t init_task_phys = virt_to_phys(init_task_virt);
    
    std::cout << "init_task:" << std::endl;
    std::cout << "  Virtual:  0x" << std::hex << init_task_virt << std::endl;
    std::cout << "  Physical: 0x" << init_task_phys << std::dec << std::endl;
    
    // Step 2: Connect to memory backend
    MemoryBackend mem;
    if (!mem.AutoDetect()) {
        std::cerr << "Failed to detect memory backend" << std::endl;
        return 1;
    }
    
    // Step 3: Read a chunk of init_task
    std::vector<uint8_t> task_data;
    size_t task_size = 0x1000;  // Read 4KB to start
    
    if (!mem.Read(init_task_phys, task_size, task_data)) {
        std::cerr << "Failed to read init_task from physical memory" << std::endl;
        return 1;
    }
    
    std::cout << "\nSearching for known patterns in init_task..." << std::endl;
    
    // Step 4: Search for known values to find offsets
    
    // A. Search for PID (should be 0 for init_task)
    std::cout << "\n1. Looking for PID=0 (32-bit value)..." << std::endl;
    for (size_t i = 0; i < task_size - 4; i += 4) {
        uint32_t val;
        memcpy(&val, task_data.data() + i, 4);
        if (val == 0) {
            // Could be PID, check if there's reasonable data around it
            if (i > 8 && i < task_size - 100) {
                std::cout << "  Possible PID at offset 0x" << std::hex << i << std::dec << std::endl;
            }
        }
    }
    
    // B. Search for "swapper" or "swapper/0" (process name)
    std::cout << "\n2. Looking for comm='swapper' string..." << std::endl;
    for (size_t i = 0; i < task_size - 16; i++) {
        if (memcmp(task_data.data() + i, "swapper", 7) == 0) {
            std::cout << "  Found 'swapper' at offset 0x" << std::hex << i << std::dec << std::endl;
            
            // Show what's around it
            char comm[17] = {0};
            memcpy(comm, task_data.data() + i, 16);
            std::cout << "  Full comm field: '" << comm << "'" << std::endl;
        }
    }
    
    // C. Search for linked list pointers (should point near init_task)
    std::cout << "\n3. Looking for tasks list pointers..." << std::endl;
    for (size_t i = 0; i < task_size - 16; i += 8) {
        uint64_t next, prev;
        memcpy(&next, task_data.data() + i, 8);
        memcpy(&prev, task_data.data() + i + 8, 8);
        
        // Check if these look like they could be list pointers
        // They should be kernel addresses near init_task
        if ((next & 0xffff800000000000) == 0xffff800000000000 &&
            (prev & 0xffff800000000000) == 0xffff800000000000) {
            
            // Check if they're within reasonable distance of init_task
            int64_t next_dist = (int64_t)(next - init_task_virt);
            int64_t prev_dist = (int64_t)(prev - init_task_virt);
            
            if (abs(next_dist) < 0x10000000 && abs(prev_dist) < 0x10000000) {
                std::cout << "  Possible list pointers at offset 0x" << std::hex << i << std::dec << std::endl;
                std::cout << "    next: 0x" << std::hex << next << " (distance: " << next_dist << ")" << std::endl;
                std::cout << "    prev: 0x" << std::hex << prev << " (distance: " << prev_dist << ")" << std::endl;
            }
        }
    }
    
    // D. Try common offsets from different kernel versions
    std::cout << "\n4. Checking common offset patterns..." << std::endl;
    
    struct KnownOffsets {
        const char* kernel;
        uint32_t pid;
        uint32_t comm;
        uint32_t tasks;
        uint32_t mm;
    };
    
    KnownOffsets common[] = {
        {"5.x typical", 0x398, 0x550, 0x2F8, 0x3A0},
        {"6.x typical", 0x3A0, 0x560, 0x308, 0x3B0},
        {"Alternative", 0x3B8, 0x540, 0x2E8, 0x390},
    };
    
    for (const auto& offsets : common) {
        std::cout << "\n  Testing " << offsets.kernel << " offsets:" << std::endl;
        
        // Check PID
        if (offsets.pid < task_size - 4) {
            uint32_t pid;
            memcpy(&pid, task_data.data() + offsets.pid, 4);
            std::cout << "    PID at 0x" << std::hex << offsets.pid << " = " << std::dec << pid;
            if (pid == 0) std::cout << " ✓";
            std::cout << std::endl;
        }
        
        // Check comm
        if (offsets.comm < task_size - 16) {
            char comm[17] = {0};
            memcpy(comm, task_data.data() + offsets.comm, 16);
            std::cout << "    comm at 0x" << std::hex << offsets.comm << " = '" << comm << "'";
            if (strstr(comm, "swapper")) std::cout << " ✓";
            std::cout << std::endl;
        }
        
        // Check tasks list
        if (offsets.tasks < task_size - 16) {
            uint64_t next;
            memcpy(&next, task_data.data() + offsets.tasks, 8);
            std::cout << "    tasks.next at 0x" << std::hex << offsets.tasks << " = 0x" << next;
            if ((next & 0xffff800000000000) == 0xffff800000000000) std::cout << " (looks valid)";
            std::cout << std::endl;
        }
    }
    
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "We can read init_task from physical memory!" << std::endl;
    std::cout << "Next step: Use the offsets to walk the process list." << std::endl;
    std::cout << "Once we find the right offsets for kernel " << output << std::endl;
    std::cout << "we can go completely agent-free!" << std::endl;
    
    agent.Disconnect();
    return 0;
}