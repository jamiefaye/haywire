#include <iostream>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

// Beacon header structure from beacon_protocol.h
struct BeaconPage {
    uint32_t magic;
    uint32_t version_top;
    uint32_t session_id;
    uint32_t category;
    uint32_t category_index;
    uint32_t timestamp;
    uint32_t sequence;
    uint32_t data_size;
    uint32_t version_bottom;
    uint8_t data[4060];
} __attribute__((packed));

// PID entry structure from companion
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

std::string executeSSH(const std::string& command) {
    std::string sshCmd = "ssh vm '" + command + "' 2>/dev/null";

    FILE* pipe = popen(sshCmd.c_str(), "r");
    if (!pipe) return "";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        result += buffer;
    }

    pclose(pipe);
    return result;
}

bool parseCompanionOutput(const std::string& output, uint64_t& address, size_t& size) {
    // Parse output looking for "Master: 0xADDRESS"
    const char* masterStr = strstr(output.c_str(), "Master: 0x");
    if (masterStr) {
        masterStr += 10; // Skip "Master: 0x"
        address = strtoull(masterStr, nullptr, 16);
        size = 417 * 4096; // Total pages from companion_oneshot
        return true;
    }
    return false;
}

int main() {
    std::cout << "Simple Triggered Beacon Test\n";
    std::cout << "============================\n\n";

    // Step 1: Trigger companion
    uint32_t magic = 0xAABBCCDD;
    std::string cmd = "./companion_oneshot --once --request=0x";
    char hexMagic[16];
    snprintf(hexMagic, sizeof(hexMagic), "%08x", magic);
    cmd += hexMagic;

    std::cout << "Triggering companion with magic: 0x" << hexMagic << "\n";
    std::string output = executeSSH(cmd);
    std::cout << "Companion output: " << output << "\n";

    // Step 2: Parse output
    uint64_t beaconAddr;
    size_t beaconSize;

    if (!parseCompanionOutput(output, beaconAddr, beaconSize)) {
        std::cerr << "Failed to parse companion output\n";
        return 1;
    }

    std::cout << "Beacon at: 0x" << std::hex << beaconAddr
              << " size: " << std::dec << beaconSize << " bytes\n\n";

    // Step 3: Open memory file and scan for beacon
    int memFd = open("/tmp/haywire-vm-mem", O_RDONLY);
    if (memFd < 0) {
        std::cerr << "Failed to open memory file\n";
        return 1;
    }

    // Get file size
    struct stat st;
    fstat(memFd, &st);
    std::cout << "Memory file size: " << st.st_size << " bytes\n";

    // Step 4: Scan for our beacon by magic
    std::cout << "Scanning for beacon with session_id 0x" << std::hex << magic << "...\n";

    BeaconPage* hdr = nullptr;
    void* mapped = nullptr;
    size_t mappedSize = 0;

    // Scan in chunks
    const size_t chunkSize = 128 * 1024 * 1024; // 128MB chunks
    bool found = false;
    const uint32_t BEACON_MAGIC = 0xBEAC0042;

    for (size_t offset = 0; offset < st.st_size && !found; offset += chunkSize) {
        size_t mapSize = std::min(chunkSize, (size_t)(st.st_size - offset));

        void* chunk = mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE,
                          memFd, offset);
        if (chunk == MAP_FAILED) continue;

        // Scan this chunk for beacon
        uint8_t* ptr = (uint8_t*)chunk;
        for (size_t i = 0; i < mapSize - sizeof(BeaconPage); i += 4096) {
            BeaconPage* candidate = (BeaconPage*)(ptr + i);

            if (candidate->magic == BEACON_MAGIC &&
                candidate->session_id == magic &&
                candidate->category == 0) { // BEACON_CATEGORY_MASTER

                std::cout << "Found master beacon at offset 0x" << std::hex << (offset + i) << "\n";

                // Keep this mapping
                hdr = candidate;
                mapped = chunk;
                mappedSize = mapSize;
                found = true;
                break;
            }
        }

        if (!found) {
            munmap(chunk, mapSize);
        }
    }

    if (!found) {
        std::cerr << "Beacon not found in memory!\n";
        close(memFd);
        return 1;
    }

    std::cout << "Beacon Header:\n";
    std::cout << "  Magic: 0x" << std::hex << hdr->magic << "\n";
    std::cout << "  Session ID: 0x" << std::hex << hdr->session_id << "\n";
    std::cout << "  Category: " << std::dec << hdr->category << "\n";
    std::cout << "  Timestamp: " << hdr->timestamp << "\n\n";

    // Verify magic
    if (hdr->magic != BEACON_MAGIC) {
        std::cerr << "Invalid beacon magic!\n";
        munmap(mapped, mappedSize);
        close(memFd);
        return 1;
    }

    if (hdr->session_id != magic) {
        std::cerr << "Session ID mismatch!\n";
        munmap(mapped, mappedSize);
        close(memFd);
        return 1;
    }

    // Step 5: Look for PID pages
    std::cout << "Looking for PID pages...\n";

    // Scan for PID pages (category 1)
    uint8_t* ptr = (uint8_t*)mapped;
    for (size_t i = 0; i < mappedSize; i += 4096) {
        BeaconPage* page = (BeaconPage*)(ptr + i);
        if (page->magic == BEACON_MAGIC &&
            page->session_id == magic &&
            page->category == 1) { // BEACON_CATEGORY_PID

            std::cout << "Found PID page at offset " << i << "\n";
            // The data contains BeaconPIDEntry structures
            // Just show we found it
            break;
        }
    }

    // Cleanup
    munmap(mapped, mappedSize);
    close(memFd);

    std::cout << "\nTest successful!\n";
    return 0;
}