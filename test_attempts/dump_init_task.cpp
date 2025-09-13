#include <iostream>
#include <iomanip>
#include <vector>
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
    
    MemoryBackend mem;
    if (!mem.AutoDetect()) {
        return 1;
    }
    
    // Read and dump first 512 bytes
    std::vector<uint8_t> data;
    if (!mem.Read(init_task_phys, 512, data)) {
        std::cerr << "Failed to read" << std::endl;
        return 1;
    }
    
    std::cout << "\nFirst 512 bytes of init_task:" << std::endl;
    std::cout << "Offset   00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  ASCII" << std::endl;
    std::cout << "-------  -----------------------  -----------------------  ----------------" << std::endl;
    
    for (size_t i = 0; i < data.size(); i += 16) {
        // Offset
        std::cout << std::hex << std::setw(6) << std::setfill('0') << i << "   ";
        
        // Hex bytes
        for (size_t j = 0; j < 16 && i+j < data.size(); j++) {
            if (j == 8) std::cout << " ";
            std::cout << std::setw(2) << std::setfill('0') << (int)data[i+j] << " ";
        }
        
        // ASCII
        std::cout << " ";
        for (size_t j = 0; j < 16 && i+j < data.size(); j++) {
            char c = data[i+j];
            std::cout << (c >= 32 && c < 127 ? c : '.');
        }
        std::cout << std::endl;
        
        // Look for non-zero 8-byte values (potential pointers)
        uint64_t val;
        memcpy(&val, data.data() + i, 8);
        if (val != 0 && (val & 0xffff000000000000) == 0xffff000000000000) {
            std::cout << "         ^-- Possible pointer: 0x" << std::hex << val << std::dec << std::endl;
        }
    }
    
    agent.Disconnect();
    return 0;
}