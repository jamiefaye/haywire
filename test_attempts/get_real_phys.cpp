#include <iostream>
#include <iomanip>
#include <sstream>
#include "guest_agent.h"

using namespace Haywire;

int main() {
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    
    std::cout << "Getting real physical addresses via /proc/iomem and pagemap..." << std::endl;
    
    // First, let's see the physical memory layout
    std::string output;
    if (agent.ExecuteCommand("sudo cat /proc/iomem 2>/dev/null | grep -i 'system ram' | head -5", output)) {
        std::cout << "\nPhysical RAM regions from /proc/iomem:" << std::endl;
        std::cout << output << std::endl;
    }
    
    // Get init_task virtual address
    uint64_t init_task_virt = 0;
    if (agent.ExecuteCommand("grep ' init_task$' /proc/kallsyms", output)) {
        std::stringstream ss(output);
        std::string addr_str;
        ss >> addr_str;
        init_task_virt = std::stoull(addr_str, nullptr, 16);
        std::cout << "init_task virtual: 0x" << std::hex << init_task_virt << std::dec << std::endl;
    }
    
    // Try to get the actual physical address using pagemap for PID 1 (init)
    // The kernel symbols are mapped in every process, so we can check PID 1's pagemap
    std::cout << "\nTrying to get physical address via pagemap..." << std::endl;
    
    // Calculate page number for init_task
    uint64_t page_num = init_task_virt / 4096;
    uint64_t offset_in_page = init_task_virt & 0xFFF;
    
    // Read the pagemap entry for this page
    std::stringstream cmd;
    cmd << "sudo dd if=/proc/1/pagemap bs=8 skip=" << page_num << " count=1 2>/dev/null | od -t x8 -An";
    
    if (agent.ExecuteCommand(cmd.str(), output)) {
        std::cout << "Pagemap entry: " << output;
        
        // Parse the pagemap entry
        uint64_t pagemap_entry = std::stoull(output, nullptr, 16);
        
        if (pagemap_entry & (1ULL << 63)) { // Present bit
            uint64_t pfn = pagemap_entry & ((1ULL << 55) - 1);
            uint64_t phys_addr = (pfn * 4096) + offset_in_page;
            
            std::cout << "init_task physical: 0x" << std::hex << phys_addr << std::dec << std::endl;
            std::cout << "\nSo the virtual->physical mapping is:" << std::endl;
            std::cout << "  0x" << std::hex << init_task_virt << " -> 0x" << phys_addr << std::endl;
            std::cout << "  Offset: 0x" << (init_task_virt - phys_addr) << std::dec << std::endl;
        } else {
            std::cout << "Page not present in physical memory!" << std::endl;
        }
    }
    
    // Also try to find where kernel code actually is
    std::cout << "\nLooking for kernel code location..." << std::endl;
    if (agent.ExecuteCommand("sudo grep -i kernel /proc/iomem | head -5", output)) {
        std::cout << output;
    }
    
    agent.Disconnect();
    return 0;
}