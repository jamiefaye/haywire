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

struct ProcessInfo {
    int pid;
    std::string name;
    std::string user;
    float cpu;
    float mem;
    std::string command;
};

struct GuestMemoryRegion {
    uint64_t start;
    uint64_t end;
    std::string permissions;
    std::string name;
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
    bool GetProcessList(std::vector<ProcessInfo>& processes);
    bool GetMemoryMap(int pid, std::vector<GuestMemoryRegion>& regions);
    bool ExecuteCommand(const std::string& command, std::string& output);
    
    // Helper to decode base64
    static std::string DecodeBase64(const std::string& encoded);
    
private:
    int sock;
    
    bool SendCommand(const std::string& cmd, std::string& response);
    bool ParseProcessList(const std::string& psOutput, std::vector<ProcessInfo>& processes);
    bool ParseMemoryMap(const std::string& mapsOutput, std::vector<GuestMemoryRegion>& regions);
};

}