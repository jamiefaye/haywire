// Example showing how to use the new platform abstraction layer
#include "platform/platform_detector.h"
#include "platform/page_walker.h"
#include "platform/process_walker.h"
#include "memory_backend.h"
#include <iostream>
#include <iomanip>

using namespace Haywire;

void ExampleUsage(MemoryBackend* backend) {
    // 1. Detect the platform automatically
    PlatformInfo platform = PlatformDetector::DetectPlatform(backend);

    std::cout << "Detected Platform:\n";
    std::cout << "  Host: " << platform.hostOS << "/" << platform.hostArch << "\n";
    std::cout << "  Guest: " << platform.guestOS << "/" << platform.guestArch << "\n";
    std::cout << "  Kernel: " << platform.kernelVersion << "\n\n";

    // 2. Create appropriate page walker for the architecture
    auto pageWalker = PlatformDetector::CreatePageWalker(backend, platform);

    std::cout << "Using page walker: " << pageWalker->GetArchitectureName() << "\n";
    std::cout << "Page size: " << pageWalker->GetPageSize() << " bytes\n\n";

    // 3. Create appropriate process walker for the OS
    auto processWalker = PlatformDetector::CreateProcessWalker(backend, platform);

    std::cout << "Using process walker: " << processWalker->GetOSName() << "\n";
    std::cout << "Kernel version: " << processWalker->GetKernelVersion() << "\n\n";

    // 4. Initialize and enumerate processes
    if (processWalker->Initialize()) {
        std::cout << "Process walker initialized successfully\n\n";

        // Enumerate all processes
        auto processes = processWalker->EnumerateProcesses();
        std::cout << "Found " << processes.size() << " processes:\n";

        for (const auto& proc : processes) {
            std::cout << "  PID " << std::setw(6) << proc.pid
                     << ": " << proc.name;

            // Set up page walker for this process
            if (proc.page_table_base != 0) {
                pageWalker->SetPageTableBase(proc.page_table_base);

                // Example: translate some virtual addresses
                uint64_t testVA = 0x400000;  // Common code segment start
                uint64_t pa = pageWalker->TranslateAddress(testVA);
                if (pa != 0) {
                    std::cout << " [VA 0x" << std::hex << testVA
                             << " -> PA 0x" << pa << std::dec << "]";
                }
            }
            std::cout << "\n";
        }

        // 5. Find a specific process
        std::cout << "\nSearching for init/System process...\n";
        ProcessInfo initProc;

        // Try Linux init (PID 1) or Windows System (PID 4)
        if (processWalker->FindProcess(1, initProc)) {
            std::cout << "Found init process: " << initProc.name << "\n";
        } else if (processWalker->FindProcess(4, initProc)) {
            std::cout << "Found System process: " << initProc.name << "\n";
        }

        // 6. Search by name
        std::cout << "\nSearching for processes containing 'ssh'...\n";
        auto sshProcs = processWalker->FindProcessesByName("ssh");
        for (const auto& proc : sshProcs) {
            std::cout << "  Found: " << proc.name << " (PID " << proc.pid << ")\n";
        }

    } else {
        std::cerr << "Failed to initialize process walker\n";
    }
}

// Alternative: Manual platform selection
void ManualPlatformSelection(MemoryBackend* backend) {
    // For Windows host with Intel Linux guest:
    std::cout << "\n=== Manual Platform Selection ===\n";
    std::cout << "Creating x86-64 page walker for Intel hardware...\n";

    auto pageWalker = CreatePageWalker(backend, "x86_64");
    std::cout << "Page walker: " << pageWalker->GetArchitectureName() << "\n";

    std::cout << "Creating Linux process walker...\n";
    auto processWalker = CreateProcessWalker(backend, "linux");
    std::cout << "Process walker: " << processWalker->GetOSName() << "\n";

    // Use them as needed...
}

int main() {
    // Create memory backend (implementation depends on your setup)
    // This could be QMP-based, shared memory, etc.
    MemoryBackend* backend = nullptr;  // CreateMemoryBackend();

    if (backend && backend->IsAvailable()) {
        // Automatic detection
        ExampleUsage(backend);

        // Manual selection
        ManualPlatformSelection(backend);
    } else {
        std::cerr << "Memory backend not available\n";
    }

    return 0;
}