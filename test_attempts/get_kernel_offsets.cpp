#include <iostream>
#include <sstream>
#include <regex>
#include "guest_agent.h"

using namespace Haywire;

int main() {
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        return 1;
    }
    
    std::cout << "Extracting kernel struct offsets for Ubuntu 6.14..." << std::endl;
    
    std::string output;
    
    // Try to get offsets from various sources
    
    // 1. Check if debug symbols are installed
    std::cout << "\n1. Checking for debug symbols..." << std::endl;
    if (agent.ExecuteCommand("ls /usr/lib/debug/boot/vmlinux* 2>/dev/null | head -1", output)) {
        if (!output.empty()) {
            std::cout << "Debug symbols found: " << output;
            
            // Try to use objdump or readelf to get struct info
            // This is complex without proper tools
        } else {
            std::cout << "No debug symbols installed" << std::endl;
        }
    }
    
    // 2. Try to get hints from kernel config
    std::cout << "\n2. Checking kernel config..." << std::endl;
    if (agent.ExecuteCommand("grep -E 'CONFIG_ARM64_.*PAGE|CONFIG_PGTABLE' /boot/config-6.14.0-29-generic | head -5", output)) {
        std::cout << output << std::endl;
    }
    
    // 3. Try to infer offsets from /proc files
    std::cout << "\n3. Inferring from /proc structures..." << std::endl;
    
    // Get our own PID's info as a reference
    if (agent.ExecuteCommand("echo $$ && cat /proc/$$/stat | cut -d' ' -f1-5", output)) {
        std::cout << "Reference process info: " << output << std::endl;
    }
    
    // 4. Common ARM64 kernel struct offsets (educated guesses)
    std::cout << "\n4. Common ARM64 struct offsets (kernel 5.x/6.x estimates):" << std::endl;
    std::cout << "struct task_struct {" << std::endl;
    std::cout << "    // Common offsets for ARM64 64-bit kernel:" << std::endl;
    std::cout << "    0x0000: struct thread_info thread_info;" << std::endl;
    std::cout << "    0x0010: volatile long state;" << std::endl;
    std::cout << "    0x0018: void *stack;" << std::endl;
    std::cout << "    0x0298-0x02A8: struct list_head tasks; // varies" << std::endl;
    std::cout << "    0x0590-0x05A0: pid_t pid; // varies" << std::endl;
    std::cout << "    0x0730-0x0750: char comm[16]; // varies" << std::endl;
    std::cout << "    0x0398-0x03B0: struct mm_struct *mm; // varies" << std::endl;
    std::cout << "}" << std::endl;
    
    // 5. Try crash utility if available
    std::cout << "\n5. Checking for crash utility..." << std::endl;
    if (agent.ExecuteCommand("which crash 2>/dev/null", output)) {
        if (!output.empty()) {
            std::cout << "crash utility available at: " << output;
            std::cout << "Could extract exact offsets with: crash --osrelease" << std::endl;
        }
    }
    
    // 6. The most reliable: Install systemtap
    std::cout << "\n6. For exact offsets, install systemtap-sdt-dev and run:" << std::endl;
    std::cout << "   stap -e 'probe begin { print(@cast(0, \"struct task_struct\")->tasks) exit() }'" << std::endl;
    
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Without debug symbols or special tools, we must:" << std::endl;
    std::cout << "1. Use heuristics (adjacent pointers for lists)" << std::endl;
    std::cout << "2. Try common offsets from similar kernels" << std::endl;
    std::cout << "3. Pattern match in memory" << std::endl;
    
    agent.Disconnect();
    return 0;
}