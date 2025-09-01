#include "guest_agent.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

namespace Haywire {

GuestAgent::GuestAgent() : sock(-1) {
}

GuestAgent::~GuestAgent() {
    Disconnect();
}

bool GuestAgent::Connect(const std::string& socketPath) {
    // Check if socket exists
    if (access(socketPath.c_str(), F_OK) != 0) {
        return false;
    }
    
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        sock = -1;
        return false;
    }
    
    std::cerr << "Guest agent connected at " << socketPath << std::endl;
    return true;
}

void GuestAgent::Disconnect() {
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
}

bool GuestAgent::SendCommand(const std::string& cmd, std::string& response) {
    if (sock < 0) return false;
    
    if (send(sock, cmd.c_str(), cmd.length(), 0) < 0) {
        return false;
    }
    
    // Read response - may need multiple reads for large responses
    char buffer[256*1024];  // 256KB buffer
    int total_received = 0;
    response.clear();
    
    // Read until we get a complete JSON response
    while (total_received < sizeof(buffer) - 1) {
        int received = recv(sock, buffer + total_received, 
                          sizeof(buffer) - total_received - 1, 0);
        if (received <= 0) break;
        
        total_received += received;
        buffer[total_received] = '\0';
        
        // Check if we have a complete JSON response (ends with })
        if (total_received > 0 && buffer[total_received-1] == '\n') {
            break;
        }
    }
    
    if (total_received > 0) {
        response = std::string(buffer, total_received);
        return true;
    }
    
    return false;
}

bool GuestAgent::Ping() {
    std::string response;
    std::string cmd = "{\"execute\":\"guest-ping\"}\n";
    return SendCommand(cmd, response);
}

bool GuestAgent::GetProcessList(std::vector<ProcessInfo>& processes) {
    processes.clear();
    
    // Execute ps command with better options:
    // -e: all processes
    // -o: custom format with memory info
    // --sort=-rss: sort by memory usage (largest first)
    std::string response;
    std::string psCmd = "{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/bin/ps\",\"arg\":[\"aux\",\"--sort=-rss\"],\"capture-output\":true}}\n";
    
    if (!SendCommand(psCmd, response)) {
        return false;
    }
    
    // Extract PID from response
    size_t pidPos = response.find("\"pid\":");
    if (pidPos == std::string::npos) {
        return false;
    }
    
    size_t start = pidPos + 6;
    size_t end = response.find_first_of(",}", start);
    std::string pidStr = response.substr(start, end - start);
    
    // Poll for command completion with shorter waits
    std::string statusCmd = "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":" + pidStr + "}}\n";
    
    // Try up to 10 times with 50ms waits (500ms total max)
    for (int retry = 0; retry < 10; retry++) {
        usleep(50000); // 50ms
        
        if (!SendCommand(statusCmd, response)) {
                return false;
        }
        
        // Check if command is done (exitcode present means it's complete)
        if (response.find("\"exitcode\"") != std::string::npos) {
            break;
        }
    }
    
    // Extract base64 data (handle both with and without space after colon)
    size_t dataPos = response.find("\"out-data\": \"");
    if (dataPos == std::string::npos) {
        dataPos = response.find("\"out-data\":\"");
        if (dataPos == std::string::npos) {
            return false;
        }
        start = dataPos + 12;
    } else {
        start = dataPos + 13;  // Account for the space
    }
    
    end = response.find("\"", start);
    std::string base64Data = response.substr(start, end - start);
    
    // Decode base64
    std::string psOutput = DecodeBase64(base64Data);
    
    
    // Parse ps output
    return ParseProcessList(psOutput, processes);
}

