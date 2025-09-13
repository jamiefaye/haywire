#include <iostream>
#include <sstream>
#include "guest_agent.h"

using namespace Haywire;

int main() {
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    
    std::string output;
    
    // First, find a kernel thread PID
    std::cout << "Finding kernel threads (processes in [brackets])..." << std::endl;
    if (agent.ExecuteCommand("ps aux | grep '^root.*\\[' | head -5", output)) {
        std::cout << output << std::endl;
    }
    
    // PID 2 is usually [kthreadd]
    std::cout << "\nTrying to read /proc/2/maps (kernel thread)..." << std::endl;
    if (agent.ExecuteCommand("cat /proc/2/maps 2>&1", output)) {
        if (output.find("No such") != std::string::npos || output.empty()) {
            std::cout << "No maps file for kernel threads (expected)" << std::endl;
        } else {
            std::cout << "Kernel maps found!:\n" << output << std::endl;
        }
    }
    
    // Try /proc/kcore - this is the real goldmine
    std::cout << "\nChecking /proc/kcore (kernel virtual memory)..." << std::endl;
    if (agent.ExecuteCommand("ls -lh /proc/kcore", output)) {
        std::cout << output;
        
        // Try to read the ELF header
        std::cout << "\nReading kcore ELF header..." << std::endl;
        if (agent.ExecuteCommand("dd if=/proc/kcore bs=64 count=1 2>/dev/null | od -t x1 -N 64", output)) {
            std::cout << output << std::endl;
            
            // Check if it's ELF (starts with 7F 45 4C 46)
            if (output.find("7f 45 4c 46") != std::string::npos) {
                std::cout << "✓ Valid ELF file - kernel memory is accessible!" << std::endl;
                
                // Try to read init_task through kcore!
                std::cout << "\nTrying to read init_task through /proc/kcore..." << std::endl;
                // init_task at 0xffff800083709840
                // kcore maps kernel virtual addresses directly
                // Offset in kcore = virtual address (for kernel addresses)
                
                // This would require parsing the ELF program headers to find the offset
                // But the idea is we could read kernel memory with proper VA!
            }
        }
    }
    
    // Alternative: Try /dev/mem (usually restricted)
    std::cout << "\nChecking /dev/mem access..." << std::endl;
    if (agent.ExecuteCommand("ls -l /dev/mem", output)) {
        std::cout << output;
    }
    
    // Check if we can translate addresses using pagemap
    std::cout << "\nTrying pagemap on kernel thread (PID 2)..." << std::endl;
    if (agent.ExecuteCommand("ls -l /proc/2/pagemap 2>&1", output)) {
        std::cout << output;
        
        if (output.find("No such") == std::string::npos) {
            // Try to read init_task's physical address via pagemap
            uint64_t init_task_va = 0xffff800083709840;
            uint64_t page_num = init_task_va / 4096;
            
            std::stringstream cmd;
            cmd << "dd if=/proc/2/pagemap bs=8 skip=" << page_num << " count=1 2>/dev/null | od -t x8 -An";
            
            std::cout << "\nReading pagemap entry for init_task..." << std::endl;
            if (agent.ExecuteCommand(cmd.str(), output)) {
                std::cout << "Pagemap entry: " << output;
            }
        }
    }
    
    agent.Disconnect();
    
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "1. /proc/kcore - Best option if readable (maps kernel VA correctly)" << std::endl;
    std::cout << "2. /proc/[pid]/pagemap - Can translate if accessible" << std::endl;
    std::cout << "3. Modified QEMU - Most reliable long-term solution" << std::endl;
    
    return 0;
}