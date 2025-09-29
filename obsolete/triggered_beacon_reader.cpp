#include "triggered_beacon_reader.h"
#include "beacon_protocol.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

namespace Haywire {

TriggeredBeaconReader::TriggeredBeaconReader() {
}

TriggeredBeaconReader::~TriggeredBeaconReader() {
    Cleanup();
}

bool TriggeredBeaconReader::InitializeTriggered(const std::string& memoryPath,
                                               const std::string& host,
                                               int port) {
    // Initialize base reader with memory file
    if (!Initialize(memoryPath)) {
        return false;
    }

    vmHost = host;
    vmPort = port;

    // Check if companion is installed
    if (!IsCompanionInstalled()) {
        std::cerr << "Triggered companion not installed in VM" << std::endl;
        std::cerr << "Run InstallCompanion() to compile it in VM" << std::endl;
        // Don't fail - allow manual installation
    }

    return true;
}

bool TriggeredBeaconReader::TriggerRefresh(uint32_t focusPid) {
    // Generate unique magic for this request
    lastRequestMagic = requestCounter++;

    // Build command
    std::stringstream cmd;
    cmd << companionPath << " --request=0x"
        << std::hex << lastRequestMagic;

    if (focusPid > 0) {
        cmd << " --focus=" << std::dec << focusPid;
    }

    // Execute companion
    std::string output = ExecuteSSHCommand(cmd.str());
    if (output.empty()) {
        std::cerr << "Failed to execute triggered companion" << std::endl;
        return false;
    }

    // Parse output to get beacon location
    uint64_t address;
    size_t size;
    if (!ParseCompanionOutput(output, address, size)) {
        std::cerr << "Failed to parse companion output: " << output << std::endl;
        return false;
    }

    std::cout << "Triggered beacon at 0x" << std::hex << address
              << " size=" << std::dec << size
              << " magic=0x" << std::hex << lastRequestMagic << std::endl;

    // Cache location
    lastBeaconAddress = address;
    lastBeaconSize = size;

    // Find and read the beacon
    return FindTriggeredBeacon(lastRequestMagic, address, size);
}

bool TriggeredBeaconReader::IsCompanionInstalled() {
    std::string result = ExecuteSSHCommand("test -x " + companionPath + " && echo yes || echo no");
    return (result.find("yes") != std::string::npos);
}

bool TriggeredBeaconReader::InstallCompanion() {
    std::cout << "Installing triggered companion in VM..." << std::endl;

    // Source code is already there from our scp
    std::string result = ExecuteSSHCommand(
        "gcc -O2 -o " + companionPath + " companion_triggered.c 2>&1"
    );

    if (result.find("error") != std::string::npos) {
        std::cerr << "Compilation failed: " << result << std::endl;
        return false;
    }

    std::cout << "Companion installed successfully" << std::endl;
    return true;
}

std::string TriggeredBeaconReader::ExecuteSSHCommand(const std::string& command) {
    // Build SSH command
    std::stringstream sshCmd;
    sshCmd << "ssh -p " << vmPort << " ubuntu@" << vmHost
           << " '" << command << "' 2>/dev/null";

    // Execute via popen
    FILE* pipe = popen(sshCmd.str().c_str(), "r");
    if (!pipe) {
        return "";
    }

    // Read output
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }

    pclose(pipe);
    return result;
}

bool TriggeredBeaconReader::ParseCompanionOutput(const std::string& output,
                                                uint64_t& address,
                                                size_t& size) {
    // Parse: BEACON_READY:0xADDRESS:SIZE:12345:MAGIC:deadbeef:PAGES:4
    std::stringstream ss(output);
    std::string line;

    while (std::getline(ss, line)) {
        if (line.find("BEACON_READY:") == 0) {
            // Parse the line
            const char* str = line.c_str() + 13; // Skip "BEACON_READY:"

            // Parse address (hex)
            char* end;
            address = strtoull(str, &end, 16);

            // Find SIZE
            const char* sizeStr = strstr(end, "SIZE:");
            if (sizeStr) {
                sizeStr += 5;
                size = strtoul(sizeStr, nullptr, 10);
                return true;
            }
        }
    }

    return false;
}

// Triggered beacon header structure (matches companion_triggered.c)
struct triggered_beacon_header {
    uint32_t magic1;
    uint32_t magic2;
    uint16_t observer_type;
    uint16_t page_count;
    uint32_t request_id;
    uint32_t timestamp;
    uint32_t entry_count;
    uint32_t focus_pid;
    uint32_t data_offset;
    uint8_t reserved[8];
} __attribute__((packed));

bool TriggeredBeaconReader::FindTriggeredBeacon(uint32_t magic,
                                               uint64_t hintAddress,
                                               size_t hintSize) {
    if (!memFd) {
        return false;
    }

    // If we have a hint address, try there first
    if (hintAddress && hintSize) {
        // Map the specific region
        void* mapped = mmap(nullptr, hintSize,
                          PROT_READ, MAP_PRIVATE,
                          memFd, hintAddress);

        if (mapped != MAP_FAILED) {
            // Check magic at start
            triggered_beacon_header* hdr = (triggered_beacon_header*)mapped;

            if (hdr->magic1 == 0x3142FACE &&
                hdr->magic2 == 0xCAFEBABE &&
                hdr->request_id == magic) {

                std::cout << "Found triggered beacon at hint address!" << std::endl;
                std::cout << "  Entry count: " << hdr->entry_count << std::endl;
                std::cout << "  Focus PID: " << hdr->focus_pid << std::endl;

                // Process the PID entries
                uint8_t* data = (uint8_t*)mapped + hdr->data_offset;

                // For now, just parse and print some PIDs
                for (uint32_t i = 0; i < std::min(10u, hdr->entry_count); i++) {
                    // Assuming PID entry structure from companion
                    struct pid_entry {
                        uint8_t entry_type;
                        uint8_t name_len;
                        uint16_t entry_size;
                        uint32_t pid;
                        uint32_t ppid;
                        uint32_t uid;
                        uint32_t vsize;
                        uint32_t rss;
                        char name[32];
                    } __attribute__((packed));

                    pid_entry* entry = (pid_entry*)(data + i * sizeof(pid_entry));
                    std::cout << "    PID " << entry->pid << ": " << entry->name << std::endl;
                }

                munmap(mapped, hintSize);
                return true;
            }

            munmap(mapped, hintSize);
        }
    }

    // Fall back to scanning (shouldn't be needed with good hint)
    std::cerr << "Warning: Hint address didn't contain beacon, scanning..." << std::endl;

    // Scan memory for our specific magic
    const size_t scanChunk = 1024 * 1024; // 1MB chunks

    for (uint64_t offset = 0; offset < memSize; offset += scanChunk) {
        size_t size = std::min(scanChunk, memSize - offset);

        void* mapped = mmap(nullptr, size,
                          PROT_READ, MAP_PRIVATE,
                          memFd, offset);

        if (mapped == MAP_FAILED) continue;

        // Scan this chunk
        uint8_t* ptr = (uint8_t*)mapped;
        for (size_t i = 0; i < size - sizeof(triggered_beacon_header); i += 4096) {
            triggered_beacon_header* hdr = (triggered_beacon_header*)(ptr + i);

            if (hdr->magic1 == 0x3142FACE &&
                hdr->magic2 == 0xCAFEBABE &&
                hdr->request_id == magic) {

                std::cout << "Found beacon via scan at 0x"
                         << std::hex << (offset + i) << std::endl;

                // Process it
                // [Same processing as above]

                munmap(mapped, size);
                return true;
            }
        }

        munmap(mapped, size);
    }

    return false;
}

} // namespace Haywire