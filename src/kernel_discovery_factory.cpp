/**
 * Kernel Discovery Factory
 *
 * Creates appropriate kernel discovery implementation based on guest OS
 */

#include "../include/ikernel_discovery.h"
#include "linux/linux_kernel_discovery.cpp"  // For now, include directly
#include "windows/windows_kernel_discovery.cpp"  // Windows implementation

#include <fstream>
#include <iostream>

namespace Haywire {

/**
 * Detect guest OS from memory file
 *
 * Looks for OS-specific signatures in memory to identify the guest OS
 */
GuestOS DetectGuestOS(const std::string& memoryFile) {
    // For now, just return Linux
    // TODO: Implement actual OS detection by scanning for signatures

    // Windows signatures to look for:
    // - "MZ" header patterns (PE executables)
    // - KPCR patterns
    // - "ntoskrnl.exe" strings

    // Linux signatures:
    // - task_struct patterns
    // - "swapper" process name
    // - Linux kernel symbols

    std::cout << "OS detection not yet implemented, assuming Linux" << std::endl;
    return GuestOS::Linux;
}

/**
 * Factory function to create appropriate kernel discovery implementation
 */
std::unique_ptr<IKernelDiscovery> CreateKernelDiscovery(
    const std::string& memoryFile,
    const std::string& profilePath,
    GuestOS osHint)
{
    // Detect OS if not provided
    GuestOS detectedOS = (osHint != GuestOS::Unknown)
        ? osHint
        : DetectGuestOS(memoryFile);

    switch (detectedOS) {
        case GuestOS::Linux:
            std::cout << "Creating Linux kernel discovery backend" << std::endl;
            return std::make_unique<LinuxKernelDiscovery>(memoryFile, profilePath);

        case GuestOS::Windows:
            std::cout << "Creating Windows kernel discovery backend" << std::endl;
            return std::make_unique<WindowsKernelDiscovery>(memoryFile, profilePath);

        default:
            std::cerr << "Could not detect guest OS type" << std::endl;
            std::cerr << "Defaulting to Linux..." << std::endl;
            return std::make_unique<LinuxKernelDiscovery>(memoryFile, profilePath);
    }
}

} // namespace Haywire
