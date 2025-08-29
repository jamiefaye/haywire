#include "qemu_connection.h"
#include "imgui.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

namespace Haywire {

QemuConnection::QemuConnection() 
    : qmpSocket(-1), monitorSocket(-1), connected(false), shouldStop(false),
      qmpPort(4445), monitorPort(4444), readSpeed(0.0f), totalBytesRead(0),
      inputQMPPort(4445), inputMonitorPort(4444), inputGDBPort(1234), useGDB(false) {
    strcpy(inputHost, "localhost");
    gdbConnection = std::make_unique<GDBConnection>();
}

QemuConnection::~QemuConnection() {
    Disconnect();
}

bool QemuConnection::ConnectQMP(const std::string& host, int port) {
    if (qmpSocket >= 0) {
        close(qmpSocket);
    }
    
    qmpSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (qmpSocket < 0) {
        return false;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    // Handle "localhost" specially
    if (host == "localhost") {
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    } else {
        addr.sin_addr.s_addr = inet_addr(host.c_str());
    }
    
    if (connect(qmpSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(qmpSocket);
        qmpSocket = -1;
        return false;
    }
    
    nlohmann::json greeting;
    char buffer[4096];
    int received = recv(qmpSocket, buffer, sizeof(buffer)-1, 0);
    if (received > 0) {
        buffer[received] = '\0';
        try {
            greeting = nlohmann::json::parse(buffer);
        } catch(...) {
            close(qmpSocket);
            qmpSocket = -1;
            return false;
        }
    }
    
    nlohmann::json capabilities = {
        {"execute", "qmp_capabilities"}
    };
    nlohmann::json response;
    if (!SendQMPCommand(capabilities, response)) {
        close(qmpSocket);
        qmpSocket = -1;
        return false;
    }
    
    connectionHost = host;
    qmpPort = port;
    
    return true;
}

bool QemuConnection::ConnectGDB(const std::string& host, int port) {
    if (gdbConnection && gdbConnection->Connect(host, port)) {
        useGDB = true;
        connectionHost = host;
        connected = true;
        return true;
    }
    return false;
}

bool QemuConnection::ConnectMonitor(const std::string& host, int port) {
    if (monitorSocket >= 0) {
        close(monitorSocket);
    }
    
    monitorSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (monitorSocket < 0) {
        return false;
    }
    
    // Set socket timeout to prevent blocking forever
    struct timeval timeout;
    timeout.tv_sec = 2;  // 2 second timeout
    timeout.tv_usec = 0;
    setsockopt(monitorSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(monitorSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    // Handle "localhost" specially
    if (host == "localhost") {
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    } else {
        addr.sin_addr.s_addr = inet_addr(host.c_str());
    }
    
    if (connect(monitorSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(monitorSocket);
        monitorSocket = -1;
        return false;
    }
    
    // Clear initial prompt
    char buffer[1024];
    recv(monitorSocket, buffer, sizeof(buffer), MSG_DONTWAIT);
    
    // Try to set monitor to not echo (this may not work on all QEMU versions)
    const char* noecho = "set echo off\n";
    send(monitorSocket, noecho, strlen(noecho), 0);
    usleep(10000); // 10ms wait
    recv(monitorSocket, buffer, sizeof(buffer), MSG_DONTWAIT); // Clear response
    
    monitorPort = port;
    connected = true;
    
    return true;
}

void QemuConnection::Disconnect() {
    connected = false;
    shouldStop = true;
    useGDB = false;
    
    if (receiveThread.joinable()) {
        receiveThread.join();
    }
    
    if (gdbConnection) {
        gdbConnection->Disconnect();
    }
    
    if (qmpSocket >= 0) {
        close(qmpSocket);
        qmpSocket = -1;
    }
    
    if (monitorSocket >= 0) {
        close(monitorSocket);
        monitorSocket = -1;
    }
}

bool QemuConnection::ReadMemory(uint64_t address, size_t size, std::vector<uint8_t>& buffer) {
    if (!connected) {
        return false;
    }
    
    // Use GDB for faster reads if available
    if (useGDB && gdbConnection && gdbConnection->IsConnected()) {
        return gdbConnection->ReadMemory(address, size, buffer);
    }
    
    // Fall back to monitor protocol
    if (monitorSocket < 0) {
        return false;
    }
    
    buffer.resize(size);
    size_t bytesRead = 0;
    
    while (bytesRead < size) {
        size_t chunkSize = std::min(size - bytesRead, size_t(4096));  // Larger chunks now that async works
        
        std::stringstream cmd;
        cmd << "xp/" << chunkSize << "xb 0x" << std::hex << (address + bytesRead);
        
        std::string response;
        if (!SendMonitorCommand(cmd.str(), response)) {
            // On timeout or error, return what we have
            buffer.resize(bytesRead);
            return bytesRead > 0;
        }
        
        // Parse the response for hex bytes
        size_t pos = 0;
        while (pos < response.length() && bytesRead < size) {
            // Look for hex pattern "0x" followed by 2 hex digits
            size_t hexPos = response.find("0x", pos);
            if (hexPos == std::string::npos) break;
            
            // Check if this is an address (has colon after) or a data byte
            size_t colonPos = response.find(':', hexPos);
            if (colonPos != std::string::npos && colonPos < hexPos + 20) {
                // This is an address line, skip to after colon
                pos = colonPos + 1;
                continue;
            }
            
            // Try to parse as hex byte
            char hex[3] = {0};
            if (hexPos + 3 < response.length()) {
                hex[0] = response[hexPos + 2];
                hex[1] = response[hexPos + 3];
                char* end;
                long val = strtol(hex, &end, 16);
                if (end != hex) {
                    buffer[bytesRead++] = static_cast<uint8_t>(val);
                }
            }
            pos = hexPos + 4;
        }
        
        // If we made no progress, bail out
        if (pos == 0) break;
    }
    
    UpdateReadSpeed(bytesRead);
    return true;
}

bool QemuConnection::SendQMPCommand(const nlohmann::json& command, nlohmann::json& response) {
    if (qmpSocket < 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(qmpMutex);
    
    std::string cmdStr = command.dump() + "\n";
    if (send(qmpSocket, cmdStr.c_str(), cmdStr.length(), 0) < 0) {
        return false;
    }
    
    char buffer[4096];
    int received = recv(qmpSocket, buffer, sizeof(buffer)-1, 0);
    if (received > 0) {
        buffer[received] = '\0';
        try {
            response = nlohmann::json::parse(buffer);
            return true;
        } catch(...) {
            return false;
        }
    }
    
    return false;
}

bool QemuConnection::SendMonitorCommand(const std::string& command, std::string& response) {
    if (monitorSocket < 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(monitorMutex);
    
    // Send command
    std::string cmdStr = command + "\n";
    if (send(monitorSocket, cmdStr.c_str(), cmdStr.length(), 0) < 0) {
        return false;
    }
    
    // Read response (with multiple attempts for slow commands)
    char buffer[8192];
    response.clear();
    int totalReceived = 0;
    int attempts = 0;
    
    while (attempts < 20 && totalReceived < sizeof(buffer) - 1) {
        usleep(1000);  // 1ms between attempts
        
        int received = recv(monitorSocket, buffer + totalReceived, 
                          sizeof(buffer) - totalReceived - 1, MSG_DONTWAIT);
        if (received > 0) {
            totalReceived += received;
            buffer[totalReceived] = '\0';
            
            // Check if we have a complete response (ends with prompt)
            if (strstr(buffer, "(qemu)") != nullptr) {
                break;
            }
        } else if (received == 0) {
            break;  // Connection closed
        }
        attempts++;
    }
    
    if (totalReceived > 0) {
        buffer[totalReceived] = '\0';
        response = buffer;
        
        // Remove the echoed command - look for our command in the response
        size_t cmdPos = response.find(command);
        if (cmdPos != std::string::npos) {
            // Skip past the command and its newline
            size_t startPos = cmdPos + command.length();
            if (startPos < response.length() && response[startPos] == '\r') startPos++;
            if (startPos < response.length() && response[startPos] == '\n') startPos++;
            response = response.substr(startPos);
        }
        
        // Remove any leading whitespace
        size_t firstNonSpace = response.find_first_not_of(" \t\r\n");
        if (firstNonSpace != std::string::npos && firstNonSpace > 0) {
            response = response.substr(firstNonSpace);
        }
        
        // Remove trailing prompt
        size_t promptPos = response.find("(qemu)");
        if (promptPos != std::string::npos) {
            response = response.substr(0, promptPos);
        }
        
        // Trim trailing whitespace
        size_t lastNonSpace = response.find_last_not_of(" \t\r\n");
        if (lastNonSpace != std::string::npos) {
            response = response.substr(0, lastNonSpace + 1);
        }
        
        return true;
    }
    
    return false;
}

void QemuConnection::UpdateReadSpeed(size_t bytesRead) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - lastReadTime).count();
    
    if (elapsed > 1.0f) {
        readSpeed = (totalBytesRead / elapsed) / (1024.0f * 1024.0f);
        totalBytesRead = 0;
        lastReadTime = now;
    } else {
        totalBytesRead += bytesRead;
    }
}

void QemuConnection::DrawConnectionUI() {
    ImGui::Text("QEMU Connection Settings");
    ImGui::Separator();
    
    ImGui::InputText("Host", inputHost, sizeof(inputHost));
    ImGui::InputInt("QMP Port", &inputQMPPort);
    ImGui::InputInt("Monitor Port", &inputMonitorPort);
    ImGui::InputInt("GDB Port", &inputGDBPort);
    
    if (!connected) {
        if (ImGui::Button("Connect (Monitor)")) {
            bool qmpSuccess = ConnectQMP(inputHost, inputQMPPort);
            bool monSuccess = ConnectMonitor(inputHost, inputMonitorPort);
            
            if (!qmpSuccess && !monSuccess) {
                ImGui::OpenPopup("Connection Failed");
            }
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Connect (GDB - Fast)")) {
            bool gdbSuccess = ConnectGDB(inputHost, inputGDBPort);
            
            if (gdbSuccess) {
                // Also try QMP for status queries
                ConnectQMP(inputHost, inputQMPPort);
            } else {
                ImGui::OpenPopup("GDB Connection Failed");
            }
        }
    } else {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Connected %s", useGDB ? "(GDB)" : "(Monitor)");
        if (useGDB) {
            ImGui::TextColored(ImVec4(0, 1, 1, 1), "Using fast GDB memory access");
        }
        if (ImGui::Button("Disconnect")) {
            Disconnect();
        }
    }
    
    if (ImGui::BeginPopupModal("Connection Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Failed to connect to QEMU.");
        ImGui::Text("Make sure QEMU is running with:");
        ImGui::Text("  -qmp tcp:localhost:4445,server,nowait");
        ImGui::Text("  -monitor telnet:localhost:4444,server,nowait");
        
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    if (ImGui::BeginPopupModal("GDB Connection Failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Failed to connect to GDB server.");
        ImGui::Text("Make sure QEMU is running with:");
        ImGui::Text("  -gdb tcp::1234");
        ImGui::Text("");
        ImGui::Text("Use launch_ubuntu_arm64_gdb.sh for GDB support");
        
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool QemuConnection::QueryStatus(nlohmann::json& status) {
    nlohmann::json cmd = {{"execute", "query-status"}};
    return SendQMPCommand(cmd, status);
}

bool QemuConnection::QueryMemoryRegions(std::vector<std::pair<uint64_t, uint64_t>>& regions) {
    return false;
}

void QemuConnection::QMPReceiveThread() {
}

}