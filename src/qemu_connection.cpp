#include "qemu_connection.h"
#include "imgui.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

namespace Haywire {

QemuConnection::QemuConnection() 
    : qmpSocket(-1), monitorSocket(-1), connected(false), shouldStop(false),
      qmpPort(4445), monitorPort(4444), readSpeed(0.0f), totalBytesRead(0),
      inputQMPPort(4445), inputMonitorPort(4444) {
    strcpy(inputHost, "localhost");
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
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    
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

bool QemuConnection::ConnectMonitor(const std::string& host, int port) {
    if (monitorSocket >= 0) {
        close(monitorSocket);
    }
    
    monitorSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (monitorSocket < 0) {
        return false;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    
    if (connect(monitorSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(monitorSocket);
        monitorSocket = -1;
        return false;
    }
    
    monitorPort = port;
    connected = true;
    
    return true;
}

void QemuConnection::Disconnect() {
    connected = false;
    shouldStop = true;
    
    if (receiveThread.joinable()) {
        receiveThread.join();
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
    if (!connected || monitorSocket < 0) {
        return false;
    }
    
    buffer.resize(size);
    size_t bytesRead = 0;
    
    while (bytesRead < size) {
        size_t chunkSize = std::min(size - bytesRead, size_t(4096));
        
        std::stringstream cmd;
        cmd << "xp/" << chunkSize << "xb 0x" << std::hex << (address + bytesRead);
        
        std::string response;
        if (!SendMonitorCommand(cmd.str(), response)) {
            return false;
        }
        
        std::istringstream iss(response);
        std::string line;
        while (std::getline(iss, line) && bytesRead < size) {
            if (line.find("0x") == 0) {
                size_t colonPos = line.find(':');
                if (colonPos != std::string::npos) {
                    std::string hexData = line.substr(colonPos + 1);
                    std::istringstream hexStream(hexData);
                    
                    int value;
                    while (hexStream >> std::hex >> value && bytesRead < size) {
                        buffer[bytesRead++] = static_cast<uint8_t>(value);
                    }
                }
            }
        }
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
    
    std::string cmdStr = command + "\n";
    if (send(monitorSocket, cmdStr.c_str(), cmdStr.length(), 0) < 0) {
        return false;
    }
    
    char buffer[4096];
    response.clear();
    
    usleep(50000);
    
    int received = recv(monitorSocket, buffer, sizeof(buffer)-1, MSG_DONTWAIT);
    if (received > 0) {
        buffer[received] = '\0';
        response = buffer;
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
    
    if (!connected) {
        if (ImGui::Button("Connect")) {
            bool qmpSuccess = ConnectQMP(inputHost, inputQMPPort);
            bool monSuccess = ConnectMonitor(inputHost, inputMonitorPort);
            
            if (!qmpSuccess && !monSuccess) {
                ImGui::OpenPopup("Connection Failed");
            }
        }
    } else {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Connected");
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