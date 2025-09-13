#include <iostream>
#include <sstream>
#include "guest_agent.h"

using namespace Haywire;

int main() {
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        std::cerr << "Failed to connect to guest agent" << std::endl;
        return 1;
    }
    
    std::cout << "=== Checking kernel symbols in guest ===" << std::endl << std::endl;
    
    // 1. Check /proc/kallsyms
    std::cout << "1. /proc/kallsyms (live kernel symbols):" << std::endl;
    std::string output;
    
    if (agent.ExecuteCommand("head -5 /proc/kallsyms", output)) {
        if (output.find("0000000000000000") != std::string::npos) {
            std::cout << "   ✗ Hidden (shows zeros - need root or kptr_restrict=0)" << std::endl;
        } else {
            std::cout << "   ✓ Visible! Sample:" << std::endl;
            std::cout << output << std::endl;
        }
    }
    
    // Check for init_task specifically
    if (agent.ExecuteCommand("grep init_task /proc/kallsyms | head -1", output)) {
        if (!output.empty() && output.find("0000000000000000") == std::string::npos) {
            std::cout << "   init_task found: " << output;
            
            // Parse the address
            std::stringstream ss(output);
            std::string addr_str;
            ss >> addr_str;
            
            uint64_t init_task_addr = std::stoull(addr_str, nullptr, 16);
            std::cout << "   init_task at: 0x" << std::hex << init_task_addr << std::dec << std::endl;
        }
    }
    
    // 2. Check kptr_restrict
    std::cout << "\n2. Kernel pointer restriction:" << std::endl;
    if (agent.ExecuteCommand("cat /proc/sys/kernel/kptr_restrict", output)) {
        int level = atoi(output.c_str());
        switch(level) {
            case 0: std::cout << "   0 = No restriction (addresses visible!)" << std::endl; break;
            case 1: std::cout << "   1 = Hidden from non-root" << std::endl; break;
            case 2: std::cout << "   2 = Hidden from everyone" << std::endl; break;
        }
    }
    
    // 3. Try with sudo
    std::cout << "\n3. Trying with sudo:" << std::endl;
    if (agent.ExecuteCommand("echo ubuntu | sudo -S grep init_task /proc/kallsyms 2>/dev/null | head -1", output)) {
        if (!output.empty() && output.find("0000000000000000") == std::string::npos) {
            std::cout << "   ✓ With sudo we can see: " << output;
        } else {
            std::cout << "   ✗ Still hidden or sudo failed" << std::endl;
        }
    }
    
    // 4. Check System.map
    std::cout << "\n4. System.map files:" << std::endl;
    if (agent.ExecuteCommand("ls -la /boot/System.map-* 2>/dev/null | head -3", output)) {
        if (!output.empty()) {
            std::cout << output;
        } else {
            std::cout << "   ✗ No System.map files found" << std::endl;
        }
    }
    
    // 5. Check kernel version for offset database
    std::cout << "\n5. Kernel version (for offset database):" << std::endl;
    if (agent.ExecuteCommand("uname -r", output)) {
        std::cout << "   " << output;
    }
    
    // 6. Check KASLR
    std::cout << "\n6. KASLR status:" << std::endl;
    if (agent.ExecuteCommand("grep -o nokaslr /proc/cmdline", output)) {
        if (output.find("nokaslr") != std::string::npos) {
            std::cout << "   ✓ KASLR disabled (addresses not randomized)" << std::endl;
        } else {
            std::cout << "   ✗ KASLR enabled (addresses randomized at boot)" << std::endl;
        }
    }
    
    // 7. Try to find some actual addresses
    std::cout << "\n7. Looking for kernel addresses we can use:" << std::endl;
    
    // Try dmesg for kernel base
    if (agent.ExecuteCommand("dmesg | grep -i 'kernel code' | head -1", output)) {
        if (!output.empty()) {
            std::cout << "   Kernel code location: " << output;
        }
    }
    
    // Summary
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "To go agent-free, we need either:" << std::endl;
    std::cout << "1. Sudo access once to read /proc/kallsyms" << std::endl;
    std::cout << "2. Pattern matching to find init_task" << std::endl;
    std::cout << "3. Kernel version to use offset database" << std::endl;
    
    agent.Disconnect();
    return 0;
}