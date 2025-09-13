#include <iostream>
#include <iomanip>
#include <chrono>
#include "guest_agent.h"
#include "arm64_page_walker.h"
#include "memory_backend.h"

using namespace Haywire;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <pid> [test_va]" << std::endl;
        std::cerr << "Example: " << argv[0] << " 1234" << std::endl;
        std::cerr << "Example: " << argv[0] << " 1234 0x400000" << std::endl;
        return 1;
    }
    
    int pid = atoi(argv[1]);
    uint64_t testVA = 0x555555554000;  // More typical for modern ASLR
    if (argc > 2) {
        testVA = strtoull(argv[2], nullptr, 0);
    }
    
    // Connect to guest agent
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        std::cerr << "Failed to connect to guest agent" << std::endl;
        return 1;
    }
    std::cout << "Connected to guest agent" << std::endl;
    
    // Get TTBR values
    GuestAgent::TTBRValues ttbr;
    if (!agent.GetTTBR(pid, ttbr)) {
        std::cerr << "Failed to get TTBR values, will use defaults" << std::endl;
        ttbr.ttbr0_el1 = 0x41000000;  // Common default
        ttbr.ttbr1_el1 = 0;
        ttbr.valid = true;
    }
    
    std::cout << "\nTTBR values:" << std::endl;
    std::cout << "  TTBR0: 0x" << std::hex << ttbr.ttbr0_el1 << std::endl;
    std::cout << "  TTBR1: 0x" << ttbr.ttbr1_el1 << std::endl;
    std::cout << "  TCR:   0x" << ttbr.tcr_el1 << std::dec << std::endl;
    
    // Connect to memory backend
    MemoryBackend memBackend;
    if (!memBackend.AutoDetect()) {
        std::cerr << "Failed to detect memory backend" << std::endl;
        return 1;
    }
    std::cout << "Connected to memory backend" << std::endl;
    
    // Create page walker
    ARM64PageWalker walker(&memBackend);
    walker.SetPageTableBase(ttbr.ttbr0_el1, ttbr.ttbr1_el1);
    
    // Test single translation - try multiple addresses to find one that works
    std::cout << "\n=== Single Address Translation Test ===" << std::endl;
    
    // Try multiple common addresses
    std::vector<uint64_t> testAddresses = {
        testVA,
        0x555555554000,  // Typical code start with ASLR  
        0x7ffff7a00000,  // Common shared library region
        0x7fffffffe000,  // Stack region
        0xaaaaaaaa0000,  // ARM64 typical code start
        0xffff80000000,  // ARM64 kernel region (may not be accessible)
    };
    
    uint64_t workingVA = 0;
    for (uint64_t va : testAddresses) {
        PagemapEntry entry;
        if (agent.TranslateAddress(pid, va, entry) && entry.present) {
            workingVA = va;
            std::cout << "Found working VA: 0x" << std::hex << va 
                      << " -> PA 0x" << entry.physAddr << std::dec << std::endl;
            break;
        }
    }
    
    if (workingVA == 0) {
        std::cout << "No valid addresses found, using default" << std::endl;
        workingVA = testVA;
    }
    
    std::cout << "Testing VA 0x" << std::hex << workingVA << std::dec << std::endl;
    testVA = workingVA;
    
    // Method 1: Slow guest agent
    auto agentStart = std::chrono::high_resolution_clock::now();
    PagemapEntry entry;
    bool agentOk = agent.TranslateAddress(pid, testVA, entry);
    auto agentEnd = std::chrono::high_resolution_clock::now();
    auto agentTime = std::chrono::duration_cast<std::chrono::microseconds>(agentEnd - agentStart);
    
    // Method 2: Fast page walker
    auto walkStart = std::chrono::high_resolution_clock::now();
    uint64_t walkPA = walker.TranslateAddress(testVA);
    auto walkEnd = std::chrono::high_resolution_clock::now();
    auto walkTime = std::chrono::duration_cast<std::chrono::microseconds>(walkEnd - walkStart);
    
    // Display results
    std::cout << "\nResults:" << std::endl;
    if (agentOk && entry.present) {
        std::cout << "  Guest Agent: VA 0x" << std::hex << testVA 
                  << " -> PA 0x" << entry.physAddr << std::dec
                  << " (Time: " << agentTime.count() << " µs)" << std::endl;
    } else {
        std::cout << "  Guest Agent: Page not present" << std::endl;
    }
    
    if (walkPA != 0) {
        std::cout << "  Page Walker: VA 0x" << std::hex << testVA 
                  << " -> PA 0x" << walkPA << std::dec
                  << " (Time: " << walkTime.count() << " µs)" << std::endl;
    } else {
        std::cout << "  Page Walker: Page not mapped" << std::endl;
    }
    
    // Check if they match
    if (agentOk && entry.present && walkPA != 0) {
        if (entry.physAddr == walkPA) {
            std::cout << "\n✓ Results MATCH!" << std::endl;
        } else {
            std::cout << "\n✗ Results DIFFER!" << std::endl;
            std::cout << "  Difference: 0x" << std::hex 
                      << (int64_t)(entry.physAddr - walkPA) << std::dec << std::endl;
        }
        
        float speedup = (float)agentTime.count() / walkTime.count();
        std::cout << "  Speedup: " << speedup << "x faster" << std::endl;
    }
    
    // Bulk test
    std::cout << "\n=== Bulk Translation Test (256 pages) ===" << std::endl;
    
    size_t numPages = 256;
    uint64_t bulkVA = 0x400000;
    
    // Agent bulk
    auto bulkAgentStart = std::chrono::high_resolution_clock::now();
    std::vector<PagemapEntry> agentEntries;
    agent.TranslateRange(pid, bulkVA, numPages * 4096, agentEntries);
    auto bulkAgentEnd = std::chrono::high_resolution_clock::now();
    auto bulkAgentTime = std::chrono::duration_cast<std::chrono::milliseconds>(bulkAgentEnd - bulkAgentStart);
    
    // Walker bulk
    auto bulkWalkStart = std::chrono::high_resolution_clock::now();
    std::vector<uint64_t> walkEntries;
    walker.TranslateRange(bulkVA, numPages, walkEntries);
    auto bulkWalkEnd = std::chrono::high_resolution_clock::now();
    auto bulkWalkTime = std::chrono::duration_cast<std::chrono::milliseconds>(bulkWalkEnd - bulkWalkStart);
    
    std::cout << "Guest Agent: " << bulkAgentTime.count() << " ms" << std::endl;
    std::cout << "Page Walker: " << bulkWalkTime.count() << " ms" << std::endl;
    
    if (bulkWalkTime.count() > 0) {
        float bulkSpeedup = (float)bulkAgentTime.count() / bulkWalkTime.count();
        std::cout << "Speedup: " << bulkSpeedup << "x faster" << std::endl;
    }
    
    // Validate some entries
    int matches = 0, mismatches = 0;
    for (size_t i = 0; i < std::min(agentEntries.size(), walkEntries.size()); i++) {
        if (agentEntries[i].physAddr == walkEntries[i]) {
            matches++;
        } else if (agentEntries[i].present && walkEntries[i] != 0) {
            mismatches++;
            if (mismatches <= 3) {
                std::cout << "  Mismatch at page " << i << ": agent=0x" << std::hex 
                          << agentEntries[i].physAddr << " walker=0x" << walkEntries[i] 
                          << std::dec << std::endl;
            }
        }
    }
    
    std::cout << "\nValidation: " << matches << " matches, " << mismatches << " mismatches" << std::endl;
    
    return 0;
}