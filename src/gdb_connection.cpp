#include "gdb_connection.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

namespace Haywire {

GDBConnection::GDBConnection() : socket(-1), connected(false) {
}

GDBConnection::~GDBConnection() {
    Disconnect();
}

bool GDBConnection::Connect(const std::string& host, int port) {
    if (socket >= 0) {
        close(socket);
    }
    
    socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        return false;
    }
    
    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (host == "localhost") {
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    } else {
        addr.sin_addr.s_addr = inet_addr(host.c_str());
    }
    
    if (connect(socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(socket);
        socket = -1;
        return false;
    }
    
    // Send initial ACK
    const char* ack = "+";
    send(socket, ack, 1, 0);
    
    // Try to attach
    std::string response;
    SendPacket("qSupported");
    ReceivePacket(response); // Ignore response
    
    SendPacket("qAttached");
    ReceivePacket(response);
    
    // Don't send continue here - we'll do stop/read/continue for each memory read
    
    connected = true;
    return true;
}

void GDBConnection::Disconnect() {
    connected = false;
    if (socket >= 0) {
        close(socket);
        socket = -1;
    }
}

bool GDBConnection::ReadMemory(uint64_t address, size_t size, std::vector<uint8_t>& buffer) {
    if (!connected || socket < 0) {
        std::cerr << "GDB: Not connected\n";
        return false;
    }
    
    std::lock_guard<std::mutex> lock(socketMutex);
    
    buffer.clear();
    buffer.reserve(size);
    
    std::cerr << "GDB: Reading " << size << " bytes from 0x" << std::hex << address << std::dec << "\n";
    
    // Note: This implementation requires the VM to be halted
    // For now, skip the interrupt - user should use monitor protocol for live reading
    
    // GDB can read up to about 16KB at a time
    const size_t maxChunk = 16384;
    size_t bytesRead = 0;
    
    while (bytesRead < size) {
        size_t chunkSize = std::min(size - bytesRead, maxChunk);
        
        // Build memory read command: m<addr>,<length>
        std::stringstream cmd;
        cmd << "m" << std::hex << (address + bytesRead) << "," << chunkSize;
        
        if (!SendPacket(cmd.str())) {
            return false;
        }
        
        std::string response;
        if (!ReceivePacket(response)) {
            return false;
        }
        
        // Check for error response
        if (response.empty()) {
            std::cerr << "GDB: Empty response\n";
            return false;
        }
        if (response[0] == 'E') {
            std::cerr << "GDB: Error response: " << response << "\n";
            return false;
        }
        
        // Decode hex response
        std::vector<uint8_t> chunk;
        if (!DecodeHex(response, chunk)) {
            return false;
        }
        
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());
        bytesRead += chunk.size();
        
        // If we got less than requested, stop
        if (chunk.size() < chunkSize) {
            break;
        }
    }
    
    // Don't resume - let user control VM state
    
    return !buffer.empty();
}

bool GDBConnection::ReadRegisters(std::string& registers) {
    if (!connected) return false;
    
    std::lock_guard<std::mutex> lock(socketMutex);
    
    if (!SendPacket("g")) {
        return false;
    }
    
    return ReceivePacket(registers);
}

std::string GDBConnection::BuildPacket(const std::string& data) {
    uint8_t checksum = CalculateChecksum(data);
    std::stringstream packet;
    packet << "$" << data << "#" << std::hex << std::setw(2) << std::setfill('0') << (int)checksum;
    return packet.str();
}

bool GDBConnection::SendPacket(const std::string& data) {
    std::string packet = BuildPacket(data);
    
    if (send(socket, packet.c_str(), packet.length(), 0) < 0) {
        return false;
    }
    
    // Wait for ACK
    char ack;
    if (recv(socket, &ack, 1, 0) != 1) {
        return false;
    }
    
    return ack == '+';
}

bool GDBConnection::ReceivePacket(std::string& response) {
    response.clear();
    
    // Read until we get a complete packet
    int totalReceived = 0;
    bool inPacket = false;
    int checksumCount = 0;
    
    while (totalReceived < sizeof(recvBuffer) - 1) {
        int received = recv(socket, recvBuffer + totalReceived, 1, 0);
        if (received <= 0) {
            break;
        }
        
        char ch = recvBuffer[totalReceived];
        totalReceived++;
        
        if (ch == '$') {
            inPacket = true;
            response.clear();
        } else if (inPacket) {
            if (ch == '#') {
                checksumCount = 0;
            } else if (checksumCount >= 0) {
                checksumCount++;
                if (checksumCount == 2) {
                    // Send ACK
                    const char* ack = "+";
                    send(socket, ack, 1, 0);
                    return true;
                }
            } else {
                response += ch;
            }
        }
    }
    
    return false;
}

uint8_t GDBConnection::CalculateChecksum(const std::string& data) {
    uint8_t sum = 0;
    for (char c : data) {
        sum += (uint8_t)c;
    }
    return sum;
}

bool GDBConnection::DecodeHex(const std::string& hex, std::vector<uint8_t>& binary) {
    binary.clear();
    
    if (hex.length() % 2 != 0) {
        return false;
    }
    
    for (size_t i = 0; i < hex.length(); i += 2) {
        char hexByte[3] = { hex[i], hex[i+1], '\0' };
        char* end;
        long val = strtol(hexByte, &end, 16);
        if (end != hexByte + 2) {
            return false;
        }
        binary.push_back(static_cast<uint8_t>(val));
    }
    
    return true;
}

std::string GDBConnection::EncodeHex(uint64_t value) {
    std::stringstream ss;
    ss << std::hex << value;
    return ss.str();
}

}