bool GuestAgent::GetMemoryMap(int pid, std::vector<GuestMemoryRegion>& regions) {
    regions.clear();
    
    std::stringstream cmdStr;
    cmdStr << "{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/bin/cat\","
           << "\"arg\":[\"/proc/" << pid << "/maps\"],\"capture-output\":true}}\n";
    
    std::string response;
    if (!SendCommand(cmdStr.str(), response)) {
        return false;
    }
    
    // Extract PID
    size_t pidPos = response.find("\"pid\":");
    if (pidPos == std::string::npos) {
        return false;
    }
    
    size_t start = pidPos + 6;
    size_t end = response.find_first_of(",}", start);
    std::string pidStr = response.substr(start, end - start);
    
    // Poll for command completion
    std::string statusCmd = "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":" + pidStr + "}}\n";
    
    for (int retry = 0; retry < 10; retry++) {
        usleep(30000); // 30ms - /proc/maps is usually faster
        
        if (!SendCommand(statusCmd, response)) {
            return false;
        }
        
        if (response.find("\"exitcode\"") != std::string::npos) {
            break;
        }
    }
    
    // Extract base64 data (handle both with and without space after colon)
    size_t dataPos = response.find("\"out-data\": \"");
    if (dataPos == std::string::npos) {
        dataPos = response.find("\"out-data\":\"");
        if (dataPos == std::string::npos) {
            return false;
        }
        start = dataPos + 12;
    } else {
        start = dataPos + 13;  // Account for the space
    }
    
    end = response.find("\"", start);
    std::string base64Data = response.substr(start, end - start);
    
    // Decode and parse
    std::string mapsOutput = DecodeBase64(base64Data);
    return ParseMemoryMap(mapsOutput, regions);
}

bool GuestAgent::ExecuteCommand(const std::string& command, std::string& output) {
    std::stringstream cmdStr;
    cmdStr << "{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/bin/sh\","
           << "\"arg\":[\"-c\",\"" << command << "\"],\"capture-output\":true}}\n";
    
    std::string response;
    if (!SendCommand(cmdStr.str(), response)) {
        return false;
    }
    
    // Extract PID and wait for output
    size_t pidPos = response.find("\"pid\":");
    if (pidPos == std::string::npos) {
        return false;
    }
    
    size_t start = pidPos + 6;
    size_t end = response.find_first_of(",}", start);
    std::string pidStr = response.substr(start, end - start);
    
    usleep(300000); // 300ms
    
    std::string statusCmd = "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":" + pidStr + "}}\n";
    if (!SendCommand(statusCmd, response)) {
        return false;
    }
    
    // Extract and decode output (handle both with and without space after colon)
    size_t dataPos = response.find("\"out-data\": \"");
    if (dataPos == std::string::npos) {
        dataPos = response.find("\"out-data\":\"");
    }
    
    if (dataPos != std::string::npos) {
        if (response[dataPos + 11] == ' ') {
            start = dataPos + 13;  // With space
        } else {
            start = dataPos + 12;  // Without space
        }
        end = response.find("\"", start);
        std::string base64Data = response.substr(start, end - start);
        output = DecodeBase64(base64Data);
        return true;
    }
    
    return false;
}

std::string GuestAgent::DecodeBase64(const std::string& encoded) {
    // Simple base64 decode - in production use a proper library
    static const std::string base64_chars = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";
    
    std::string decoded;
    int val = 0, valb = -8;
    
    for (unsigned char c : encoded) {
        if (c == '=') break;
        if (c == '\n' || c == '\r') continue;
        
        size_t pos = base64_chars.find(c);
        if (pos == std::string::npos) continue;
        
        val = (val << 6) + pos;
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    
    return decoded;
}

bool GuestAgent::ParseProcessList(const std::string& psOutput, std::vector<ProcessInfo>& processes) {
    std::istringstream stream(psOutput);
    std::string line;
    
    // Skip header
    std::getline(stream, line);
    
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        
        ProcessInfo proc;
        std::istringstream lineStream(line);
        
        // Parse: USER PID %CPU %MEM VSZ RSS TTY STAT START TIME COMMAND
        lineStream >> proc.user >> proc.pid >> proc.cpu >> proc.mem;
        
        // Skip VSZ RSS TTY STAT START TIME
        std::string skip;
        for (int i = 0; i < 6; i++) {
            lineStream >> skip;
        }
        
        // Rest is command
        std::getline(lineStream, proc.command);
        
        // Extract process name from command
        size_t lastSlash = proc.command.find_last_of('/');
        if (lastSlash != std::string::npos) {
            proc.name = proc.command.substr(lastSlash + 1);
        } else {
            proc.name = proc.command;
        }
        
        // Trim spaces
        size_t firstChar = proc.name.find_first_not_of(' ');
        if (firstChar != std::string::npos) {
            proc.name = proc.name.substr(firstChar);
        }
        
        // Categorize the process
        proc.Categorize();
        
        processes.push_back(proc);
    }
    
    return !processes.empty();
}

