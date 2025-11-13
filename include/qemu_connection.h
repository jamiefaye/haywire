#pragma once

#include "common.h"
#include "gdb_connection.h"
#include "mmap_reader.h"
#include "memory_backend.h"
#include "guest_agent.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <memory>
#include <nlohmann/json.hpp>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    // Define SOCKET type and invalid value for Windows
    typedef SOCKET SocketType;
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
    #define SOCKET_ERROR_VALUE SOCKET_ERROR
#else
    // Define SOCKET type and invalid value for POSIX
    typedef int SocketType;
    #define INVALID_SOCKET_VALUE -1
    #define SOCKET_ERROR_VALUE -1
#endif

namespace Haywire {

class QemuConnection {
private:
    // Private constructor for singleton
    QemuConnection();

public:
    // Get singleton instance
    static QemuConnection& getInstance() {
        static QemuConnection instance;
        return instance;
    }

    // Delete copy constructor and assignment operator
    QemuConnection(const QemuConnection&) = delete;
    QemuConnection& operator=(const QemuConnection&) = delete;

    ~QemuConnection();
    
    bool ConnectQMP(const std::string& host = "localhost", int port = 4445);
    bool ConnectMonitor(const std::string& host = "localhost", int port = 4444);
    bool ConnectGDB(const std::string& host = "localhost", int port = 1234);
    void Disconnect();
    
    // Auto-connect with smart fallback
    bool AutoConnect();
    
    bool IsConnected() const { return connected.load(); }
    bool IsAvailable() const { return IsConnected(); }
    bool IsQMPConnected() const { return qmpSocket != INVALID_SOCKET_VALUE; }
    bool IsUsingMemoryBackend() const { return useMemoryBackend; }
    MemoryBackend* GetMemoryBackend() { return memoryBackend.get(); }
    bool IsGuestAgentConnected() const { return guestAgent && guestAgent->IsConnected(); }
    
    bool ReadMemory(uint64_t address, size_t size, std::vector<uint8_t>& buffer);
    bool ReadMemoryMMap(uint64_t address, size_t size, std::vector<uint8_t>& buffer);
    bool ReadMemoryDirect(uint64_t address, size_t size, std::vector<uint8_t>& buffer);  // Bypass memory backend, use monitor directly
    bool ReadMemoryViaQMP(uint64_t address, size_t size, std::vector<uint8_t>& buffer);  // Use QMP human-monitor-command (most reliable)

    // Test if a memory page contains any non-zero bytes (zero-copy when possible)
    bool TestPageNonZero(uint64_t address, size_t size = 4096);
    
    // Guest agent access
    GuestAgent* GetGuestAgent() { return guestAgent.get(); }
    std::shared_ptr<GuestAgent> GetGuestAgentPtr() { return guestAgent; }
    
    // VA->PA translation via QMP (fast path)
    bool TranslateVA2PA(int cpuIndex, uint64_t virtualAddr, uint64_t& physicalAddr);
    
    bool QueryStatus(nlohmann::json& status);
    bool QueryMemoryRegions(std::vector<std::pair<uint64_t, uint64_t>>& regions);

    // Query kernel info (swapper PGD, current task, etc.)
    bool QueryKernelInfo(int cpuIndex, uint64_t& swapperPgd, uint64_t& currentTask);

    // Query CPU registers (x86_64: returns CR3, ARM64: returns TTBR0/TTBR1)
    bool QueryCR3(int cpuIndex, uint64_t& cr3);

    // Public for MMapReader
    bool SendQMPCommand(const nlohmann::json& command, nlohmann::json& response);
    
    void DrawConnectionUI();
    
    float GetReadSpeed() const { return readSpeed.load(); }
    
private:
    bool SendMonitorCommand(const std::string& command, std::string& response);
    
    void QMPReceiveThread();
    void UpdateReadSpeed(size_t bytesRead);

    // Cross-platform socket initialization/cleanup helpers
    bool InitializeSockets();
    void CleanupSockets();
    void CloseSocket(SocketType& sock);

    SocketType qmpSocket;
    SocketType monitorSocket;
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
    
    // MMap reader for fastest memory access
    std::unique_ptr<MMapReader> mmapReader;
    bool useMMap;
    
    // Direct memory backend for zero-copy access
    std::unique_ptr<MemoryBackend> memoryBackend;
    bool useMemoryBackend;
    
    // Guest agent for introspection
    std::shared_ptr<GuestAgent> guestAgent;
};

}