#pragma once

#include <string>
#include <memory>

namespace Haywire {

class MemoryBackend;
class PageWalker;
class ProcessWalker;

// Platform information structure
struct PlatformInfo {
    std::string hostOS;        // Host OS (Windows, macOS, Linux)
    std::string hostArch;      // Host architecture (x86_64, arm64)
    std::string guestOS;       // Guest OS (Linux, Windows)
    std::string guestArch;     // Guest architecture (x86_64, arm64)
    std::string kernelVersion; // Guest kernel version
    bool is64Bit;              // Is guest 64-bit
};

// Platform detection and factory class
class PlatformDetector {
public:
    // Detect platform information from QEMU/memory
    static PlatformInfo DetectPlatform(MemoryBackend* backend);

    // Create appropriate page walker for detected platform
    static std::unique_ptr<PageWalker> CreatePageWalker(
        MemoryBackend* backend,
        const PlatformInfo& platform);

    // Create appropriate process walker for detected platform
    static std::unique_ptr<ProcessWalker> CreateProcessWalker(
        MemoryBackend* backend,
        const PlatformInfo& platform);

    // Helper to detect guest OS from memory patterns
    static std::string DetectGuestOS(MemoryBackend* backend);

    // Helper to detect guest architecture from QEMU
    static std::string DetectGuestArchitecture(MemoryBackend* backend);

private:
    // Check for Linux signatures in memory
    static bool HasLinuxSignatures(MemoryBackend* backend);

    // Check for Windows signatures in memory
    static bool HasWindowsSignatures(MemoryBackend* backend);

    // Get kernel version string if available
    static std::string GetKernelVersion(MemoryBackend* backend,
                                        const std::string& os);
};

}