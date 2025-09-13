#include <iostream>
#include "guest_agent.h"

using namespace Haywire;

int main() {
    GuestAgent agent;
    if (!agent.Connect("/tmp/qga.sock")) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    
    std::cout << "Connected!" << std::endl;
    
    // Test simple command
    std::string output;
    if (agent.ExecuteCommand("echo 'Hello from guest'", output)) {
        std::cout << "Command output: " << output << std::endl;
    } else {
        std::cerr << "Command failed" << std::endl;
    }
    
    // Test getting process list
    std::vector<ProcessInfo> processes;
    if (agent.GetProcessList(processes)) {
        std::cout << "Got " << processes.size() << " processes" << std::endl;
        for (size_t i = 0; i < std::min(size_t(5), processes.size()); i++) {
            std::cout << "  PID " << processes[i].pid << ": " << processes[i].name << std::endl;
        }
    }
    
    return 0;
}