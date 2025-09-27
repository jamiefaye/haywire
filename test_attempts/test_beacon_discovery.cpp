#include <iostream>
#include <memory>
#include "beacon_reader.h"
#include "beacon_decoder.h"

int main() {
    std::cout << "Testing Beacon Discovery with Enhanced Reporting\n";
    std::cout << "================================================\n\n";
    
    auto reader = std::make_unique<Haywire::BeaconReader>();
    
    // Initialize with memory file
    if (!reader->Initialize("/tmp/haywire-vm-mem")) {
        std::cerr << "Failed to initialize beacon reader\n";
        return 1;
    }
    
    // Find discovery page - this will show scanning progress
    std::cout << "Phase 1: Finding Discovery Page\n";
    std::cout << "--------------------------------\n";
    if (!reader->FindDiscovery()) {
        std::cerr << "Failed to find discovery page\n";
        return 1;
    }
    
    std::cout << "\nPhase 2: Getting PID List\n";
    std::cout << "-------------------------\n";
    std::vector<uint32_t> pids;
    if (reader->GetPIDList(pids)) {
        std::cout << "Got " << pids.size() << " PIDs from beacon\n";
    } else {
        std::cout << "No PID list available\n";
    }
    
    std::cout << "\nTest complete!\n";
    return 0;
}