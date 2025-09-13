#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include "guest_agent.h"
#include "arm64_page_walker.h"
#include "memory_backend.h"
#include "qemu_connection.h"

using namespace Haywire;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <pid>" << std::endl;
        return 1;
    }
    
    int pid = atoi(argv[1]);
    
    // Connect to guest agent
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        std::cerr << "Failed to connect to guest agent" << std::endl;
        return 1;
    }
    
    // Get TTBR values
    GuestAgent::TTBRValues ttbr;
    if (!agent.GetTTBR(pid, ttbr)) {
        std::cerr << "Failed to get TTBR values for PID " << pid << std::endl;
        return 1;
    }
    
    std::cout << "Got TTBR values:" << std::endl;
    std::cout << "  TTBR0: 0x" << std::hex << ttbr.ttbr0_el1 << std::endl;
    std::cout << "  TTBR1: 0x" << ttbr.ttbr1_el1 << std::endl;
    std::cout << "  TCR:   0x" << ttbr.tcr_el1 << std::dec << std::endl;
    
    // Connect to QEMU for physical memory access
    QemuConnection qemu;
    if (!qemu.Connect("localhost", 7777)) {
        std::cerr << "Failed to connect to QEMU" << std::endl;
        return 1;
    }
    
    // Create page walker
    ARM64PageWalker walker(&qemu);
    walker.SetPageTableBase(ttbr.ttbr0_el1, ttbr.ttbr1_el1);
    
    // Test some addresses and compare results
    std::vector<uint64_t> testAddresses = {
        0x400000,        // Code segment (usually low memory)
        0x7fffffffe000,  // Stack (high user memory)
        0x7f8000000000,  // Shared libraries
        0xffff80000000   // Kernel space (if accessible)
    };
    
    std::cout << "\nComparing translation methods:" << std::endl;
    std::cout << "VA               | Guest Agent PA   | Direct Walk PA   | Match? | Agent Time | Walk Time" << std::endl;
    std::cout << "-----------------|------------------|------------------|--------|------------|----------" << std::endl;
    
    for (uint64_t va : testAddresses) {
        // Method 1: Guest agent (slow)
        PagemapEntry entry;
        auto agentStart = std::chrono::high_resolution_clock::now();
        bool agentSuccess = agent.TranslateAddress(pid, va, entry);
        auto agentEnd = std::chrono::high_resolution_clock::now();
        auto agentTime = std::chrono::duration_cast<std::chrono::microseconds>(agentEnd - agentStart);
        
        // Method 2: Direct page walk (fast)
        auto walkStart = std::chrono::high_resolution_clock::now();
        uint64_t walkPA = walker.TranslateAddress(va);
        auto walkEnd = std::chrono::high_resolution_clock::now();
        auto walkTime = std::chrono::duration_cast<std::chrono::microseconds>(walkEnd - walkStart);
        
        // Compare results
        uint64_t agentPA = agentSuccess ? entry.physAddr : 0;
        bool match = (agentPA == walkPA);
        
        std::cout << std::hex << std::setw(16) << va << " | ";
        std::cout << std::setw(16) << agentPA << " | ";
        std::cout << std::setw(16) << walkPA << " | ";
        std::cout << (match ? "  YES  " : "  NO   ") << " | ";
        std::cout << std::dec << std::setw(8) << agentTime.count() << "us | ";
        std::cout << std::setw(8) << walkTime.count() << "us" << std::endl;
    }
    
    // Benchmark bulk translation
    std::cout << "\nBulk translation benchmark (1024 consecutive pages):" << std::endl;
    
    uint64_t bulkStart = 0x400000;
    size_t numPages = 1024;
    
    // Guest agent bulk
    auto agentBulkStart = std::chrono::high_resolution_clock::now();
    std::vector<PagemapEntry> agentEntries;
    agent.TranslateRange(pid, bulkStart, numPages * 4096, agentEntries);
    auto agentBulkEnd = std::chrono::high_resolution_clock::now();
    auto agentBulkTime = std::chrono::duration_cast<std::chrono::milliseconds>(agentBulkEnd - agentBulkStart);
    
    // Direct walk bulk
    auto walkBulkStart = std::chrono::high_resolution_clock::now();
    std::vector<uint64_t> walkAddrs;
    walker.TranslateRange(bulkStart, numPages, walkAddrs);
    auto walkBulkEnd = std::chrono::high_resolution_clock::now();
    auto walkBulkTime = std::chrono::duration_cast<std::chrono::milliseconds>(walkBulkEnd - walkBulkStart);
    
    std::cout << "Guest agent: " << agentBulkTime.count() << "ms for " << agentEntries.size() << " pages" << std::endl;
    std::cout << "Direct walk: " << walkBulkTime.count() << "ms for " << walkAddrs.size() << " pages" << std::endl;
    std::cout << "Speedup: " << (float)agentBulkTime.count() / walkBulkTime.count() << "x" << std::endl;
    
    // Verify correctness on a sample
    int mismatches = 0;
    for (size_t i = 0; i < std::min(agentEntries.size(), walkAddrs.size()); i++) {
        if (agentEntries[i].physAddr != walkAddrs[i]) {
            mismatches++;
            if (mismatches <= 5) {  // Show first 5 mismatches
                std::cout << "Mismatch at page " << i << ": agent=0x" << std::hex 
                          << agentEntries[i].physAddr << " walk=0x" << walkAddrs[i] << std::dec << std::endl;
            }
        }
    }
    
    if (mismatches > 0) {
        std::cout << "WARNING: " << mismatches << " mismatches found!" << std::endl;
    } else {
        std::cout << "All translations match!" << std::endl;
    }
    
    return 0;
}