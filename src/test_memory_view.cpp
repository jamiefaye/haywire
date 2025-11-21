/**
 * Test program for PageDatabase and MemoryView system
 *
 * Demonstrates filter/sort capabilities by:
 * 1. Building PageDatabase from all processes
 * 2. Creating different filtered/sorted views
 * 3. Showing statistics and region coalescing
 */

#include <iostream>
#include <iomanip>
#include <sstream>
#include "page_database.h"
#include "memory_view.h"
#include "kernel_discovery_backend.h"
#include "qemu_connection.h"

using namespace Haywire;

void PrintViewInfo(const MemoryView& view, const std::string& title) {
    std::cout << "\n=== " << title << " ===\n";
    std::cout << "Description: " << view.GetDescription() << "\n";

    auto stats = view.GetStats();
    std::cout << "Total pages: " << stats.totalPages << "\n";
    std::cout << "Unique PIDs: " << stats.uniquePIDs << "\n";

    if (!stats.pagesByType.empty()) {
        std::cout << "\nPages by type:\n";
        PageMetadata tempMeta;
        for (const auto& [type, count] : stats.pagesByType) {
            tempMeta.ownershipType = type;
            std::cout << "  " << std::left << std::setw(15) << tempMeta.getTypeName()
                     << ": " << count << " pages\n";
        }
    }

    auto regions = view.GetRegions();
    std::cout << "\nCoalesced into " << regions.size() << " regions\n";

    // Show first few regions as examples
    int shown = 0;
    for (const auto& region : regions) {
        if (shown++ >= 5) {
            std::cout << "  ... (" << (regions.size() - 5) << " more regions)\n";
            break;
        }
        std::cout << "  " << std::hex << std::setw(16) << std::setfill('0')
                 << region.start << "-" << region.end << std::dec << std::setfill(' ')
                 << " " << region.permissions << " " << region.name << "\n";
    }
}

int main() {
    std::cout << "Memory View Test Program\n";
    std::cout << "========================\n\n";

    // Initialize QMP connection first (singleton required by kernel discovery)
    QemuConnection& qemu = QemuConnection::getInstance();
    if (!qemu.AutoConnect()) {
        std::cerr << "WARNING: QMP auto-connect failed, trying manual connection...\n";
        if (!qemu.ConnectQMP("localhost", 4445)) {
            std::cerr << "FATAL: Could not connect to QMP on localhost:4445\n";
            std::cerr << "Make sure QEMU is running with: -qmp tcp:localhost:4445,server=on,wait=off\n";
            return 1;
        }
    }
    std::cout << "QMP connected successfully\n\n";

    // Initialize kernel discovery
    auto kernelDiscovery = std::make_shared<KernelDiscoveryBackend>();

    if (!kernelDiscovery->Initialize("/tmp/haywire-vm-mem", "localhost", 4445)) {
        std::cerr << "FATAL: Failed to initialize kernel discovery\n";
        return 1;
    }

    std::cout << "Kernel discovery initialized\n";

    // Get RAM bounds from memory file
    uint64_t ramBase = 0x40000000;  // Standard ARM64 QEMU RAM base
    uint64_t ramSize = 4ULL * 1024 * 1024 * 1024;  // 4GB

    // Create and initialize page database
    PageDatabase pageDB;
    pageDB.Initialize(ramBase, ramSize);

    std::cout << "Scanning all processes...\n";

    // Test with different thread counts
    int numThreads = 8;  // Use 8 threads (adjust based on your CPU)
    size_t attributedCount = pageDB.ScanAllProcesses(kernelDiscovery.get(), numThreads);

    std::cout << "\nPage Database built:\n";
    auto dbStats = pageDB.GetStats();
    std::cout << "  Total pages: " << dbStats.totalPages << "\n";
    std::cout << "  Attributed: " << dbStats.attributedPages << "\n";
    std::cout << "  Unattributed: " << dbStats.unattributedPages << "\n";

    // Test different view presets

    // 1. Physical scan (all pages)
    MemoryView physView;
    auto [physFilter, physSort] = ViewPresets::PhysicalScan();
    physView.BuildFromDatabase(&pageDB, physFilter, physSort);
    PrintViewInfo(physView, "Physical Memory Scan (All Pages)");

    // 2. All stacks across system
    MemoryView stackView;
    auto [stackFilter, stackSort] = ViewPresets::AllStacks();
    stackView.BuildFromDatabase(&pageDB, stackFilter, stackSort);
    PrintViewInfo(stackView, "All Stack Pages");

    // 3. All executable pages
    MemoryView execView;
    auto [execFilter, execSort] = ViewPresets::ExecutablePages();
    execView.BuildFromDatabase(&pageDB, execFilter, execSort);
    PrintViewInfo(execView, "All Executable Pages");

    // 4. Unattributed pages only
    MemoryView unattribView;
    auto [unattribFilter, unattribSort] = ViewPresets::UnattributedPages();
    unattribView.BuildFromDatabase(&pageDB, unattribFilter, unattribSort);
    PrintViewInfo(unattribView, "Unattributed Pages");

    // 5. Single process (find a PID first)
    std::vector<uint32_t> pids;
    if (kernelDiscovery->GetPIDList(pids) && !pids.empty()) {
        uint32_t testPid = pids[0];
        ProcessInfo info;
        kernelDiscovery->GetProcessInfo(testPid, info);

        MemoryView pidView;
        auto [pidFilter, pidSort] = ViewPresets::SingleProcess(testPid);
        pidView.BuildFromDatabase(&pageDB, pidFilter, pidSort);

        std::stringstream ss;
        ss << "Single Process: PID " << testPid << " (" << info.name << ")";
        PrintViewInfo(pidView, ss.str());
    }

    std::cout << "\n=== Test completed successfully ===\n";
    return 0;
}
