#include "qemu_connection.h"
#include "imgui.h"
#include <iostream>
#include <iomanip>
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
      inputQMPPort(4445), inputMonitorPort(4444), inputGDBPort(1234), 
      useGDB(false), useMMap(false), useMemoryBackend(false) {
    strcpy(inputHost, "localhost");
    gdbConnection = std::make_unique<GDBConnection>();
    mmapReader = std::make_unique<MMapReader>();
    memoryBackend = std::make_unique<MemoryBackend>();
    guestAgent = std::make_shared<GuestAgent>();
    
    // Try to auto-detect memory backend on startup
    if (memoryBackend->AutoDetect()) {
        useMemoryBackend = true;
        std::cerr << "Memory backend auto-detected and enabled!\n";
        
        // Initialize memory mapping discovery
        std::cerr << "Discovering memory regions from QEMU monitor...\n";
        memoryBackend->InitializeMemoryMapping("localhost", 4444);
    }
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

bool QemuConnection::AutoConnect() {
    // Auto-connecting to QEMU...
    
    // Step 1: Try to detect memory backend first (fastest)
    // NOTE: On macOS, using MAP_SHARED with aggressive cache invalidation
    if (memoryBackend && memoryBackend->AutoDetect()) {
        useMemoryBackend = true;
        // Memory backend detected, try QMP for control
        if (ConnectQMP("localhost", 4445)) {
            connected = true;
            // Also try to connect guest agent
            if (guestAgent && guestAgent->Connect()) {
                std::cerr << "✓ Guest agent connected\n";
            }
            return true;
        }
        
        // Memory backend works even without QMP for read-only access
        connected = true;
        // Try guest agent even without QMP
        if (guestAgent && guestAgent->Connect()) {
            std::cerr << "✓ Guest agent connected\n";
        }
        return true;
    }
    
    // Step 2: Try QMP connection
    if (ConnectQMP("localhost", 4445)) {
        // Try to enable mmap mode for better performance
        if (ConnectMonitor("localhost", 4444)) {
            useMMap = true;
        }
        
        connected = true;
        // Try guest agent
        if (guestAgent && guestAgent->Connect()) {
            std::cerr << "✓ Guest agent connected\n";
        }
        return true;
    }
    
    // Step 3: Try monitor-only connection
    if (ConnectMonitor("localhost", 4444)) {
        std::cerr << "✓ Connected via Monitor (slower)\n";
        connected = true;
        return true;
    }
    
    // Step 4: No connection available
    std::cerr << "⚠ No QEMU connection available\n";
    std::cerr << "  Please start QEMU with scripts/launch_qemu_membackend.sh\n";
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
    useMMap = false;
    
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

bool QemuConnection::TestPageNonZero(uint64_t address, size_t size) {
    if (!connected) {
        return false;
    }

    // Use direct memory backend if available (fastest - zero copy!)
    if (useMemoryBackend && memoryBackend && memoryBackend->IsAvailable()) {
        const uint8_t* ptr = memoryBackend->GetDirectPointer(address);
        if (!ptr) {
            return false;  // Address not in mapped region
        }

        // Direct scan - no allocation, no copying!
        // Accumulate entire page with OR, check only at the end
        const uint64_t* ptr64 = reinterpret_cast<const uint64_t*>(ptr);
        size_t numQuads = size / 8;

        // For pages (4096 bytes = 512 quadwords), accumulate everything
        uint64_t accumulator = 0;

        // Unroll by 8 for better pipelining (512 = 64 * 8)
        for (size_t i = 0; i < numQuads; i += 8) {
            accumulator |= ptr64[i];
            accumulator |= ptr64[i+1];
            accumulator |= ptr64[i+2];
            accumulator |= ptr64[i+3];
            accumulator |= ptr64[i+4];
            accumulator |= ptr64[i+5];
            accumulator |= ptr64[i+6];
            accumulator |= ptr64[i+7];
        }

        // Single check at the end
        return (accumulator != 0);
    }

    // Fallback: read and test (slower - requires copy)
    std::vector<uint8_t> buffer;
    if (!ReadMemory(address, size, buffer)) {
        return false;
    }

    for (const uint8_t& byte : buffer) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

bool QemuConnection::ReadMemoryMMap(uint64_t address, size_t size, std::vector<uint8_t>& buffer) {
    if (!connected || qmpSocket < 0) {
        return false;
    }
    
    // Dump and map the memory
    if (mmapReader->DumpAndMap(*this, address, size)) {
        // Read from mapped memory
        return mmapReader->Read(0, size, buffer);
    }
    
    return false;
}

bool QemuConnection::ReadMemory(uint64_t address, size_t size, std::vector<uint8_t>& buffer) {
    if (!connected) {
        return false;
    }
    
    // Use direct memory backend if available (fastest!)
    if (useMemoryBackend && memoryBackend && memoryBackend->IsAvailable()) {
        bool result = memoryBackend->Read(address, size, buffer);
        if (result) {
            UpdateReadSpeed(size);
            return true;
        }
    }
    
    // Use mmap if enabled
    if (useMMap) {
        return ReadMemoryMMap(address, size, buffer);
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
        size_t chunkSize = std::min(size - bytesRead, size_t(1024));  // Medium chunks for balance
        
        std::stringstream cmd;
        cmd << "xp/" << chunkSize << "xb 0x" << std::hex << (address + bytesRead);
        // Only log first command
        if (bytesRead == 0) {
            std::cerr << "Monitor: Reading from 0x" << std::hex << address << std::dec << "\n";
        }
        
        std::string response;
        if (!SendMonitorCommand(cmd.str(), response)) {
            // On timeout or error, return what we have
            std::cerr << "Monitor read failed at offset " << bytesRead << " of " << size << "\n";
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
        if (pos == 0) {
            std::cerr << "No progress parsing response, stopping at " << bytesRead << " bytes\n";
            break;
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
        std::cerr << "Monitor: Socket not connected\n";
        return false;
    }
    
    std::lock_guard<std::mutex> lock(monitorMutex);
    
    // Send command
    std::string cmdStr = command + "\n";
    if (send(monitorSocket, cmdStr.c_str(), cmdStr.length(), 0) < 0) {
        std::cerr << "Monitor: Failed to send command: " << command << "\n";
        return false;
    }
    
    // Read response (with multiple attempts for slow commands)
    char buffer[16384];  // Larger buffer
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
        // Only log if unusual size
        if (totalReceived >= 8190) {
            std::cerr << "Monitor: Hit buffer limit (" << totalReceived << " bytes)\n";
        }
        
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
    
    std::cerr << "Monitor: No response received (timeout)\n";
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
    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_FirstUseEver);
    ImGui::Text("QEMU Connection Settings");
    ImGui::Separator();
    
    ImGui::InputText("Host", inputHost, sizeof(inputHost));
    ImGui::InputInt("QMP Port", &inputQMPPort);
    ImGui::InputInt("Monitor Port", &inputMonitorPort);
    ImGui::InputInt("GDB Port", &inputGDBPort);
    
    if (!connected) {
        // Show launch helper prominently if no VM detected
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0, 1));
        ImGui::Text("⚠ No QEMU VM detected");
        ImGui::PopStyleColor();
        
        ImGui::TextWrapped("Start QEMU with memory backend for best performance:");
        
        if (ImGui::Button("Show Launch Command", ImVec2(-1, 30))) {
            ImGui::OpenPopup("Launch QEMU");
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Check if memory backend is available
        if (memoryBackend && !memoryBackend->IsAvailable()) {
            // Try to auto-detect again
            if (memoryBackend->AutoDetect()) {
                useMemoryBackend = true;
            }
        }
        
        // Show memory backend status
        if (useMemoryBackend && memoryBackend && memoryBackend->IsAvailable()) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "✓ Memory backend detected!");
            ImGui::TextColored(ImVec4(0.7, 0.7, 1, 1), "Path: %s", 
                memoryBackend->GetBackendPath().c_str());
            if (ImGui::Button("Connect with Zero-Copy Access", ImVec2(250, 0))) {
                connected = ConnectQMP(inputHost, inputQMPPort);
                if (!connected) {
                    ImGui::OpenPopup("Connection Failed");
                }
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        } else {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "⚠ No memory backend detected");
            ImGui::TextWrapped("For fastest performance, restart QEMU with memory-backend-file");
            if (ImGui::Button("Show Launch Command", ImVec2(180, 0))) {
                ImGui::OpenPopup("Memory Backend Setup");
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh", ImVec2(80, 0))) {
                if (memoryBackend->AutoDetect()) {
                    useMemoryBackend = true;
                }
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
        
        ImGui::Text("Connection Options:");
        
        if (ImGui::Button("Connect via Monitor (Slower)", ImVec2(200, 0))) {
            bool qmpSuccess = ConnectQMP(inputHost, inputQMPPort);
            bool monSuccess = ConnectMonitor(inputHost, inputMonitorPort);
            
            if (!qmpSuccess && !monSuccess) {
                ImGui::OpenPopup("Connection Failed");
            }
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Connect via GDB (Experimental)", ImVec2(200, 0))) {
            ImGui::OpenPopup("GDB Not Implemented");
        }
        
        ImGui::Spacing();
        
        if (ImGui::Button("Connect via MMap (Fastest)", ImVec2(200, 0))) {
            bool qmpSuccess = ConnectQMP(inputHost, inputQMPPort);
            
            if (qmpSuccess) {
                useMMap = true;
                connected = true;
                ImGui::OpenPopup("MMap Info");
            } else {
                ImGui::OpenPopup("Connection Failed");
            }
        }
        
        ImGui::SameLine();
        
        if (ImGui::Button("Test Memory Dump", ImVec2(200, 0))) {
            // Test dumping 1MB of memory from address 0
            nlohmann::json cmd = {
                {"execute", "pmemsave"},
                {"arguments", {
                    {"val", 0},
                    {"size", 1048576},
                    {"filename", "/tmp/haywire_test.dump"}
                }}
            };
            
            bool qmpSuccess = ConnectQMP(inputHost, inputQMPPort);
            if (qmpSuccess) {
                nlohmann::json response;
                if (SendQMPCommand(cmd, response)) {
                    if (!response.contains("error")) {
                        ImGui::OpenPopup("Dump Success");
                    }
                }
            }
        }
        
        ImGui::Spacing();
        ImGui::TextDisabled("Monitor protocol recommended for live memory viewing");
    } else {
        ImGui::Spacing();
        const char* protocol = useMemoryBackend ? "Memory Backend (Zero-Copy)" : 
                               (useMMap ? "MMap (Dump & Map)" : 
                               (useGDB ? "GDB Protocol" : "Monitor Protocol"));
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "✓ Connected via %s", protocol);
        
        if (useMemoryBackend) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "🚀 Direct memory backend - FASTEST!");
            ImGui::TextColored(ImVec4(0, 0.8, 1, 1), "Zero-copy access to guest memory");
            if (memoryBackend && memoryBackend->IsAvailable()) {
                ImGui::TextColored(ImVec4(0.7, 0.7, 1, 1), "Backend: %s", 
                    memoryBackend->GetBackendPath().c_str());
                ImGui::TextColored(ImVec4(0.7, 0.7, 1, 1), "Size: %zu MB", 
                    memoryBackend->GetMappedSize() / (1024*1024));
            }
        } else if (useMMap) {
            ImGui::TextColored(ImVec4(0, 1, 1, 1), "Using memory dump + mmap (fast)");
            ImGui::TextColored(ImVec4(0.7, 0.7, 1, 1), "Each read dumps then maps memory");
        } else if (useGDB) {
            ImGui::TextColored(ImVec4(0.5, 1, 1, 1), "Memory reads using fast binary protocol");
        } else {
            ImGui::TextColored(ImVec4(1, 1, 0.5, 1), "Memory reads using text-based monitor");
        }
        ImGui::Spacing();
        
        // Guest agent UI
        if (guestAgent && guestAgent->IsConnected()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "✓ Guest Agent Connected");
            
            if (ImGui::Button("List Processes", ImVec2(120, 0))) {
                std::vector<GuestProcessInfo> processes;
                if (guestAgent->GetProcessList(processes)) {
                    std::cout << "\n=== Guest Processes (sorted by memory) ===\n";
                    
                    // Show user apps first
                    std::cout << "\n--- User Applications ---\n";
                    int userAppCount = 0;
                    for (const auto& proc : processes) {
                        if (proc.category == GuestProcessInfo::USER_APP) {
                            std::cout << "PID " << std::setw(5) << proc.pid 
                                     << " | MEM: " << std::setw(5) << std::fixed 
                                     << std::setprecision(1) << proc.mem << "%"
                                     << " | " << proc.name << "\n";
                            userAppCount++;
                        }
                    }
                    
                    // Show high-memory services
                    std::cout << "\n--- Services (>0.5% memory) ---\n";
                    for (const auto& proc : processes) {
                        if (proc.category == GuestProcessInfo::SERVICE && proc.mem > 0.5) {
                            std::cout << "PID " << std::setw(5) << proc.pid 
                                     << " | MEM: " << std::setw(5) << std::fixed 
                                     << std::setprecision(1) << proc.mem << "%"
                                     << " | " << proc.name << "\n";
                        }
                    }
                    
                    std::cout << "\nTotal: " << processes.size() << " processes"
                             << " (" << userAppCount << " user apps)\n";
                }
            }
            
            ImGui::SameLine();
            static int pidForMaps = 1;
            ImGui::InputInt("PID", &pidForMaps);
            
            if (ImGui::Button("Get Memory Map", ImVec2(120, 0))) {
                std::vector<GuestMemoryRegion> regions;
                if (guestAgent->GetMemoryMap(pidForMaps, regions)) {
                    std::cout << "\n=== Memory Map for PID " << pidForMaps << " ===\n";
                    for (const auto& region : regions) {
                        std::cout << std::hex << region.start << "-" << region.end 
                                 << " " << region.permissions << " " << region.name << "\n";
                    }
                    std::cout << std::dec;
                }
            }
        } else if (guestAgent) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Guest Agent Not Connected");
            if (ImGui::Button("Connect Guest Agent", ImVec2(150, 0))) {
                if (guestAgent->Connect()) {
                    std::cerr << "✓ Guest agent connected\n";
                } else {
                    std::cerr << "Failed to connect guest agent\n";
                }
            }
        }
        
        ImGui::Spacing();
        if (ImGui::Button("Disconnect", ImVec2(150, 0))) {
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
    
    if (ImGui::BeginPopupModal("MMap Info", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Memory Dump + MMap Mode Active");
        ImGui::Separator();
        ImGui::Text("This mode uses QMP pmemsave to dump memory");
        ImGui::Text("then mmaps the file for instant access.");
        ImGui::Text("");
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "✓ Much faster than network protocols");
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "✓ VM continues running");
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "⚠ Shows snapshot, not live data");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    if (ImGui::BeginPopupModal("Dump Success", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Memory dumped to /tmp/haywire_test.dump");
        ImGui::Text("1MB dumped from address 0x0");
        ImGui::Text("Check with: hexdump -C /tmp/haywire_test.dump | head");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    if (ImGui::BeginPopupModal("GDB Not Implemented", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("GDB protocol pauses the VM and is not suitable");
        ImGui::Text("for live memory viewing. Use Monitor protocol.");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    if (ImGui::BeginPopupModal("Launch QEMU", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Run this command to start QEMU with memory backend:");
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5, 1, 0.5, 1));
        ImGui::Text("cd /Users/jamie/haywire");
        ImGui::Text("./scripts/launch_qemu_membackend.sh");
        ImGui::PopStyleColor();
        
        ImGui::Spacing();
        ImGui::Text("Or manually add these to your QEMU command:");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7, 0.7, 1, 1));
        ImGui::TextWrapped("-m 4G -object memory-backend-file,id=mem,size=4G,mem-path=/tmp/haywire-vm-mem,share=on -numa node,memdev=mem");
        ImGui::PopStyleColor();
        
        ImGui::Spacing();
        if (ImGui::Button("Retry Connection", ImVec2(150, 0))) {
            if (AutoConnect()) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    if (ImGui::BeginPopupModal("Memory Backend Setup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("To enable zero-copy memory access, restart QEMU with:");
        ImGui::Separator();
        
        ImGui::Text("Add these parameters to your QEMU command:");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5, 1, 0.5, 1));
        ImGui::TextWrapped("-m 4G");
        ImGui::TextWrapped("-object memory-backend-file,id=mem,size=4G,mem-path=/tmp/haywire-vm-mem,share=on");
        ImGui::TextWrapped("-numa node,memdev=mem");
        ImGui::PopStyleColor();
        
        ImGui::Spacing();
        ImGui::Text("Or use the provided launch script:");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5, 0.5, 1, 1));
        ImGui::Text("scripts/launch_qemu_membackend.sh");
        ImGui::PopStyleColor();
        
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Benefits:");
        ImGui::BulletText("1000x faster than network protocols");
        ImGui::BulletText("Zero-copy direct memory access");
        ImGui::BulletText("No disk I/O during reads");
        
        if (ImGui::Button("OK", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    if (ImGui::BeginPopupModal("GDB Warning", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "⚠ Warning: GDB pauses the VM");
        ImGui::Text("");
        ImGui::Text("The GDB protocol halts the VM when connected.");
        ImGui::Text("This stops video playback and all VM activity.");
        ImGui::Text("");
        ImGui::Text("For live memory viewing of running processes,");
        ImGui::Text("use Monitor protocol instead.");
        
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

bool QemuConnection::TranslateVA2PA(int cpuIndex, uint64_t virtualAddr, uint64_t& physicalAddr) {
    if (!connected || qmpSocket < 0) {
        return false;
    }
    
    nlohmann::json cmd = {
        {"execute", "query-va2pa"},
        {"arguments", {
            {"cpu-index", cpuIndex},
            {"addr", virtualAddr}
        }}
    };
    
    nlohmann::json response;
    if (!SendQMPCommand(cmd, response)) {
        return false;
    }
    
    if (response.contains("return")) {
        auto ret = response["return"];
        if (ret.contains("valid") && ret["valid"].get<bool>()) {
            physicalAddr = ret["phys"].get<uint64_t>();
            return true;
        }
    }
    
    return false;
}

void QemuConnection::QMPReceiveThread() {
}

}