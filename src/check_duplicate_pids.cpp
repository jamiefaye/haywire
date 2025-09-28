/**
 * Check for duplicate PIDs in our filtered processes
 */

#include "kernel_discovery.cpp"
#include <map>
#include <set>

int main() {
    std::cout << "=== Checking for Duplicate PIDs ===" << std::endl;

    Haywire::KernelDiscovery discovery;

    if (!discovery.Initialize()) {
        std::cerr << "Failed to initialize kernel discovery" << std::endl;
        return 1;
    }

    // Discover kernel structures
    discovery.DiscoverKernel();

    // Discover processes with our validated filtering
    if (!discovery.DiscoverProcesses()) {
        std::cerr << "Failed to discover processes" << std::endl;
        return 1;
    }

    // Analyze for duplicates
    const auto& processes = discovery.GetProcesses();
    std::map<uint32_t, std::vector<std::pair<std::string, uint64_t>>> pidOccurrences;
    std::map<std::string, std::set<uint32_t>> nameToUniquePids;

    for (const auto& proc : processes) {
        pidOccurrences[proc.pid].push_back({proc.comm, proc.task_addr});
        nameToUniquePids[proc.comm].insert(proc.pid);
    }

    // Find duplicate PIDs (same PID appearing multiple times)
    std::cout << "\n=== Duplicate PIDs (Same PID at Multiple Locations) ===" << std::endl;
    int duplicateCount = 0;
    for (const auto& [pid, occurrences] : pidOccurrences) {
        if (occurrences.size() > 1) {
            duplicateCount++;
            std::cout << "PID " << pid << " appears " << occurrences.size() << " times:" << std::endl;
            for (const auto& [name, addr] : occurrences) {
                std::cout << "  - Name: " << name << " at PA 0x" << std::hex << addr << std::dec << std::endl;
            }
        }
    }

    if (duplicateCount == 0) {
        std::cout << "  No duplicate PIDs found - each PID appears only once!" << std::endl;
    } else {
        std::cout << "\nTotal PIDs with duplicates: " << duplicateCount << std::endl;
    }

    // Find processes with multiple instances (same name, different PIDs)
    std::cout << "\n=== Process Names with Multiple PIDs ===" << std::endl;
    std::vector<std::pair<std::string, size_t>> multiInstanceProcs;

    for (const auto& [name, pids] : nameToUniquePids) {
        if (pids.size() > 5) {  // Show processes with 6+ instances
            multiInstanceProcs.push_back({name, pids.size()});
        }
    }

    // Sort by instance count
    std::sort(multiInstanceProcs.begin(), multiInstanceProcs.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    for (const auto& [name, count] : multiInstanceProcs) {
        std::cout << std::setw(20) << std::left << name << " : " << count << " instances" << std::endl;

        // Show some PIDs
        const auto& pids = nameToUniquePids[name];
        std::cout << "    PIDs: ";
        int shown = 0;
        for (const auto pid : pids) {
            if (shown >= 10) {
                std::cout << "...";
                break;
            }
            if (shown > 0) std::cout << ", ";
            std::cout << pid;
            shown++;
        }
        std::cout << std::endl;
    }

    // Check if these are threads vs processes
    std::cout << "\n=== Analysis ===" << std::endl;
    std::cout << "Total unique PIDs: " << pidOccurrences.size() << std::endl;
    std::cout << "Total task_structs found: " << processes.size() << std::endl;

    if (processes.size() == pidOccurrences.size()) {
        std::cout << "✓ No duplicate PIDs - each task_struct has a unique PID" << std::endl;
        std::cout << "The 'duplicates' are actually different threads/processes with the same name" << std::endl;
    } else {
        std::cout << "⚠ Found " << (processes.size() - pidOccurrences.size())
                  << " duplicate task_structs with same PID" << std::endl;
    }

    return 0;
}