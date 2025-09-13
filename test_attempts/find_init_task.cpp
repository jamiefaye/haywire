#include <iostream>
#include <sstream>
#include <iomanip>
#include "guest_agent.h"

using namespace Haywire;

int main() {
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    
    std::cout << "Searching for init_task in /proc/kallsyms..." << std::endl;
    
    std::string output;
    
    // Search for exact init_task symbol (not sched_ext_ops__init_task)
    if (agent.ExecuteCommand("grep ' init_task$' /proc/kallsyms", output)) {
        if (!output.empty()) {
            std::cout << "Found init_task: " << output;
            
            std::stringstream ss(output);
            std::string addr_str, type, name;
            ss >> addr_str >> type >> name;
            
            uint64_t addr = std::stoull(addr_str, nullptr, 16);
            std::cout << "init_task address: 0x" << std::hex << addr << std::dec << std::endl;
            std::cout << "Type: " << type << " (D=data, B=BSS, R=rodata)" << std::endl;
        } else {
            std::cout << "Exact init_task not found, searching for related symbols..." << std::endl;
            
            // Search for anything with init_task
            if (agent.ExecuteCommand("grep init_task /proc/kallsyms | head -10", output)) {
                std::cout << "Related symbols:\n" << output << std::endl;
            }
        }
    }
    
    // Also look for other useful symbols
    std::cout << "\nOther useful kernel symbols:" << std::endl;
    
    // swapper_pg_dir (page tables)
    if (agent.ExecuteCommand("grep swapper_pg_dir /proc/kallsyms", output)) {
        if (!output.empty()) {
            std::cout << "swapper_pg_dir: " << output;
        }
    }
    
    // init_mm (init memory descriptor)
    if (agent.ExecuteCommand("grep ' init_mm$' /proc/kallsyms", output)) {
        if (!output.empty()) {
            std::cout << "init_mm: " << output;
        }
    }
    
    // Try to get struct offsets from debugfs (if available)
    std::cout << "\nChecking for struct layout info:" << std::endl;
    if (agent.ExecuteCommand("ls /sys/kernel/debug/tracing/events/sched 2>/dev/null | head -5", output)) {
        if (!output.empty()) {
            std::cout << "Tracing events available (might have struct info)" << std::endl;
        }
    }
    
    agent.Disconnect();
    return 0;
}