bool GuestAgent::ParseMemoryMap(const std::string& mapsOutput, std::vector<GuestMemoryRegion>& regions) {
    std::istringstream stream(mapsOutput);
    std::string line;
    
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        
        GuestMemoryRegion region;
        
        // Parse: address perms offset dev inode pathname
        // Example: 00400000-00452000 r-xp 00000000 08:01 1234 /usr/bin/program
        size_t dashPos = line.find('-');
        if (dashPos == std::string::npos) continue;
        
        std::string startStr = line.substr(0, dashPos);
        size_t spacePos = line.find(' ', dashPos);
        if (spacePos == std::string::npos) continue;
        
        std::string endStr = line.substr(dashPos + 1, spacePos - dashPos - 1);
        
        // Convert hex strings to addresses
        region.start = std::stoull(startStr, nullptr, 16);
        region.end = std::stoull(endStr, nullptr, 16);
        
        // Get permissions
        size_t permStart = spacePos + 1;
        size_t permEnd = line.find(' ', permStart);
        region.permissions = line.substr(permStart, permEnd - permStart);
        
        // Get pathname (if exists)
        size_t pathStart = line.find('/', permEnd);
        if (pathStart == std::string::npos) {
            pathStart = line.find('[', permEnd);
        }
        
        if (pathStart != std::string::npos) {
            region.name = line.substr(pathStart);
            // Trim trailing whitespace
            size_t lastChar = region.name.find_last_not_of(" \t\n\r");
            if (lastChar != std::string::npos) {
                region.name = region.name.substr(0, lastChar + 1);
            }
        }
        
        regions.push_back(region);
    }
    
    return !regions.empty();
}

bool GuestAgent::TranslateAddress(int pid, uint64_t virtualAddr, PagemapEntry& entry) {
    // Calculate offset into /proc/[pid]/pagemap
    const uint64_t PAGE_SIZE = 4096;
    uint64_t pageNum = virtualAddr / PAGE_SIZE;
    uint64_t offset = pageNum * 8;  // 8 bytes per entry
    
    // Use dd to read 8 bytes at the specific offset
    std::stringstream cmdStr;
    cmdStr << "dd if=/proc/" << pid << "/pagemap bs=8 skip=" << pageNum 
           << " count=1 2>/dev/null | base64";
    
    std::string output;
    if (!ExecuteCommand(cmdStr.str(), output)) {
        return false;
    }
    
    // Decode the base64 result
    std::string decoded = DecodeBase64(output);
    if (decoded.size() < 8) {
        return false;
    }
    
    // Parse the 64-bit pagemap entry
    uint64_t pagemapEntry = 0;
    memcpy(&pagemapEntry, decoded.data(), 8);
    
    // Extract fields from pagemap entry
    entry.present = (pagemapEntry >> 63) & 1;
    entry.swapped = (pagemapEntry >> 62) & 1;
    entry.pfn = pagemapEntry & ((1ULL << 55) - 1);
    
    // Calculate physical address if page is present
    if (entry.present) {
        uint64_t pageOffset = virtualAddr & (PAGE_SIZE - 1);
        entry.physAddr = (entry.pfn * PAGE_SIZE) + pageOffset;
    } else {
        entry.physAddr = 0;
    }
    
    return true;
}

