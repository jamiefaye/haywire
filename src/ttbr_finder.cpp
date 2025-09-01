#include <iostream>
#include <iomanip>
#include <vector>
#include <map>
#include <algorithm>
#include "guest_agent.h"
#include "memory_backend.h"

namespace Haywire {

class TTBRFinder {
public:
    TTBRFinder(MemoryBackend* backend, GuestAgent* agent) 
        : memory(backend), agent(agent) {}
    
    // Find TTBR by using known VA->PA mappings to locate page tables
    bool FindTTBR(int pid, uint64_t& ttbr0, uint64_t& ttbr1) {
        std::cerr << "Finding TTBR for PID " << pid << " using page table patterns..." << std::endl;
        
        // Step 1: Get a few known VA->PA translations using the slow method
        std::vector<std::pair<uint64_t, uint64_t>> knownMappings;
        if (!GetKnownMappings(pid, knownMappings)) {
            return false;
        }
        
        // Step 2: For each known mapping, try to find page table entries pointing to it
        std::map<uint64_t, int> candidateTTBRs;
        
        for (const auto& mapping : knownMappings) {
            uint64_t va = mapping.first;
            uint64_t pa = mapping.second;
            
            // Find potential page table pages that contain this PA
            FindPageTableCandidates(va, pa, candidateTTBRs);
        }
        
        // Step 3: The most frequent candidate is likely the TTBR
        if (candidateTTBRs.empty()) {
            std::cerr << "No TTBR candidates found" << std::endl;
            return false;
        }
        
        // Find the most common candidate
        auto best = std::max_element(candidateTTBRs.begin(), candidateTTBRs.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        
        ttbr0 = best->first;
        ttbr1 = 0;  // Would need kernel mappings to find TTBR1
        
        std::cerr << "Found probable TTBR0: 0x" << std::hex << ttbr0 
                  << " (confidence: " << std::dec << best->second << " matches)" << std::endl;
        
        // Step 4: Validate by checking if it looks like a page table
        return ValidatePageTable(ttbr0);
    }
    
private:
    MemoryBackend* memory;
    GuestAgent* agent;
    
    // ARM64 page table constants
    static constexpr uint64_t PAGE_SIZE = 4096;
    static constexpr uint64_t DESC_VALID = 1ULL << 0;
    static constexpr uint64_t DESC_TABLE = 1ULL << 1;
    
    bool GetKnownMappings(int pid, std::vector<std::pair<uint64_t, uint64_t>>& mappings) {
        // Get a diverse set of addresses to sample
        std::vector<uint64_t> testAddresses = {
            0x400000,         // Typical code start
            0x600000,         // More code/data
            0x7fff00000000,   // Stack area
            0x7f0000000000,   // Shared libraries
        };
        
        std::cerr << "Getting known VA->PA mappings..." << std::endl;
        
        for (uint64_t va : testAddresses) {
            PagemapEntry entry;
            if (agent->TranslateAddress(pid, va, entry) && entry.present) {
                mappings.push_back({va, entry.physAddr});
                std::cerr << "  VA 0x" << std::hex << va 
                          << " -> PA 0x" << entry.physAddr << std::dec << std::endl;
            }
        }
        
        if (mappings.size() < 2) {
            std::cerr << "Not enough valid mappings found" << std::endl;
            return false;
        }
        
        return true;
    }
    
    void FindPageTableCandidates(uint64_t va, uint64_t pa, 
                                 std::map<uint64_t, int>& candidates) {
        // For ARM64 4-level page tables with 4KB pages
        // We need to reverse-engineer where the page table entries would be
        
        // Extract indices from VA
        uint64_t l0_idx = (va >> 39) & 0x1FF;
        uint64_t l1_idx = (va >> 30) & 0x1FF;
        uint64_t l2_idx = (va >> 21) & 0x1FF;
        uint64_t l3_idx = (va >> 12) & 0x1FF;
        
        // The L3 PTE should contain our PA (aligned to page)
        uint64_t expected_l3_pte = (pa & ~0xFFFULL) | DESC_VALID;
        
        // Scan physical memory for pages containing this PTE
        // We'll scan a reasonable range of physical memory
        uint64_t scanStart = 0x40000000;  // Start at 1GB
        uint64_t scanEnd = 0x80000000;    // End at 2GB
        uint64_t scanStep = PAGE_SIZE;
        
        std::cerr << "Scanning for L3 PTE containing PA 0x" << std::hex << pa << std::endl;
        
        for (uint64_t paddr = scanStart; paddr < scanEnd; paddr += scanStep) {
            // Read a page worth of data
            std::vector<uint8_t> pageData;
            if (!memory->ReadMemory(paddr, PAGE_SIZE, pageData)) {
                continue;
            }
            
            // Check each 8-byte entry in the page
            for (size_t offset = 0; offset < PAGE_SIZE; offset += 8) {
                uint64_t entry;
                memcpy(&entry, pageData.data() + offset, 8);
                
                // Check if this looks like our PTE
                if ((entry & ~0xFFFULL) == (pa & ~0xFFFULL) && (entry & DESC_VALID)) {
                    // This could be an L3 page table containing our mapping
                    // The L3 table would be at paddr
                    uint64_t l3_table = paddr;
                    
                    // Now work backwards to find L2, L1, L0
                    // The L2 entry pointing to this L3 table would be at some L2 table
                    // For now, just record this as a potential page table
                    
                    // Try to find the L2 table that points to this L3
                    FindL2Table(l3_table, l2_idx, l1_idx, l0_idx, candidates);
                }
            }
        }
    }
    
    void FindL2Table(uint64_t l3_table, uint64_t l2_idx, uint64_t l1_idx, 
                     uint64_t l0_idx, std::map<uint64_t, int>& candidates) {
        // Look for an L2 table entry pointing to our L3 table
        uint64_t expected_l2_pte = (l3_table & ~0xFFFULL) | DESC_TABLE | DESC_VALID;
        
        // Scan for pages containing this L2 PTE
        uint64_t scanStart = 0x40000000;
        uint64_t scanEnd = 0x80000000;
        
        for (uint64_t paddr = scanStart; paddr < scanEnd; paddr += PAGE_SIZE) {
            std::vector<uint8_t> pageData;
            if (!memory->ReadMemory(paddr, PAGE_SIZE, pageData)) {
                continue;
            }
            
            // Check if this page contains our expected L2 PTE at the right index
            if (l2_idx * 8 < PAGE_SIZE) {
                uint64_t entry;
                memcpy(&entry, pageData.data() + l2_idx * 8, 8);
                
                if ((entry & ~0xFFFULL) == (l3_table & ~0xFFFULL)) {
                    // Found potential L2 table
                    // Continue up the chain to find L1 and L0
                    // For now, just assume this could lead to a valid TTBR
                    
                    // Estimate TTBR (would need full chain validation)
                    // For simplicity, record this as a candidate
                    candidates[paddr - l0_idx * 512 * 512 * 8]++;
                }
            }
        }
    }
    
    bool ValidatePageTable(uint64_t ttbr) {
        // Read the page table and check if it looks valid
        std::vector<uint8_t> pageData;
        if (!memory->ReadMemory(ttbr & ~0xFFFULL, PAGE_SIZE, pageData)) {
            return false;
        }
        
        // Count valid entries
        int validEntries = 0;
        int tableEntries = 0;
        
        for (size_t i = 0; i < 512; i++) {
            uint64_t entry;
            memcpy(&entry, pageData.data() + i * 8, 8);
            
            if (entry & DESC_VALID) {
                validEntries++;
                if (entry & DESC_TABLE) {
                    tableEntries++;
                }
            }
        }
        
        std::cerr << "Page table validation: " << validEntries << " valid entries, "
                  << tableEntries << " table entries" << std::endl;
        
        // A reasonable page table should have some valid entries
        return validEntries > 0 && tableEntries > 0;
    }
};

} // namespace Haywire