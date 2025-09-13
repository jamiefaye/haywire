#include <iostream>
#include <iomanip>
#include <vector>
#include "memory_backend.h"

using namespace Haywire;

int main() {
    MemoryBackend mem;
    if (!mem.AutoDetect()) {
        std::cerr << "Failed to detect memory backend" << std::endl;
        return 1;
    }
    
    std::cout << "Testing physical memory reads at various addresses..." << std::endl;
    
    // Try different physical addresses
    uint64_t test_addrs[] = {
        0x40000000,  // 1GB - common kernel location
        0x80000000,  // 2GB
        0x83709840,  // Our supposed init_task
        0x100000000, // 4GB
        0x40100000,  // Near kernel start
    };
    
    for (uint64_t addr : test_addrs) {
        std::vector<uint8_t> data;
        if (mem.Read(addr, 64, data) && data.size() == 64) {
            // Check if it's all zeros
            bool all_zero = true;
            for (uint8_t b : data) {
                if (b != 0) {
                    all_zero = false;
                    break;
                }
            }
            
            std::cout << "0x" << std::hex << std::setw(10) << addr << ": ";
            if (all_zero) {
                std::cout << "All zeros" << std::endl;
            } else {
                std::cout << "Has data: ";
                for (int i = 0; i < 16; i++) {
                    std::cout << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
                }
                std::cout << "..." << std::endl;
            }
        } else {
            std::cout << "0x" << std::hex << std::setw(10) << addr << ": Read failed" << std::endl;
        }
    }
    
    // Also check if the kernel text is where we expect
    std::cout << "\nLooking for kernel signatures around 1-2GB..." << std::endl;
    
    // Scan for Linux version string or other signatures
    for (uint64_t addr = 0x40000000; addr < 0x50000000; addr += 0x100000) { // Every 1MB
        std::vector<uint8_t> data;
        if (mem.Read(addr, 256, data) && data.size() == 256) {
            // Look for "Linux" string
            for (size_t i = 0; i < data.size() - 5; i++) {
                if (memcmp(data.data() + i, "Linux", 5) == 0) {
                    std::cout << "Found 'Linux' at 0x" << std::hex << (addr + i) << std::endl;
                    // Show context
                    std::cout << "  Context: ";
                    for (size_t j = i; j < std::min(i + 32, data.size()); j++) {
                        char c = data[j];
                        std::cout << (c >= 32 && c < 127 ? c : '.');
                    }
                    std::cout << std::endl;
                    goto next_addr;
                }
            }
        }
        next_addr:;
    }
    
    return 0;
}