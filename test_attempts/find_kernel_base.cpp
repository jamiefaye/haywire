#include <iostream>
#include <sstream>
#include "guest_agent.h"

using namespace Haywire;

int main() {
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        return 1;
    }
    
    std::string output;
    
    // Get the actual kernel addresses from /proc/kallsyms
    std::cout << "Finding kernel base from /proc/kallsyms..." << std::endl;
    if (agent.ExecuteCommand("grep ' _text\\| _stext\\| init_task' /proc/kallsyms | head -5", output)) {
        std::cout << output << std::endl;
    }
    
    // Check if we need root
    std::cout << "\nChecking kallsyms permissions..." << std::endl;
    if (agent.ExecuteCommand("ls -la /proc/kallsyms", output)) {
        std::cout << output;
    }
    
    // Try with sudo
    std::cout << "\nTrying with sudo..." << std::endl;
    if (agent.ExecuteCommand("sudo grep ' _text\\| init_task' /proc/kallsyms 2>&1 | head -5", output)) {
        std::cout << output << std::endl;
    }
    
    // Alternative: Check System.map
    std::cout << "\nChecking System.map..." << std::endl;
    if (agent.ExecuteCommand("ls /boot/System.map* 2>/dev/null | head -1", output)) {
        if (!output.empty()) {
            std::cout << "Found: " << output;
            std::string map_file = output.substr(0, output.find('\n'));
            
            std::stringstream cmd;
            cmd << "grep ' init_task\\| _text' " << map_file << " | head -5";
            if (agent.ExecuteCommand(cmd.str(), output)) {
                std::cout << output << std::endl;
            }
        }
    }
    
    // Check kernel command line for nokaslr
    std::cout << "\nChecking for KASLR..." << std::endl;
    if (agent.ExecuteCommand("cat /proc/cmdline", output)) {
        std::cout << "Kernel cmdline: " << output;
        if (output.find("nokaslr") != std::string::npos) {
            std::cout << "✓ KASLR is disabled" << std::endl;
        } else {
            std::cout << "⚠ KASLR may be enabled (addresses randomized)" << std::endl;
        }
    }
    
    // Try to find kernel in kcore segments
    std::cout << "\nKcore segments that might contain kernel:" << std::endl;
    if (agent.ExecuteCommand("readelf -l /proc/kcore 2>/dev/null | grep -A1 'LOAD.*0xffff80' | head -10", output)) {
        std::cout << output << std::endl;
    }
    
    agent.Disconnect();
    return 0;
}