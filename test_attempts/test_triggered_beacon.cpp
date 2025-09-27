#include <iostream>
#include <vector>
#include "triggered_beacon_reader.h"

using namespace Haywire;

int main(int argc, char* argv[]) {
    std::cout << "Testing Triggered Beacon Reader" << std::endl;
    std::cout << "================================" << std::endl;

    // Create reader
    TriggeredBeaconReader reader;

    // Initialize with memory file
    if (!reader.InitializeTriggered("/tmp/haywire-vm-mem", "localhost", 2222)) {
        std::cerr << "Failed to initialize beacon reader" << std::endl;
        return 1;
    }

    // Check if companion is installed
    if (!reader.IsCompanionInstalled()) {
        std::cout << "Companion not installed, installing..." << std::endl;
        if (!reader.InstallCompanion()) {
            std::cerr << "Failed to install companion" << std::endl;
            return 1;
        }
    }

    // Test 1: Basic trigger
    std::cout << "\nTest 1: Basic process list" << std::endl;
    if (reader.TriggerRefresh()) {
        std::cout << "Successfully triggered and read beacon" << std::endl;

        // Get PID list
        std::vector<uint32_t> pids;
        if (reader.GetPIDList(pids)) {
            std::cout << "Found " << pids.size() << " processes:" << std::endl;

            // Show first 10
            for (size_t i = 0; i < std::min<size_t>(10, pids.size()); i++) {
                BeaconProcessInfo info;
                if (reader.GetProcessInfo(pids[i], info)) {
                    std::cout << "  PID " << pids[i]
                             << ": " << info.name << std::endl;
                }
            }
        }
    } else {
        std::cerr << "Failed to trigger refresh" << std::endl;
    }

    // Test 2: Trigger with focus
    if (argc > 1) {
        uint32_t focusPid = atoi(argv[1]);
        std::cout << "\nTest 2: With focus on PID " << focusPid << std::endl;

        if (reader.TriggerRefresh(focusPid)) {
            std::cout << "Successfully triggered with focus" << std::endl;

            BeaconProcessInfo info;
            if (reader.GetProcessInfo(focusPid, info)) {
                std::cout << "Process " << focusPid << ": " << info.name << std::endl;
                std::cout << "  State: " << info.state << std::endl;
                std::cout << "  VSZ: " << info.vsize << " KB" << std::endl;
                std::cout << "  RSS: " << info.rss << " KB" << std::endl;
            }
        }
    }

    std::cout << "\nTest complete!" << std::endl;
    return 0;
}