/*
 * memory_mapper.cpp - QEMU memory region discovery and mapping
 */

#include "memory_mapper.h"
#include <iostream>
#include <sstream>
#include <regex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

namespace Haywire {

MemoryMapper::MemoryMapper() : discovered_(false) {
}

MemoryMapper::~MemoryMapper() {
}

std::string MemoryMapper::QueryMonitor(const std::string& host, int port, const std::string& command) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "MemoryMapper: Failed to create socket" << std::endl;
        return "";
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(host.c_str());

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "MemoryMapper: Failed to connect to QEMU monitor at " 
                  << host << ":" << port << std::endl;
        close(sock);
        return "";
    }

    // Read and discard the banner
    char buffer[4096];
    recv(sock, buffer, sizeof(buffer), 0);

    // Send command
    std::string cmd = command + "\n";
    send(sock, cmd.c_str(), cmd.length(), 0);

    // Read response
    std::string response;
    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        response += buffer;
        
        // Check if we got the prompt back (end of response)
        if (response.find("(qemu)") != std::string::npos) {
            break;
        }
    }

    close(sock);
    return response;
}

bool MemoryMapper::ParseMtreeOutput(const std::string& output) {
    regions_.clear();
    
    std::cout << "MemoryMapper: Parsing mtree output (" << output.length() << " bytes)" << std::endl;
    
    // Look for FlatView sections which show the actual memory layout
    // Format: address-range: name
    // Example: 0000000040000000-00000000bfffffff: mem (prio 0, ram)
    
    std::istringstream stream(output);
    std::string line;
    bool in_flatview = false;
    uint64_t file_offset = 0;  // Track cumulative offset in file
    
    while (std::getline(stream, line)) {
        // Look for FlatView section
        if (line.find("FlatView") != std::string::npos) {
            in_flatview = true;
            std::cout << "MemoryMapper: Found FlatView section" << std::endl;
            continue;
        }
        
        if (in_flatview) {
            // Look for RAM regions (containing "ram" or "mem")
            // Pattern: spaces, hex-hex: name
            std::regex ram_regex(R"(\s*([0-9a-f]+)-([0-9a-f]+):\s*([^\s]+).*\b(ram|mem)\b)");
            std::smatch match;
            
            if (std::regex_search(line, match, ram_regex)) {
                uint64_t start = std::stoull(match[1].str(), nullptr, 16);
                uint64_t end = std::stoull(match[2].str(), nullptr, 16);
                std::string name = match[3].str();
                
                MemoryRegion region;
                region.gpa_start = start;
                region.gpa_end = end;
                region.size = end - start + 1;
                region.name = name;
                region.file_offset = file_offset;
                
                regions_.push_back(region);
                
                std::cout << "MemoryMapper: Found RAM region '" << name << "': "
                          << "GPA 0x" << std::hex << start << "-0x" << end 
                          << " (size: 0x" << region.size << ")"
                          << " -> File offset: 0x" << file_offset << std::dec << std::endl;
                
                // Update file offset for next region
                file_offset += region.size;
            }
            
            // Stop at next section or empty line
            if (line.empty() || (line[0] != ' ' && line.find("FlatView") == std::string::npos)) {
                in_flatview = false;
            }
        }
    }
    
    // If we didn't find FlatView, try the simpler format
    if (regions_.empty()) {
        std::cout << "MemoryMapper: No FlatView found, trying simple format" << std::endl;
        
        stream.clear();
        stream.seekg(0);
        file_offset = 0;
        
        while (std::getline(stream, line)) {
            // Look for lines with "ram" or system RAM
            // Pattern: hex-hex : name (containing ram)
            if (line.find("ram") != std::string::npos || line.find("RAM") != std::string::npos) {
                std::regex simple_regex(R"(([0-9a-f]+)-([0-9a-f]+)\s*:\s*([^\s]+))");
                std::smatch match;
                
                if (std::regex_search(line, match, simple_regex)) {
                    uint64_t start = std::stoull(match[1].str(), nullptr, 16);
                    uint64_t end = std::stoull(match[2].str(), nullptr, 16);
                    std::string name = match[3].str();
                    
                    // Skip non-RAM regions
                    if (name.find("rom") != std::string::npos || 
                        name.find("io") != std::string::npos) {
                        continue;
                    }
                    
                    MemoryRegion region;
                    region.gpa_start = start;
                    region.gpa_end = end;
                    region.size = end - start + 1;
                    region.name = name;
                    region.file_offset = file_offset;
                    
                    regions_.push_back(region);
                    
                    std::cout << "MemoryMapper: Found RAM region '" << name << "': "
                              << "GPA 0x" << std::hex << start << "-0x" << end 
                              << " (size: 0x" << region.size << ")"
                              << " -> File offset: 0x" << file_offset << std::dec << std::endl;
                    
                    file_offset += region.size;
                }
            }
        }
    }
    
    // If still no regions found, fall back to hardcoded ARM64 default
    if (regions_.empty()) {
        std::cout << "MemoryMapper: WARNING: No RAM regions found in mtree output" << std::endl;
        std::cout << "MemoryMapper: Falling back to ARM64 default (0x40000000)" << std::endl;
        
        // Assume 4GB RAM at 0x40000000 (ARM64 default)
        MemoryRegion region;
        region.gpa_start = 0x40000000;
        region.gpa_end = 0x13FFFFFFF;  // 4GB
        region.size = 0x100000000;
        region.name = "default-ram";
        region.file_offset = 0;
        
        regions_.push_back(region);
    }
    
    std::cout << "MemoryMapper: Total regions found: " << regions_.size() << std::endl;
    return !regions_.empty();
}

