#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>
#include "memory_backend.h"
#include "guest_agent.h"

namespace Haywire {

// Scan physical memory to find TTBR by looking for page table patterns
class TTBRScanner {
public:
    TTBRScanner(MemoryBackend* backend) : memory(backend) {}
    
    // Use a known VA->PA mapping to find the TTBR
    bool FindTTBRFromMapping(uint64_t va, uint64_t pa, uint64_t& ttbr) {
        std::cerr << "Searching for TTBR using VA 0x" << std::hex << va 
                  << " -> PA 0x" << pa << std::dec << std::endl;
        
        // ARM64 4KB pages, 4-level translation
        // Extract VA indices
        uint64_t l3_idx = (va >> 12) & 0x1FF;  // Bits 20:12
        uint64_t l2_idx = (va >> 21) & 0x1FF;  // Bits 29:21
        uint64_t l1_idx = (va >> 30) & 0x1FF;  // Bits 38:30
        uint64_t l0_idx = (va >> 39) & 0x1FF;  // Bits 47:39
        
        // The final L3 PTE should point to our PA
        uint64_t expected_l3_pte = (pa & ~0xFFFULL) | 0x3;  // Valid + AF bit
        
        std::cerr << "Looking for L3 PTE: 0x" << std::hex << expected_l3_pte 
                  << " at index " << std::dec << l3_idx << std::endl;
        
        // Scan physical memory for this PTE
        // Typically page tables are in the first few GB of physical memory
        uint64_t scanStart = 0x40000000;  // Start at 1GB
        uint64_t scanEnd   = 0x100000000; // Up to 4GB
        uint64_t scanStep  = 0x1000;      // Page size
        
        std::vector<uint64_t> l3_tables;
        
        // Find all L3 tables containing our PTE
        for (uint64_t paddr = scanStart; paddr < scanEnd; paddr += scanStep) {
            if (CheckForL3PTE(paddr, l3_idx, expected_l3_pte)) {
                l3_tables.push_back(paddr);
                std::cerr << "Found L3 table at PA 0x" << std::hex << paddr << std::dec << std::endl;
                
                // Try to walk back up to find the TTBR
                uint64_t foundTTBR = 0;
                if (WalkBackToTTBR(paddr, l2_idx, l1_idx, l0_idx, va, foundTTBR)) {
                    ttbr = foundTTBR;
                    return true;
                }
            }
            
            // Show progress every 256MB
            if ((paddr & 0xFFFFFFF) == 0) {
                std::cerr << "Scanned up to 0x" << std::hex << paddr << std::dec << "\r";
            }
        }
        
        std::cerr << std::endl;
        return false;
    }
    
    // Quick scan using multiple known mappings
    bool QuickFindTTBR(GuestAgent* agent, int pid, uint64_t& ttbr0, uint64_t& ttbr1) {
        // Get a few translations to work with
        std::vector<std::pair<uint64_t, uint64_t>> mappings;
        
        // Try common addresses
        uint64_t testVAs[] = {
            0x400000,     // Typical code start
            0x401000,     // Next page
            0x600000,     // Data segment
        };
        
        for (uint64_t va : testVAs) {
            PagemapEntry entry;
            if (agent->TranslateAddress(pid, va, entry) && entry.present) {
                mappings.push_back({va, entry.physAddr});
                std::cerr << "Known: VA 0x" << std::hex << va 
                          << " -> PA 0x" << entry.physAddr << std::dec << std::endl;
                
                // Try to find TTBR from this mapping
                if (FindTTBRFromMapping(va, entry.physAddr, ttbr0)) {
                    std::cerr << "Found TTBR0: 0x" << std::hex << ttbr0 << std::dec << std::endl;
                    
                    // For TTBR1, we'd need kernel addresses (0xffff...)
                    ttbr1 = 0;
                    return true;
                }
            }
        }
        
        return false;
    }
    
private:
    MemoryBackend* memory;
    static constexpr uint64_t PAGE_SIZE = 4096;
    
    bool CheckForL3PTE(uint64_t tableAddr, uint64_t index, uint64_t expectedPTE) {
        // Read the specific entry from the potential L3 table
        uint64_t entryAddr = tableAddr + index * 8;
        std::vector<uint8_t> data;
        
        if (!memory->ReadMemory(entryAddr, 8, data) || data.size() != 8) {
            return false;
        }
        
        uint64_t entry;
        memcpy(&entry, data.data(), 8);
        
        // Check if the PTE matches (ignoring attribute bits)
        return (entry & ~0xFFFULL) == (expectedPTE & ~0xFFFULL);
    }
    
    bool WalkBackToTTBR(uint64_t l3_table, uint64_t l2_idx, uint64_t l1_idx, 
                        uint64_t l0_idx, uint64_t originalVA, uint64_t& ttbr) {
        // Now we need to find the L2 table that points to this L3 table
        uint64_t expected_l2_pte = (l3_table & ~0xFFFULL) | 0x3;  // Table descriptor
        
        std::cerr << "Looking for L2 table with PTE 0x" << std::hex << expected_l2_pte
                  << " at index " << std::dec << l2_idx << std::endl;
        
        // Search for L2 table
        uint64_t scanStart = 0x40000000;
        uint64_t scanEnd   = 0x100000000;
        
        for (uint64_t l2_addr = scanStart; l2_addr < scanEnd; l2_addr += PAGE_SIZE) {
            if (CheckForL3PTE(l2_addr, l2_idx, expected_l2_pte)) {
                std::cerr << "Found L2 table at 0x" << std::hex << l2_addr << std::dec << std::endl;
                
                // Continue to L1
                uint64_t expected_l1_pte = (l2_addr & ~0xFFFULL) | 0x3;
                
                for (uint64_t l1_addr = scanStart; l1_addr < scanEnd; l1_addr += PAGE_SIZE) {
                    if (CheckForL3PTE(l1_addr, l1_idx, expected_l1_pte)) {
                        std::cerr << "Found L1 table at 0x" << std::hex << l1_addr << std::dec << std::endl;
                        
                        // Finally find L0 (TTBR)
                        uint64_t expected_l0_pte = (l1_addr & ~0xFFFULL) | 0x3;
                        
                        for (uint64_t l0_addr = scanStart; l0_addr < scanEnd; l0_addr += PAGE_SIZE) {
                            if (CheckForL3PTE(l0_addr, l0_idx, expected_l0_pte)) {
                                ttbr = l0_addr;
                                std::cerr << "Found TTBR at 0x" << std::hex << l0_addr << std::dec << std::endl;
                                
                                // Validate by trying to walk the page table
                                if (ValidateTTBR(ttbr, originalVA)) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        
        return false;
    }
    
    bool ValidateTTBR(uint64_t ttbr, uint64_t testVA) {
        // Do a simple validation - check if the TTBR has valid entries
        std::vector<uint8_t> data;
        if (!memory->ReadMemory(ttbr, PAGE_SIZE, data) || data.size() != PAGE_SIZE) {
            return false;
        }
        
        // Count valid entries
        int validCount = 0;
        for (size_t i = 0; i < 512; i++) {
            uint64_t entry;
            memcpy(&entry, data.data() + i * 8, 8);
            if (entry & 0x1) {  // Valid bit
                validCount++;
            }
        }
        
        std::cerr << "TTBR validation: " << validCount << "/512 valid entries" << std::endl;
        return validCount > 0;  // Should have at least some valid entries
    }
};

} // namespace Haywire