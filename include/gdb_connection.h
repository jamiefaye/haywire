#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

namespace Haywire {

class GDBConnection {
public:
    GDBConnection();
    ~GDBConnection();
    
    bool Connect(const std::string& host = "localhost", int port = 1234);
    void Disconnect();
    bool IsConnected() const { return connected; }
    
    // Fast binary memory read
    bool ReadMemory(uint64_t address, size_t size, std::vector<uint8_t>& buffer);
    
    // Read registers
    bool ReadRegisters(std::string& registers);
    
private:
    // GDB Remote Serial Protocol helpers
    std::string BuildPacket(const std::string& data);
    bool SendPacket(const std::string& packet);
    bool ReceivePacket(std::string& response);
    uint8_t CalculateChecksum(const std::string& data);
    bool DecodeHex(const std::string& hex, std::vector<uint8_t>& binary);
    std::string EncodeHex(uint64_t value);
    
    int socket;
    bool connected;
    std::mutex socketMutex;
    
    char recvBuffer[65536]; // Large buffer for memory reads
};

}