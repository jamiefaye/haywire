/**
 * Test the updated KernelDiscovery class
 */

#include "kernel_discovery.cpp"

int main() {
    std::cout << "=== Testing Updated Kernel Discovery ===" << std::endl;

    Haywire::KernelDiscovery discovery;

    if (!discovery.Initialize()) {
        std::cerr << "Failed to initialize kernel discovery" << std::endl;
        return 1;
    }

    // Discover kernel structures
    discovery.DiscoverKernel();

    // Discover processes
    if (!discovery.DiscoverProcesses()) {
        std::cerr << "Failed to discover processes" << std::endl;
        return 1;
    }

    // Get summary
    const auto& processes = discovery.GetProcesses();
    std::cout << "\n=== Final Summary ===" << std::endl;
    std::cout << "Total processes found: " << processes.size() << std::endl;

    // Count by type
    int kernelCount = 0, userCount = 0;
    std::map<std::string, std::vector<uint32_t>> processByName;

    for (const auto& proc : processes) {
        if (proc.is_kernel_thread) kernelCount++;
        else userCount++;
        processByName[proc.comm].push_back(proc.pid);
    }

    std::cout << "Kernel threads: " << kernelCount << std::endl;
    std::cout << "User processes: " << userCount << std::endl;
    std::cout << "Unique process names: " << processByName.size() << std::endl;

    // Show ALL processes grouped by name
    std::cout << "\n=== ALL PROCESSES BY NAME ===" << std::endl;
    for (const auto& [name, pids] : processByName) {
        std::cout << std::setw(20) << std::left << name << " : ";

        // Show PIDs
        for (size_t i = 0; i < pids.size(); i++) {
            if (i < 5) {
                std::cout << pids[i];
                if (i < pids.size() - 1 && i < 4) std::cout << ", ";
            }
        }
        if (pids.size() > 5) {
            std::cout << " ... (" << pids.size() << " total)";
        }

        // Mark system processes
        bool isSystem = false;
        const std::vector<std::string> sysProcs = {
            "systemd", "init", "kworker", "kthread", "ksoftirq",
            "migration", "rcu_", "sshd", "bash", "dbus"
        };
        for (const auto& sp : sysProcs) {
            if (name.find(sp) != std::string::npos) {
                isSystem = true;
                break;
            }
        }
        if (isSystem) std::cout << " <-- SYSTEM";

        std::cout << std::endl;
    }

    return 0;
}