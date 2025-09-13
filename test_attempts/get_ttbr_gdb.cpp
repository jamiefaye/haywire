#include <iostream>
#include <iomanip>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <sstream>

// Simple GDB remote protocol client to read ARM64 system registers
class GDBClient {
public:
    bool Connect(const std::string& host, int port) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            sock = -1;
            return false;
        }
        
        return true;
    }
    
    void Disconnect() {
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }
    }
    
    // Send GDB packet with checksum
    bool SendPacket(const std::string& data) {
        std::stringstream packet;
        packet << "$" << data << "#";
        
        // Calculate checksum
        uint8_t checksum = 0;
        for (char c : data) {
            checksum += c;
        }
        
        packet << std::hex << std::setfill('0') << std::setw(2) << (int)checksum;
        
        std::string pkt = packet.str();
        if (send(sock, pkt.c_str(), pkt.length(), 0) != pkt.length()) {
            return false;
        }
        
        // Wait for ACK
        char ack;
        recv(sock, &ack, 1, 0);
        return ack == '+';
    }
    
    // Receive GDB packet
    std::string ReceivePacket() {
        std::string response;
        char buffer[4096];
        
        int n = recv(sock, buffer, sizeof(buffer)-1, 0);
        if (n > 0) {
            buffer[n] = 0;
            response = buffer;
            
            // Send ACK
            send(sock, "+", 1, 0);
            
            // Extract data between $ and #
            size_t start = response.find('$');
            size_t end = response.find('#');
            if (start != std::string::npos && end != std::string::npos) {
                return response.substr(start+1, end-start-1);
            }
        }
        
        return response;
    }
    
    // Read ARM64 system register
    uint64_t ReadSystemRegister(const std::string& regname) {
        // For ARM64, we need to read the system registers
        // TTBR0_EL1: op0=3, op1=0, CRn=2, CRm=0, op2=0 -> encoding = 0xC002
        // TTBR1_EL1: op0=3, op1=0, CRn=2, CRm=0, op2=1 -> encoding = 0xC003
        
        // GDB doesn't have direct system register access, but we can try:
        // 1. Read all registers with 'g' command
        // 2. Use monitor commands if QEMU supports them
        
        // Try monitor command first (QEMU specific)
        std::string cmd = "qRcmd,";
        std::string monitor_cmd = "info registers";
        for (char c : monitor_cmd) {
            char hex[3];
            sprintf(hex, "%02x", c);
            cmd += hex;
        }
        
        if (SendPacket(cmd)) {
            std::string resp = ReceivePacket();
            std::cout << "Monitor response: " << resp << std::endl;
        }
        
        return 0;
    }
    
    // Read general registers
    std::vector<uint64_t> ReadGeneralRegisters() {
        std::vector<uint64_t> regs;
        
        if (SendPacket("g")) {
            std::string resp = ReceivePacket();
            
            // Parse hex response (each register is 16 hex chars for 64-bit)
            for (size_t i = 0; i + 15 < resp.length(); i += 16) {
                std::string hex = resp.substr(i, 16);
                
                // Convert from little-endian hex string
                uint64_t value = 0;
                for (int j = 7; j >= 0; j--) {
                    std::string byte = hex.substr(j*2, 2);
                    value = (value << 8) | std::stoul(byte, nullptr, 16);
                }
                
                regs.push_back(value);
            }
        }
        
        return regs;
    }
    
private:
    int sock = -1;
};

int main() {
    std::cout << "Connecting to QEMU GDB server on port 1234..." << std::endl;
    
    GDBClient gdb;
    if (!gdb.Connect("localhost", 1234)) {
        std::cerr << "Failed to connect to GDB server" << std::endl;
        std::cerr << "Make sure QEMU was started with -gdb tcp::1234" << std::endl;
        return 1;
    }
    
    std::cout << "Connected!" << std::endl;
    
    // Try to get registers
    std::cout << "\nReading general registers..." << std::endl;
    auto regs = gdb.ReadGeneralRegisters();
    
    std::cout << "Got " << regs.size() << " registers:" << std::endl;
    for (size_t i = 0; i < std::min(size_t(10), regs.size()); i++) {
        std::cout << "  R" << i << " = 0x" << std::hex << regs[i] << std::dec << std::endl;
    }
    
    // Try to read TTBR (this is tricky via GDB)
    std::cout << "\nTrying to read system registers..." << std::endl;
    gdb.ReadSystemRegister("TTBR0_EL1");
    
    gdb.Disconnect();
    return 0;
}