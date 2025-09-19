// Test program for the new platform abstraction layer
#include "platform/page_walker.h"
#include "platform/process_walker.h"
#include "memory_backend.h"
#include "qemu_connection.h"
#include <iostream>
#include <iomanip>

using namespace Haywire;

int main(int argc, char* argv[]) {
    std::cout << "=== Testing Platform Abstraction Layer ===\n\n";

    // Connect to QEMU
    auto qemu = std::make_unique<QemuConnection>();
    if (!qemu->IsAvailable()) {
        std::cerr << "Failed to connect to QEMU (check if VM is running)\n";
        return 1;
    }

    std::cout << "Connected to QEMU\n";

    // Test 1: Create ARM64 page walker
    std::cout << "\n1. Testing ARM64 Page Walker:\n";
    auto arm64Walker = CreatePageWalker(qemu->GetMemoryBackend(), "arm64");
    if (arm64Walker) {
        std::cout << "   Created " << arm64Walker->GetArchitectureName()
                 << " page walker\n";
        std::cout << "   Page size: " << arm64Walker->GetPageSize() << " bytes\n";
    }

    // Test 2: Create x86-64 page walker
    std::cout << "\n2. Testing x86-64 Page Walker:\n";
    auto x86Walker = CreatePageWalker(qemu->GetMemoryBackend(), "x86_64");
    if (x86Walker) {
        std::cout << "   Created " << x86Walker->GetArchitectureName()
                 << " page walker\n";
        std::cout << "   Page size: " << x86Walker->GetPageSize() << " bytes\n";
    }

    // Test 3: Create Linux process walker
    std::cout << "\n3. Testing Linux Process Walker:\n";
    auto linuxWalker = CreateProcessWalker(qemu->GetMemoryBackend(), "linux");
    if (linuxWalker) {
        std::cout << "   Created " << linuxWalker->GetOSName()
                 << " process walker\n";

        // Initialize and enumerate processes
        if (linuxWalker->Initialize()) {
            std::cout << "   Process walker initialized\n";

            auto processes = linuxWalker->EnumerateProcesses();
            std::cout << "   Found " << processes.size() << " processes:\n";

            // Show first 10 processes
            int count = 0;
            for (const auto& proc : processes) {
                if (count++ >= 10) break;
                std::cout << "     PID " << std::setw(6) << proc.pid
                         << ": " << std::setw(16) << proc.name;
                if (proc.page_table_base != 0) {
                    std::cout << " [TTBR/CR3: 0x" << std::hex
                             << proc.page_table_base << std::dec << "]";
                }
                std::cout << "\n";
            }

            if (processes.size() > 10) {
                std::cout << "     ... and " << (processes.size() - 10)
                         << " more\n";
            }

            // Test finding specific process
            std::cout << "\n   Testing FindProcess(PID 1):\n";
            Haywire::ProcessInfo initProc;
            if (linuxWalker->FindProcess(1, initProc)) {
                std::cout << "     Found: " << initProc.name
                         << " at 0x" << std::hex << initProc.task_struct_addr
                         << std::dec << "\n";
            }

            // Test searching by name
            std::cout << "\n   Testing FindProcessesByName(\"ssh\"):\n";
            auto sshProcs = linuxWalker->FindProcessesByName("ssh");
            for (const auto& proc : sshProcs) {
                std::cout << "     Found: " << proc.name
                         << " (PID " << proc.pid << ")\n";
            }
        } else {
            std::cerr << "   Failed to initialize process walker\n";
        }
    }

    // Test 4: Combined usage - Process walker + Page walker
    std::cout << "\n4. Testing Combined Usage:\n";
    if (linuxWalker && arm64Walker) {
        Haywire::ProcessInfo proc;
        if (linuxWalker->FindProcess(1, proc) && proc.page_table_base != 0) {
            std::cout << "   Setting up page walker for init process\n";
            arm64Walker->SetPageTableBase(proc.page_table_base, 0);

            // Try to translate some common addresses
            std::vector<uint64_t> testAddresses = {
                0x400000,      // Common code start
                0x600000,      // Data segment
                0xFFFF000000000000ULL,  // Kernel space
            };

            for (uint64_t va : testAddresses) {
                uint64_t pa = arm64Walker->TranslateAddress(va);
                if (pa != 0) {
                    std::cout << "     VA 0x" << std::hex << va
                             << " -> PA 0x" << pa << std::dec << "\n";
                }
            }
        }
    }

    std::cout << "\n=== Platform Abstraction Tests Complete ===\n";
    return 0;
}