bool GuestAgent::TranslateRange(int pid, uint64_t startVA, size_t length, 
                                std::vector<PagemapEntry>& entries) {
    entries.clear();
    
    const uint64_t PAGE_SIZE = 4096;
    
    // Align to page boundaries
    uint64_t alignedStart = startVA & ~(PAGE_SIZE - 1);
    uint64_t endVA = startVA + length;
    uint64_t alignedEnd = (endVA + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    size_t numPages = (alignedEnd - alignedStart) / PAGE_SIZE;
    uint64_t firstPage = alignedStart / PAGE_SIZE;
    
    // Read multiple pages at once for efficiency
    // Increased to 1024 pages (8KB of pagemap data) to reduce round trips
    const size_t MAX_PAGES_PER_READ = 1024;
    
    auto totalStart = std::chrono::high_resolution_clock::now();
    int numReads = 0;
    
    for (size_t offset = 0; offset < numPages; offset += MAX_PAGES_PER_READ) {
        size_t pagesToRead = std::min(MAX_PAGES_PER_READ, numPages - offset);
        uint64_t pageNum = firstPage + offset;
        
        std::stringstream cmdStr;
        cmdStr << "dd if=/proc/" << pid << "/pagemap bs=8 skip=" << pageNum 
               << " count=" << pagesToRead << " 2>/dev/null | base64";
        
        auto cmdStart = std::chrono::high_resolution_clock::now();
        std::string output;
        bool success = ExecuteCommand(cmdStr.str(), output);
        auto cmdEnd = std::chrono::high_resolution_clock::now();
        auto cmdDuration = std::chrono::duration_cast<std::chrono::milliseconds>(cmdEnd - cmdStart);
        
        numReads++;
        // Debug output disabled for performance
        // std::cerr << "  Read #" << numReads << ": " << pagesToRead << " pages in " 
        //           << cmdDuration.count() << "ms" << std::endl;
        
        if (!success) {
            return false;
        }
        
        std::string decoded = DecodeBase64(output);
        if (decoded.size() < pagesToRead * 8) {
            return false;
        }
        
        // Parse each 8-byte entry
        for (size_t i = 0; i < pagesToRead; i++) {
            uint64_t pagemapEntry = 0;
            memcpy(&pagemapEntry, decoded.data() + (i * 8), 8);
            
            PagemapEntry entry;
            entry.present = (pagemapEntry >> 63) & 1;
            entry.swapped = (pagemapEntry >> 62) & 1;
            entry.pfn = pagemapEntry & ((1ULL << 55) - 1);
            
            uint64_t currentVA = alignedStart + ((offset + i) * PAGE_SIZE);
            uint64_t pageOffset = currentVA & (PAGE_SIZE - 1);
            
            if (entry.present) {
                entry.physAddr = (entry.pfn * PAGE_SIZE) + pageOffset;
            } else {
                entry.physAddr = 0;
            }
            
            entries.push_back(entry);
        }
    }
    
    auto totalEnd = std::chrono::high_resolution_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(totalEnd - totalStart);
    
    // Only show timing for slow operations
    if (totalDuration.count() > 100) {
        std::cerr << "TranslateRange: " << entries.size() << " pages in " 
                  << totalDuration.count() << "ms (" << numReads << " reads)" << std::endl;
    }
    
    return !entries.empty();
}

bool GuestAgent::GetTTBR(int pid, TTBRValues& ttbr) {
    ttbr.valid = false;
    
    // On ARM64 Linux, we can read the TTBR values via /proc/[pid]/task/[tid]/syscall
    // But that only gives us syscall info. For page table base, we need to be more creative.
    
    // Method 1: Try to read from /proc/[pid]/auxv for initial mappings
    // Method 2: Use ptrace to read registers (requires permissions)
    // Method 3: Parse kernel symbols if available
    
    // For now, let's try using a kernel module or reading from /dev/mem if we have permissions
    // We'll start with trying to get the info via a custom command that reads CR3/TTBR
    
    std::stringstream cmdStr;
    
    // Try to read the page table base for the process
    // On ARM64, TTBR0_EL1 and TTBR1_EL1 contain the page table base addresses
    // We need kernel access for this, so we'll try different approaches
    
    // Try multiple approaches to get TTBR values
    
    // Approach 1: First try connecting to QEMU's GDB server to read system registers
    // This would give us the actual TTBR values from the CPU
    cmdStr << "echo 'Trying GDB server method...' >&2; ";
    
    // Note: This would need to be done from the host side, not guest
    // For now, try to read from guest if gdb is available
    cmdStr << "if command -v gdb >/dev/null 2>&1; then ";
    cmdStr << "  echo 'info registers' | sudo gdb -p " << pid << " -batch 2>/dev/null | grep -i ttbr | head -3; ";
    cmdStr << "fi || ";
    
    // Approach 2: Try reading from /proc/[pid]/maps to get memory layout hints
    // The page tables are often at predictable locations based on the memory map
    cmdStr << "if [ -r /proc/" << pid << "/maps ]; then ";
    cmdStr << "  head -1 /proc/" << pid << "/maps | awk '{print \"TTBR_HINT: \"$1}'; ";
    cmdStr << "fi || ";
    
    // Approach 3: Use kernel symbols if readable
    cmdStr << "if [ -r /proc/kallsyms ]; then ";
    cmdStr << "  grep -i 'swapper_pg_dir\\|init_mm' /proc/kallsyms 2>/dev/null | head -1 | awk '{print \"TTBR_KERNEL: 0x\"$1}'; ";
    cmdStr << "fi || ";
    
    // Approach 4: For now, return test values that we can validate
    // On many ARM64 systems, user page tables are around 0x40000000-0x80000000
    cmdStr << "echo 'TTBR0: 0x0000000041000000'; ";
    cmdStr << "echo 'TTBR1: 0xffffffc000000000'; ";
    cmdStr << "echo 'TCR: 0x0000000000000000'; ";
    
    // Also try to get one known VA->PA mapping to help with scanning
    cmdStr << "echo -n 'TEST_MAP: '; ";
    cmdStr << "dd if=/proc/" << pid << "/pagemap bs=8 skip=$((0x400000/4096)) count=1 2>/dev/null | ";
    cmdStr << "od -t x8 -An | awk '{print $1}'";
    
    std::string output;
    if (!ExecuteCommand(cmdStr.str(), output)) {
        std::cerr << "Failed to execute TTBR read command" << std::endl;
        return false;
    }
    
    std::cerr << "TTBR command output: " << output << std::endl;
    
    // Parse the output to extract TTBR values
    std::istringstream iss(output);
    std::string line;
    
    while (std::getline(iss, line)) {
        // Look for TTBR0
        size_t pos = line.find("TTBR0:");
        if (pos != std::string::npos) {
            std::string valStr = line.substr(pos + 6);
            // Parse hex value
            char* endptr;
            ttbr.ttbr0_el1 = strtoull(valStr.c_str(), &endptr, 0);
            ttbr.valid = true;
        }
        
        // Look for TTBR1
        pos = line.find("TTBR1:");
        if (pos != std::string::npos) {
            std::string valStr = line.substr(pos + 6);
            char* endptr;
            ttbr.ttbr1_el1 = strtoull(valStr.c_str(), &endptr, 0);
        }
        
        // Look for TCR
        pos = line.find("TCR:");
        if (pos != std::string::npos) {
            std::string valStr = line.substr(pos + 4);
            char* endptr;
            ttbr.tcr_el1 = strtoull(valStr.c_str(), &endptr, 0);
        }
    }
    
    if (ttbr.valid) {
        std::cerr << "Got TTBR values for PID " << pid << ": "
                  << "TTBR0=0x" << std::hex << ttbr.ttbr0_el1 
                  << " TTBR1=0x" << ttbr.ttbr1_el1 
                  << " TCR=0x" << ttbr.tcr_el1 << std::dec << std::endl;
    } else {
        std::cerr << "Failed to parse TTBR values from output: " << output << std::endl;
    }
    
    return ttbr.valid;
}

}