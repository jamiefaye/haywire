#include <iostream>
#include <iomanip>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

int main() {
    // Connect to QMP
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(4445);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to QMP\n";
        return 1;
    }

    char buffer[8192];
    // Read greeting
    recv(sock, buffer, sizeof(buffer), 0);

    // Send capabilities
    const char* caps = "{\"execute\": \"qmp_capabilities\"}\n";
    send(sock, caps, strlen(caps), 0);
    recv(sock, buffer, sizeof(buffer), 0);

    std::cout << "Querying current_task for all CPUs:\n\n";

    // Query each CPU (0-3)
    for (int cpu = 0; cpu < 4; cpu++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                "{\"execute\": \"query-kernel-info\", \"arguments\": {\"cpu-index\": %d}}\n", cpu);

        send(sock, cmd, strlen(cmd), 0);
        int len = recv(sock, buffer, sizeof(buffer)-1, 0);
        buffer[len] = '\0';

        std::cout << "CPU " << cpu << " response:\n";

        // Quick parse for current-task
        std::string response(buffer);
        size_t pos = response.find("\"current-task\":");
        if (pos != std::string::npos) {
            size_t start = pos + 15;
            while (response[start] == ' ') start++;
            size_t end = start;
            while (response[end] != ',' && response[end] != '}') end++;
            std::string task = response.substr(start, end-start);

            uint64_t task_addr = std::stoull(task);
            std::cout << "  current-task: 0x" << std::hex << task_addr << std::dec;

            // Check if it looks valid
            if ((task_addr & 0xFFFF000000000000ULL) == 0xFFFF000000000000ULL) {
                std::cout << " (valid kernel VA)";
            } else if (task_addr == 0) {
                std::cout << " (NULL - CPU idle?)";
            } else {
                std::cout << " (suspicious!)";
            }
            std::cout << "\n";
        }

        // Also check TTBR1 to see if it's consistent
        pos = response.find("\"ttbr1\":");
        if (pos != std::string::npos) {
            size_t start = pos + 8;
            while (response[start] == ' ') start++;
            size_t end = start;
            while (response[end] != ',' && response[end] != '}') end++;
            std::string ttbr = response.substr(start, end-start);

            uint64_t ttbr_val = std::stoull(ttbr);
            std::cout << "  ttbr1: 0x" << std::hex << ttbr_val << std::dec << "\n";
        }
        std::cout << "\n";
    }

    close(sock);
    return 0;
}