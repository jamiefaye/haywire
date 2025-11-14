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

    // Show detailed section information for first few user processes
    std::cout << "\n=== DETAILED SECTION ANALYSIS (First 3 User Processes) ===" << std::endl;
    int shown = 0;
    for (const auto& proc : processes) {
        if (proc.is_kernel_thread || shown >= 3) continue;
        if (proc.sections.empty()) continue;

        std::cout << "\nPID " << proc.pid << " (" << proc.comm << "):" << std::endl;
        std::cout << "  Sections: " << proc.sections.size() << std::endl;

        // Count by type
        std::map<std::string, int> typeCounts;
        std::map<std::string, std::vector<std::string>> filesByType;

        for (const auto& sec : proc.sections) {
            std::string typeName;
            switch (sec.type) {
                case Haywire::KernelDiscovery::MemorySection::SHARED_LIB: typeName = "Shared Lib"; break;
                case Haywire::KernelDiscovery::MemorySection::EXECUTABLE: typeName = "Executable"; break;
                case Haywire::KernelDiscovery::MemorySection::FILE_BACKED: typeName = "File-backed"; break;
                case Haywire::KernelDiscovery::MemorySection::STACK: typeName = "Stack"; break;
                case Haywire::KernelDiscovery::MemorySection::HEAP: typeName = "Heap"; break;
                case Haywire::KernelDiscovery::MemorySection::VDSO: typeName = "vDSO"; break;
                case Haywire::KernelDiscovery::MemorySection::VVAR: typeName = "vvar"; break;
                case Haywire::KernelDiscovery::MemorySection::ANONYMOUS: typeName = "Anonymous"; break;
                default: typeName = "Unknown"; break;
            }
            typeCounts[typeName]++;

            if (!sec.name.empty() && (sec.type == Haywire::KernelDiscovery::MemorySection::SHARED_LIB ||
                                      sec.type == Haywire::KernelDiscovery::MemorySection::EXECUTABLE)) {
                filesByType[typeName].push_back(sec.name);
            }
        }

        // Display counts
        for (const auto& [type, count] : typeCounts) {
            std::cout << "    " << std::setw(15) << std::left << type << ": " << count << " sections";

            // Show unique files for libs/executables
            if (filesByType.count(type)) {
                auto& files = filesByType[type];
                std::sort(files.begin(), files.end());
                files.erase(std::unique(files.begin(), files.end()), files.end());

                if (!files.empty()) {
                    std::cout << " (";
                    for (size_t i = 0; i < std::min(files.size(), size_t(3)); i++) {
                        // Extract basename
                        std::string basename = files[i];
                        size_t lastSlash = basename.find_last_of('/');
                        if (lastSlash != std::string::npos) {
                            basename = basename.substr(lastSlash + 1);
                        }
                        std::cout << basename;
                        if (i < std::min(files.size(), size_t(3)) - 1) std::cout << ", ";
                    }
                    if (files.size() > 3) std::cout << " +" << (files.size() - 3) << " more";
                    std::cout << ")";
                }
            }
            std::cout << std::endl;
        }

        shown++;
    }

    return 0;
}