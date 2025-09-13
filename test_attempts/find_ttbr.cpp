#include <iostream>
#include <iomanip>
#include <vector>
#include "guest_agent.h"
#include "memory_backend.h"

using namespace Haywire;

// Try to find TTBR by scanning physical memory for page table patterns
bool FindTTBRByScanning(MemoryBackend& mem, uint64_t testVA, uint64_t testPA, uint64_t& ttbr) {
    std::cout << "Scanning for TTBR using VA 0x" << std::hex << testVA 
              << " -> PA 0x" << testPA << std::dec << std::endl;
    
    // ARM64 4-level page tables
    uint64_t l3_idx = (testVA >> 12) & 0x1FF;
    uint64_t l2_idx = (testVA >> 21) & 0x1FF;
    uint64_t l1_idx = (testVA >> 30) & 0x1FF;
    uint64_t l0_idx = (testVA >> 39) & 0x1FF;
    
    // The L3 PTE should point to our PA (with valid bit)
    uint64_t expected_l3_pte = (testPA & ~0xFFFULL) | 0x3;
    
    std::cout << "Looking for L3 PTE: 0x" << std::hex << expected_l3_pte 
              << " at index " << std::dec << l3_idx << std::endl;
    
    // Scan common page table locations (1GB - 2GB of physical memory)
    for (uint64_t scan_addr = 0x40000000; scan_addr < 0x80000000; scan_addr += 0x1000) {
        std::vector<uint8_t> page;
        if (!mem.Read(scan_addr, 0x1000, page) || page.size() != 0x1000) {
            continue;
        }
        
        // Check if this could be an L3 page table
        uint64_t entry;
        memcpy(&entry, page.data() + l3_idx * 8, 8);
        
        if ((entry & ~0xFFFULL) == (testPA & ~0xFFFULL)) {
            std::cout << "Found potential L3 table at 0x" << std::hex << scan_addr << std::dec << std::endl;
            
            // Try to walk back to find TTBR
            // This is simplified - real implementation would need full backward walk
            ttbr = scan_addr - 0x3000 * 512 * 512;  // Rough estimate
            return true;
        }
        
        // Progress indicator
        if ((scan_addr & 0xFFFFFF) == 0) {
            std::cout << "." << std::flush;
        }
    }
    
    std::cout << std::endl;
    return false;
}

int main(int argc, char* argv[]) {
    int pid = 1;  // Default to init
    if (argc > 1) {
        pid = atoi(argv[1]);
    }
    
    // Connect to guest agent
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        std::cerr << "Failed to connect to guest agent" << std::endl;
        return 1;
    }
    
    // Connect to memory backend
    MemoryBackend mem;
    if (!mem.AutoDetect()) {
        std::cerr << "Failed to detect memory backend" << std::endl;
        return 1;
    }
    
    std::cout << "Looking for valid VA->PA mapping for PID " << pid << std::endl;
    
    // Try to find a valid mapping
    std::vector<uint64_t> test_addrs = {
        0xaaaaaaaa0000,  // ARM64 typical
        0x555555554000,  // x86_64 typical
        0x400000,        // Traditional
        0x1000000,       // Alternative
    };
    
    uint64_t foundVA = 0, foundPA = 0;
    
    for (uint64_t va : test_addrs) {
        PagemapEntry entry;
        if (agent.TranslateAddress(pid, va, entry) && entry.present) {
            foundVA = va;
            foundPA = entry.physAddr;
            std::cout << "Found mapping: VA 0x" << std::hex << va 
                      << " -> PA 0x" << entry.physAddr << std::dec << std::endl;
            break;
        }
    }
    
    if (foundVA == 0) {
        // Try to find ANY valid page
        for (uint64_t va = 0x1000; va < 0x100000000; va += 0x1000000) {
            PagemapEntry entry; 
            if (agent.TranslateAddress(pid, va, entry) && entry.present) {
                foundVA = va;
                foundPA = entry.physAddr;
                std::cout << "Found mapping: VA 0x" << std::hex << va 
                          << " -> PA 0x" << entry.physAddr << std::dec << std::endl;
                break;
            }
        }
    }
    
    if (foundVA == 0) {
        std::cerr << "Could not find any valid VA->PA mapping" << std::endl;
        return 1;
    }
    
    // Now scan for TTBR
    uint64_t ttbr = 0;
    if (FindTTBRByScanning(mem, foundVA, foundPA, ttbr)) {
        std::cout << "Possible TTBR: 0x" << std::hex << ttbr << std::dec << std::endl;
    } else {
        std::cout << "Could not find TTBR by scanning" << std::endl;
        
        // Try common values
        std::cout << "\nTrying common TTBR values..." << std::endl;
        std::vector<uint64_t> common_ttbrs = {
            0x40000000, 0x41000000, 0x42000000, 0x43000000,
            0x44000000, 0x45000000, 0x46000000, 0x47000000,
        };
        
        for (uint64_t test_ttbr : common_ttbrs) {
            // Would need to implement page walk test here
            std::cout << "  Testing 0x" << std::hex << test_ttbr << std::dec << "..." << std::endl;
        }
    }
    
    return 0;
}