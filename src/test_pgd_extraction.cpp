/**
 * Test PGD extraction and memory section discovery
 */

#include "kernel_discovery.cpp"

int main() {
    std::cout << "=== Testing PGD Extraction and Memory Sections ===" << std::endl;

    Haywire::KernelDiscovery discovery;

    if (!discovery.Initialize()) {
        std::cerr << "Failed to initialize kernel discovery" << std::endl;
        return 1;
    }

    // Discover kernel structures
    if (!discovery.DiscoverKernel()) {
        std::cerr << "Warning: Kernel discovery incomplete" << std::endl;
    }

    // Discover processes
    if (!discovery.DiscoverProcesses()) {
        std::cerr << "Failed to discover processes" << std::endl;
        return 1;
    }

    // Extract PGDs and memory sections
    discovery.ExtractProcessPGDs();

    // Get detailed analysis
    const auto& processes = discovery.GetProcesses();

    std::cout << "\n=== Detailed Process Analysis ===" << std::endl;

    // Count processes with PGDs and sections
    int withPGD = 0;
    int withSections = 0;
    int kernelThreads = 0;

    for (const auto& proc : processes) {
        if (proc.is_kernel_thread) {
            kernelThreads++;
            continue;
        }
        if (proc.pgd != 0) withPGD++;
        if (!proc.sections.empty()) withSections++;
    }

    std::cout << "User processes: " << (processes.size() - kernelThreads) << std::endl;
    std::cout << "With PGD extracted: " << withPGD << std::endl;
    std::cout << "With memory sections: " << withSections << std::endl;

    // Show details for a few interesting processes
    std::cout << "\n=== Sample Process Details ===" << std::endl;

    int shown = 0;
    for (const auto& proc : processes) {
        if (proc.is_kernel_thread) continue;
        if (proc.pgd == 0) continue;

        // Show interesting processes
        if (proc.comm == "systemd" || proc.comm == "bash" ||
            proc.comm == "vlc" || proc.comm.find("firefox") != std::string::npos ||
            shown < 5) {

            std::cout << "\nPID " << proc.pid << " (" << proc.comm << "):" << std::endl;
            std::cout << "  Task struct: 0x" << std::hex << proc.task_addr << std::dec << std::endl;
            std::cout << "  mm_struct: 0x" << std::hex << proc.mm_addr << std::dec << std::endl;
            std::cout << "  PGD: 0x" << std::hex << proc.pgd << std::dec << std::endl;
            std::cout << "  Memory sections: " << proc.sections.size() << std::endl;

            // Show first few memory sections
            if (!proc.sections.empty()) {
                std::cout << "  First sections:" << std::endl;
                for (size_t i = 0; i < std::min(size_t(3), proc.sections.size()); i++) {
                    const auto& sec = proc.sections[i];
                    std::cout << "    0x" << std::hex << sec.start
                              << " - 0x" << sec.end
                              << " (flags: 0x" << sec.flags << ")" << std::dec;
                    if (!sec.name.empty()) {
                        std::cout << " [" << sec.name << "]";
                    }
                    std::cout << std::endl;
                }
            }

            shown++;
            if (shown >= 10) break;
        }
    }

    return 0;
}