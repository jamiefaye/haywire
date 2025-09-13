#include <iostream>
#include <sstream>
#include <iomanip>
#include "guest_agent.h"

using namespace Haywire;

int main() {
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        return 1;
    }
    
    std::string output;
    
    // Try PID 0 (kernel swapper)
    std::cout << "=== Checking /proc/0/maps (kernel) ===" << std::endl;
    if (agent.ExecuteCommand("cat /proc/0/maps 2>&1 | head -20", output)) {
        std::cout << output << std::endl;
    }
    
    // Try PID 1 (init - but runs in userspace mostly)
    std::cout << "\n=== Checking /proc/1/maps (init) ===" << std::endl;
    if (agent.ExecuteCommand("cat /proc/1/maps 2>&1 | head -20", output)) {
        std::cout << output << std::endl;
    }
    
    // Try PID 2 (usually kthreadd - kernel thread)
    std::cout << "\n=== Checking /proc/2/maps (kthreadd) ===" << std::endl;
    if (agent.ExecuteCommand("cat /proc/2/maps 2>&1 | head -20", output)) {
        std::cout << output << std::endl;
    }
    
    // Check for kernel threads
    std::cout << "\n=== Looking for kernel threads ===" << std::endl;
    if (agent.ExecuteCommand("ps aux | grep '\\[' | head -5", output)) {
        std::cout << "Kernel threads (in brackets):" << std::endl;
        std::cout << output << std::endl;
    }
    
    // Try to get pagemap for a kernel thread
    std::cout << "\n=== Checking if we can read kernel thread pagemap ===" << std::endl;
    if (agent.ExecuteCommand("ls -la /proc/2/pagemap 2>&1", output)) {
        std::cout << output << std::endl;
    }
    
    // Check what kernel sees as its memory
    std::cout << "\n=== Kernel's view from /proc/iomem ===" << std::endl;
    if (agent.ExecuteCommand("cat /proc/iomem 2>/dev/null | grep -i kernel | head -5", output)) {
        std::cout << output << std::endl;
    }
    
    // Try kcore - kernel's view of memory
    std::cout << "\n=== Checking /proc/kcore (kernel core) ===" << std::endl;
    if (agent.ExecuteCommand("ls -la /proc/kcore", output)) {
        std::cout << output << std::endl;
        
        // kcore is an ELF file representing kernel memory
        if (agent.ExecuteCommand("readelf -l /proc/kcore 2>/dev/null | grep LOAD | head -5", output)) {
            std::cout << "Kernel memory segments:" << std::endl;
            std::cout << output << std::endl;
        }
    }
    
    agent.Disconnect();
    return 0;
}