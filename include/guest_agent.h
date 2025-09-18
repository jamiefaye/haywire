#pragma once

#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>

namespace Haywire {

struct GuestProcessInfo {
    int pid;
    std::string name;
    std::string user;
    float cpu;
    float mem;
    std::string command;
    
    enum Category {
        USER_APP,      // GUI apps, browsers, games
        SERVICE,       // systemd services, daemons
        KERNEL_THREAD, // [kernel] processes
        SYSTEM_UTIL    // system utilities
    };
    Category category;
    
    // Helper to categorize process
    void Categorize() {
        // Kernel threads have names in brackets
        if (!name.empty() && name[0] == '[') {
            category = KERNEL_THREAD;
        }
        // High memory usage typically means user app
        else if (mem > 1.0) {
            category = USER_APP;
        }
        // Known user applications
        else if (name.find("vlc") != std::string::npos ||
                 name.find("firefox") != std::string::npos ||
                 name.find("chrome") != std::string::npos ||
                 name.find("gnome") != std::string::npos ||
                 name.find("kde") != std::string::npos ||
                 name.find("X") == 0 ||
                 name.find("wayland") != std::string::npos) {
            category = USER_APP;
        }
        // Services/daemons
        else if (name.find("systemd") != std::string::npos ||
                 name.find("daemon") != std::string::npos ||
                 name.find("d") == name.length() - 1 ||  // ends with 'd'
                 user == "root") {
            category = SERVICE;
        }
        else {
            category = SYSTEM_UTIL;
        }
    }
};

struct GuestMemoryRegion {
    uint64_t start;
    uint64_t end;
    std::string permissions;
    std::string name;
};

struct PagemapEntry {
    uint64_t pfn;      // Page Frame Number (physical page number)
    bool present;      // Page is present in RAM
    bool swapped;      // Page is swapped out
    uint64_t physAddr; // Calculated physical address
};

class GuestAgent {
public:
    GuestAgent();
    ~GuestAgent();
    
    bool Connect(const std::string& socketPath = "/tmp/qga.sock");
    void Disconnect();
    bool IsConnected() const { return sock >= 0; }
    
    // Guest agent commands
    bool Ping();
    bool GetProcessList(std::vector<GuestProcessInfo>& processes);
    bool GetMemoryMap(int pid, std::vector<GuestMemoryRegion>& regions);
    bool ExecuteCommand(const std::string& command, std::string& output);
    
    // VA to PA translation via pagemap
    bool TranslateAddress(int pid, uint64_t virtualAddr, PagemapEntry& entry);
    bool TranslateRange(int pid, uint64_t startVA, size_t length, 
                       std::vector<PagemapEntry>& entries);
    
    // Get page table base registers for a process (ARM64)
    struct TTBRValues {
        uint64_t ttbr0_el1;
        uint64_t ttbr1_el1;
        uint64_t tcr_el1;  // Translation Control Register
        bool valid;
    };
    bool GetTTBR(int pid, TTBRValues& ttbr);
    
    // Helper to decode base64
    static std::string DecodeBase64(const std::string& encoded);
    
private:
    int sock;
    
    bool SendCommand(const std::string& cmd, std::string& response);
    bool ParseProcessList(const std::string& psOutput, std::vector<GuestProcessInfo>& processes);
    bool ParseMemoryMap(const std::string& mapsOutput, std::vector<GuestMemoryRegion>& regions);
};

}