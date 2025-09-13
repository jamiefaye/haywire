#include <iostream>
#include <iomanip>
#include <vector>
#include "memory_backend.h"

using namespace Haywire;

int main() {
    MemoryBackend mem;
    if (!mem.AutoDetect()) {
        return 1;
    }
    
    std::cout << "Scanning physical memory for kernel signatures..." << std::endl;
    std::cout << "Looking for: ARM64 kernel magic, Linux version string, etc." << std::endl << std::endl;
    
    // Scan in 16MB chunks
    for (uint64_t addr = 0; addr < 0x100000000; addr += 0x1000000) { // Up to 4GB
        std::vector<uint8_t> data;
        if (!mem.Read(addr, 4096, data) || data.size() != 4096) {
            continue;
        }
        
        // Check if this chunk has any data at all
        bool has_data = false;
        for (uint8_t b : data) {
            if (b != 0) {
                has_data = true;
                break;
            }
        }
        
        if (has_data) {
            std::cout << "Data found at 0x" << std::hex << addr << ": ";
            
            // Show first 32 bytes
            for (int i = 0; i < 32 && i < data.size(); i++) {
                std::cout << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
            }
            std::cout << std::endl;
            
            // Look for specific patterns
            
            // ARM64 kernel image header (ARM64 boot protocol)
            if (data.size() >= 4 && data[0] == 0x4D && data[1] == 0x5A) { // "MZ"
                std::cout << "  ^-- Possible ARM64 kernel image header (MZ signature)" << std::endl;
            }
            
            // Linux version string
            for (size_t i = 0; i < data.size() - 10; i++) {
                if (memcmp(data.data() + i, "Linux vers", 10) == 0) {
                    std::cout << "  ^-- Found 'Linux version' string at offset " << i << std::endl;
                    break;
                }
            }
            
            // Look for "ARM64" or "aarch64"
            for (size_t i = 0; i < data.size() - 6; i++) {
                if (memcmp(data.data() + i, "aarch64", 7) == 0 ||
                    memcmp(data.data() + i, "ARM64", 5) == 0) {
                    std::cout << "  ^-- Found ARM64/aarch64 string" << std::endl;
                    break;
                }
            }
            
            // ELF header (7F 45 4C 46)
            if (data.size() >= 4 && data[0] == 0x7F && data[1] == 0x45 && 
                data[2] == 0x4C && data[3] == 0x46) {
                std::cout << "  ^-- ELF header found!" << std::endl;
            }
        }
    }
    
    std::cout << "\nScan complete." << std::endl;
    return 0;
}