bool MemoryMapper::DiscoverMemoryMap(const std::string& monitor_host, int monitor_port) {
    std::cout << "MemoryMapper: Discovering memory map from QEMU monitor at " 
              << monitor_host << ":" << monitor_port << std::endl;
    
    // Query memory tree from QEMU monitor
    std::string output = QueryMonitor(monitor_host, monitor_port, "info mtree -f");
    
    if (output.empty()) {
        std::cerr << "MemoryMapper: Failed to query QEMU monitor" << std::endl;
        
        // Fall back to default ARM64 mapping
        std::cout << "MemoryMapper: Using default ARM64 memory map" << std::endl;
        MemoryRegion region;
        region.gpa_start = 0x40000000;
        region.gpa_end = 0x13FFFFFFF;  // 4GB
        region.size = 0x100000000;
        region.name = "default-ram";
        region.file_offset = 0;
        regions_.push_back(region);
        discovered_ = true;
        return true;
    }
    
    // Log first 2000 chars of output for debugging
    std::cout << "MemoryMapper: Raw mtree output (first 2000 chars):" << std::endl;
    std::cout << output.substr(0, 2000) << std::endl;
    
    if (ParseMtreeOutput(output)) {
        discovered_ = true;
        LogRegions();
        return true;
    }
    
    return false;
}

int64_t MemoryMapper::TranslateGPAToFileOffset(uint64_t gpa) const {
    for (const auto& region : regions_) {
        if (gpa >= region.gpa_start && gpa <= region.gpa_end) {
            uint64_t offset_in_region = gpa - region.gpa_start;
            return region.file_offset + offset_in_region;
        }
    }
    
    // Not found in any region
    return -1;
}

void MemoryMapper::LogRegions() const {
    std::cout << "MemoryMapper: Memory regions mapping:" << std::endl;
    std::cout << "================================================" << std::endl;
    
    for (const auto& region : regions_) {
        std::cout << "Region: " << region.name << std::endl;
        std::cout << "  GPA range: 0x" << std::hex << region.gpa_start 
                  << " - 0x" << region.gpa_end << std::dec << std::endl;
        std::cout << "  Size: " << (region.size / (1024*1024)) << " MB" << std::endl;
        std::cout << "  File offset: 0x" << std::hex << region.file_offset << std::dec << std::endl;
        std::cout << std::endl;
    }
    std::cout << "================================================" << std::endl;
}

} // namespace Haywire