#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include "beacon_reader.h"

namespace Haywire {

/**
 * Triggered beacon reader for single-shot companion mode
 *
 * Instead of continuous scanning, this:
 * 1. Triggers companion via SSH/QGA
 * 2. Gets beacon location from output
 * 3. Reads beacon directly (no scanning needed)
 */
class TriggeredBeaconReader : public BeaconReader {
public:
    TriggeredBeaconReader();
    ~TriggeredBeaconReader();

    // Initialize with VM connection details
    bool InitializeTriggered(const std::string& memoryPath = "/tmp/haywire-vm-mem",
                            const std::string& vmHost = "localhost",
                            int vmPort = 2222);

    // Trigger companion and refresh process list
    bool TriggerRefresh(uint32_t focusPid = 0);

    // Get last trigger's magic number
    uint32_t GetLastRequestMagic() const { return lastRequestMagic; }

    // Check if companion is installed in VM
    bool IsCompanionInstalled();

    // Install companion via SSH (compile from source)
    bool InstallCompanion();

private:
    // Execute command in VM via SSH
    std::string ExecuteSSHCommand(const std::string& command);

    // Parse companion output to get beacon location
    bool ParseCompanionOutput(const std::string& output,
                             uint64_t& address,
                             size_t& size);

    // Find beacon by magic number (faster than full scan)
    bool FindTriggeredBeacon(uint32_t magic,
                            uint64_t hintAddress = 0,
                            size_t hintSize = 0);

    // SSH connection details
    std::string vmHost;
    int vmPort;
    std::string companionPath = "/home/ubuntu/companion_triggered";

    // Request tracking
    uint32_t requestCounter = 0x10000000;
    uint32_t lastRequestMagic = 0;

    // Cache beacon location for faster subsequent reads
    uint64_t lastBeaconAddress = 0;
    size_t lastBeaconSize = 0;
};

} // namespace Haywire