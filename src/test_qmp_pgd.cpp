/**
 * Test QMP-based PGD extraction
 */

#include "kernel_discovery.cpp"

int main() {
    std::cout << "=== Testing QMP-based PGD Extraction ===" << std::endl;

    Haywire::KernelDiscovery discovery;

    // Explicitly set QMP parameters (default should work)
    // Constructor: KernelDiscovery(memFile, qmpHost, qmpPort)

    if (!discovery.Initialize()) {
        std::cerr << "Failed to initialize kernel discovery" << std::endl;
        return 1;
    }

    // QMP connection is already attempted in Initialize()
    // No need to call ConnectQMP() separately

    // Discover kernel structures (will use QMP if connected)
    if (!discovery.DiscoverKernel()) {
        std::cerr << "Warning: Kernel discovery incomplete" << std::endl;
    }

    // Show what we found
    const auto& kernelInfo = discovery.GetKernelInfo();
    std::cout << "\n=== Kernel Info ===" << std::endl;
    std::cout << "Swapper PGD: 0x" << std::hex << kernelInfo.swapper_pgd << std::dec << std::endl;
    if (kernelInfo.swapper_pgd == 0x136deb000) {
        std::cout << "  ✓ This matches the expected QMP value!" << std::endl;
    } else if (kernelInfo.swapper_pgd == 0xbce45000) {
        std::cout << "  ✗ This is from scanning, not QMP" << std::endl;
    }

    // Discover processes
    if (!discovery.DiscoverProcesses()) {
        std::cerr << "Failed to discover processes" << std::endl;
        return 1;
    }

    // Extract PGDs
    discovery.ExtractProcessPGDs();

    // Get results
    const auto& processes = discovery.GetProcesses();

    // Count successes
    int withPGD = 0;
    int userProcs = 0;
    for (const auto& proc : processes) {
        if (!proc.is_kernel_thread) {
            userProcs++;
            if (proc.pgd != 0) withPGD++;
        }
    }

    std::cout << "\n=== Final Results ===" << std::endl;
    std::cout << "Total user processes: " << userProcs << std::endl;
    std::cout << "With PGD extracted: " << withPGD << std::endl;
    std::cout << "Success rate: " << (userProcs > 0 ? (100.0 * withPGD / userProcs) : 0) << "%" << std::endl;

    return 0;
}