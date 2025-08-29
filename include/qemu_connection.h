#pragma once

#include "common.h"
#include "gdb_connection.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <memory>
#include <nlohmann/json.hpp>

namespace Haywire {

class QemuConnection {
public:
    QemuConnection();
    ~QemuConnection();
    
    bool ConnectQMP(const std::string& host = "localhost", int port = 4445);
    bool ConnectMonitor(const std::string& host = "localhost", int port = 4444);
    bool ConnectGDB(const std::string& host = "localhost", int port = 1234);
    void Disconnect();
    
    bool IsConnected() const { return connected.load(); }
    
    bool ReadMemory(uint64_t address, size_t size, std::vector<uint8_t>& buffer);
    
    bool QueryStatus(nlohmann::json& status);
    bool QueryMemoryRegions(std::vector<std::pair<uint64_t, uint64_t>>& regions);
    
    void DrawConnectionUI();
    
    float GetReadSpeed() const { return readSpeed.load(); }
    
private:
    bool SendQMPCommand(const nlohmann::json& command, nlohmann::json& response);
    bool SendMonitorCommand(const std::string& command, std::string& response);
    
    void QMPReceiveThread();
    void UpdateReadSpeed(size_t bytesRead);
    
    int qmpSocket;
    int monitorSocket;
    std::atomic<bool> connected;
    std::atomic<bool> shouldStop;
    
    std::thread receiveThread;
    std::mutex qmpMutex;
    std::mutex monitorMutex;
    std::queue<nlohmann::json> qmpResponses;
    
    std::string connectionHost;
    int qmpPort;
    int monitorPort;
    
    std::atomic<float> readSpeed;
    std::chrono::steady_clock::time_point lastReadTime;
    size_t totalBytesRead;
    
    char inputHost[256];
    int inputQMPPort;
    int inputMonitorPort;
    int inputGDBPort;
    
    // GDB connection for faster memory reads
    std::unique_ptr<GDBConnection> gdbConnection;
    bool useGDB;
};

}