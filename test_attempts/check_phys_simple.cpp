#include <iostream>
#include <iomanip>
#include <sstream>
#include "guest_agent.h"
#include "memory_backend.h"

using namespace Haywire;

int main() {
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        return 1;
    }
    
    // Try dmesg for memory info
    std::string output;
    std::cout << "Checking dmesg for memory layout..." << std::endl;
    if (agent.ExecuteCommand("dmesg | grep -i 'memory:' | head -3", output)) {
        std::cout << output << std::endl;
    }
    
    // Look for kernel load address in dmesg
    if (agent.ExecuteCommand("dmesg | grep -i 'kernel code' | head -1", output)) {
        std::cout << "Kernel location: " << output << std::endl;
    }
    
    // Try to find Memory ranges
    if (agent.ExecuteCommand("dmesg | grep -E '(DRAM|RAM|Memory).*0x' | head -5", output)) {
        std::cout << "Memory ranges:\n" << output << std::endl;
    }
    
    // Get kernel virtual base from config
    if (agent.ExecuteCommand("grep CONFIG_PAGE_OFFSET /boot/config-* 2>/dev/null | head -1", output)) {
        std::cout << "Kernel PAGE_OFFSET: " << output << std::endl;
    }
    
    // Now let's scan physical memory for kernel signatures
    MemoryBackend mem;
    if (!mem.AutoDetect()) {
        std::cerr << "Failed to open memory backend" << std::endl;
        return 1;
    }
    
    std::cout << "\nScanning for kernel signatures in physical memory..." << std::endl;
    
    // Check some likely addresses
    uint64_t addrs[] = {
        0x40000000,  // 1GB (video memory as we found)
        0x80000000,  // 2GB  
        0xC0000000,  // 3GB
        0x100000000, // 4GB
        0x43709840,  // init_task if offset by 1GB
        0x3709840,   // init_task if at low memory
    };
    
    for (uint64_t addr : addrs) {
        std::vector<uint8_t> data;
        if (mem.Read(addr, 256, data) && data.size() == 256) {
            bool has_data = false;
            for (uint8_t b : data) {
                if (b != 0 && b != 0xFF) {
                    has_data = true;
                    break;
                }
            }
            
            if (has_data) {
                std::cout << "Data at 0x" << std::hex << addr << ": ";
                for (int i = 0; i < 16; i++) {
                    std::cout << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
                }
                
                // Check for text
                bool has_text = true;
                for (int i = 0; i < 16; i++) {
                    if (data[i] < 32 || data[i] > 126) {
                        has_text = false;
                        break;
                    }
                }
                if (has_text) {
                    std::cout << " \"";
                    for (int i = 0; i < 16; i++) {
                        std::cout << (char)data[i];
                    }
                    std::cout << "\"";
                }
                
                std::cout << std::endl;
            }
        }
    }
    
    agent.Disconnect();
    return 0;
}