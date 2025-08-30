#include "memory_overview.h"
#include "qemu_connection.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace Haywire {

MemoryOverview::MemoryOverview() 
    : startAddress(0x0),          // Start at 0 to see low memory (ROM/bootloader)
      endAddress(0x10000000),     // First 256MB should show ROM and early RAM
      pageSize(4096),
      chunkSize(65536),          // 64KB chunks
      pixelsPerRow(256),
      bytesPerPixel(65536),      // Each pixel = 64KB
      scanning(false),
      scanProgress(0.0f),
      textureID(0) {
    
    // Calculate how many pages/chunks we need to track
    uint64_t range = endAddress - startAddress;
    size_t numChunks = range / chunkSize;
    pageStates.resize(numChunks, PageState::Unknown);
    
    // Create texture for overview
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

MemoryOverview::~MemoryOverview() {
    if (textureID) {
        glDeleteTextures(1, &textureID);
    }
}

void MemoryOverview::ScanMemoryLayout(QemuConnection& qemu) {
    regions.clear();
    
    // Get memory tree from QEMU
    nlohmann::json cmd = {
        {"execute", "human-monitor-command"},
        {"arguments", {
            {"command-line", "info mtree -f"}
        }}
    };
    
    nlohmann::json response;
    if (qemu.SendQMPCommand(cmd, response)) {
        if (response.contains("return")) {
            std::string mtree = response["return"];
            
            // Parse the memory tree output to find populated regions
            std::istringstream iss(mtree);
            std::string line;
            while (std::getline(iss, line)) {
                // Look for actual memory regions (not aliases or containers)
                if ((line.find("ram") != std::string::npos || 
                     line.find("rom") != std::string::npos ||
                     line.find("flash") != std::string::npos) &&
                    line.find("alias") == std::string::npos) {
                    
                    // Parse address range
                    size_t pos = line.find_first_not_of(" ");
                    if (pos != std::string::npos) {
                        line = line.substr(pos);
                    }
                    
                    size_t dash = line.find('-');
                    if (dash != std::string::npos && dash < 20) {
                        std::string startStr = line.substr(0, dash);
                        size_t space = line.find(' ', dash);
                        if (space == std::string::npos) space = line.length();
                        std::string endStr = line.substr(dash + 1, space - dash - 1);
                        
                        try {
                            uint64_t start = std::stoull(startStr, nullptr, 16);
                            uint64_t end = std::stoull(endStr, nullptr, 16);
                            
                            // Only add regions > 1MB
                            if (end - start > 1024*1024) {
                                MemoryRegion region;
                                region.base = start;
                                region.size = end - start + 1;
                                
                                // Extract name
                                size_t colon = line.find(':');
                                if (colon != std::string::npos && colon < line.length() - 1) {
                                    region.name = line.substr(colon + 2);
                                    // Clean up name
                                    size_t paren = region.name.find('(');
                                    if (paren != std::string::npos) {
                                        region.name = region.name.substr(0, paren);
                                    }
                                }
                                
                                regions.push_back(region);
                                std::cout << "Found region: 0x" << std::hex << start 
                                         << "-0x" << end << " " << region.name << std::dec << "\n";
                            }
                        } catch (...) {
                            // Skip malformed lines
                        }
                    }
                }
            }
        }
    }
    
    // Keep a broad view including low memory
    if (!regions.empty()) {
        // Find min/max addresses
        uint64_t minAddr = 0;  // Always start at 0 to see boot ROM
        uint64_t maxAddr = 0;
        
        for (const auto& region : regions) {
            maxAddr = std::max(maxAddr, region.base + region.size);
        }
        
        // Make sure we at least cover first 256MB even if no regions found there
        maxAddr = std::max(maxAddr, uint64_t(0x10000000));
        
        // Align to chunk boundaries
        startAddress = minAddr;
        endAddress = (maxAddr + chunkSize - 1) & ~(chunkSize - 1);
        
        // Resize page states for new range
        uint64_t range = endAddress - startAddress;
        size_t numChunks = range / chunkSize;
        pageStates.resize(numChunks, PageState::Unknown);
        
        std::cout << "Overview range: 0x" << std::hex << startAddress 
                  << " - 0x" << endAddress << std::dec 
                  << " (" << numChunks << " chunks)\n";
    }
}

PageState MemoryOverview::ProbeMemory(QemuConnection& qemu, uint64_t address) {
    // Quick probe - read only first 64 bytes for speed
    std::vector<uint8_t> buffer;
    if (!qemu.ReadMemory(address, 64, buffer)) {
        return PageState::NotPresent;
    }
    
    // Check if all zeros
    bool allZero = true;
    int nonZeroCount = 0;
    
    for (uint8_t b : buffer) {
        if (b != 0) {
            allZero = false;
            nonZeroCount++;
        }
    }
    
    if (allZero) {
        return PageState::Zero;
    }
    
    // Simplified detection for speed
    if (nonZeroCount > 40) {
        return PageState::Data;
    }
    
    return PageState::Data;
}

void MemoryOverview::UpdatePageStates(QemuConnection& qemu) {
    // Super fast initial probe - just check a few key addresses
    // to get something on screen immediately
    
    if (pageStates.empty()) return;
    
    // On first call, just mark everything as unknown and probe a few spots
    static bool firstScan = true;
    if (firstScan) {
        // Mark known regions as potentially present
        for (size_t i = 0; i < pageStates.size(); i++) {
            uint64_t addr = startAddress + (i * chunkSize);
            
            // Check if in a known region
            bool inRegion = false;
            for (const auto& region : regions) {
                if (addr >= region.base && addr < region.base + region.size) {
                    inRegion = true;
                    break;
                }
            }
            
            pageStates[i] = inRegion ? PageState::Unknown : PageState::NotPresent;
        }
        
        // Probe just a few key addresses to start
        std::vector<uint64_t> keyAddresses = {
            0x0,          // Boot ROM
            0x1000,       // Often has vectors
            0x10000,      // Common firmware location
            0x40000000,   // RAM start on many ARM systems
            0x80000000    // Alternative RAM location
        };
        
        for (uint64_t addr : keyAddresses) {
            if (addr >= startAddress && addr < endAddress) {
                size_t idx = (addr - startAddress) / chunkSize;
                if (idx < pageStates.size()) {
                    std::vector<uint8_t> probe;
                    if (qemu.ReadMemory(addr, 16, probe)) {
                        bool allZero = true;
                        for (uint8_t b : probe) {
                            if (b != 0) {
                                allZero = false;
                                break;
                            }
                        }
                        pageStates[idx] = allZero ? PageState::Zero : PageState::Data;
                        
                        // Mark nearby chunks as likely present too
                        for (size_t j = (idx > 10 ? idx - 10 : 0); 
                             j < std::min(idx + 10, pageStates.size()); j++) {
                            if (pageStates[j] == PageState::Unknown) {
                                pageStates[j] = PageState::Data;
                            }
                        }
                    }
                }
            }
        }
        
        firstScan = false;
        scanning = false;
        lastScan = std::chrono::steady_clock::now();
        return;
    }
    
    // After first scan, do very minimal incremental updates
    static size_t incrementalIndex = 0;
    const size_t chunksPerUpdate = 4; // Only 4 chunks per update!
    
    size_t checked = 0;
    while (checked < chunksPerUpdate && incrementalIndex < pageStates.size()) {
        if (pageStates[incrementalIndex] == PageState::Unknown || 
            pageStates[incrementalIndex] == PageState::Data) {
            
            uint64_t addr = startAddress + (incrementalIndex * chunkSize);
            std::vector<uint8_t> probe;
            
            if (qemu.ReadMemory(addr, 16, probe)) {  // Only 16 bytes!
                bool allZero = true;
                for (uint8_t b : probe) {
                    if (b != 0) {
                        allZero = false;
                        break;
                    }
                }
                pageStates[incrementalIndex] = allZero ? PageState::Zero : PageState::Data;
            } else {
                pageStates[incrementalIndex] = PageState::NotPresent;
            }
            checked++;
        }
        
        incrementalIndex = (incrementalIndex + 1) % pageStates.size();
    }
    
    scanning = false;
    lastScan = std::chrono::steady_clock::now();
}

uint32_t MemoryOverview::StateToColor(PageState state) const {
    switch (state) {
        case PageState::Unknown:     return IM_COL32(32, 32, 32, 255);    // Dark gray
        case PageState::NotPresent:  return IM_COL32(0, 0, 0, 255);        // Black
        case PageState::Zero:        return IM_COL32(0, 0, 64, 255);       // Dark blue
        case PageState::Data:        return IM_COL32(0, 128, 0, 255);      // Green
        case PageState::Changing:    return IM_COL32(255, 255, 0, 255);    // Yellow
        case PageState::Executable:  return IM_COL32(128, 0, 128, 255);    // Purple
        case PageState::VideoLike:   return IM_COL32(255, 128, 0, 255);    // Orange
        default:                     return IM_COL32(128, 128, 128, 255);  // Gray
    }
}

void MemoryOverview::DrawCompact() {
    // Compact view without window chrome
    // Show detected regions
    if (!regions.empty()) {
        ImGui::Text("Memory Regions:");
        ImGui::Separator();
        for (const auto& region : regions) {
            if (ImGui::Selectable(region.name.c_str())) {
                // TODO: Jump to this region in main view
            }
            ImGui::Text("  0x%llx (%.1f MB)", 
                       region.base, 
                       region.size / (1024.0 * 1024.0));
        }
        ImGui::Separator();
    }
    
    // Memory map visualization
    if (!pixelBuffer.empty()) {
        int width = pixelsPerRow;
        int height = (pageStates.size() + pixelsPerRow - 1) / pixelsPerRow;
        
        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, 
                     GL_RGBA, GL_UNSIGNED_BYTE, pixelBuffer.data());
        
        // Display the texture
        ImVec2 size(width * 1.5, height * 1.5);  // Slightly smaller scale
        ImGui::Image((ImTextureID)(intptr_t)textureID, size);
        
        // Handle clicks
        if (ImGui::IsItemHovered()) {
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 itemPos = ImGui::GetItemRectMin();
            
            int x = (mousePos.x - itemPos.x) / 1.5;
            int y = (mousePos.y - itemPos.y) / 1.5;
            
            if (x >= 0 && x < width && y >= 0 && y < height) {
                uint64_t address = GetAddressAt(x, y);
                ImGui::SetTooltip("Address: 0x%llx", address);
            }
        }
    } else {
        ImGui::Text("No memory scanned yet");
    }
}

void MemoryOverview::Draw() {
    ImGui::Begin("Memory Overview");
    
    // Show detected regions
    if (!regions.empty()) {
        ImGui::Text("Populated Memory Regions:");
        for (const auto& region : regions) {
            ImGui::Text("  0x%llx - 0x%llx (%s, %.1f MB)", 
                       region.base, 
                       region.base + region.size - 1,
                       region.name.c_str(),
                       region.size / (1024.0 * 1024.0));
        }
        ImGui::Separator();
    }
    
    // Status
    if (scanning) {
        ImGui::Text("Scanning: %.1f%%", scanProgress * 100.0f);
    } else {
        if (lastScan.time_since_epoch().count() > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<float>(now - lastScan).count();
            ImGui::Text("Last scan: %.1f seconds ago", elapsed);
        } else {
            ImGui::Text("Not scanned yet");
        }
    }
    
    ImGui::Separator();
    
    // Legend
    ImGui::Text("Legend:");
    ImGui::SameLine(); ImGui::ColorButton("Not Present", ImVec4(0, 0, 0, 1), 0, ImVec2(20, 20));
    ImGui::SameLine(); ImGui::Text("Unmapped");
    
    ImGui::SameLine(); ImGui::ColorButton("Zero", ImVec4(0, 0, 0.25, 1), 0, ImVec2(20, 20));
    ImGui::SameLine(); ImGui::Text("Zeros");
    
    ImGui::SameLine(); ImGui::ColorButton("Data", ImVec4(0, 0.5, 0, 1), 0, ImVec2(20, 20));
    ImGui::SameLine(); ImGui::Text("Data");
    
    ImGui::SameLine(); ImGui::ColorButton("Video", ImVec4(1, 0.5, 0, 1), 0, ImVec2(20, 20));
    ImGui::SameLine(); ImGui::Text("Video?");
    
    ImGui::Separator();
    
    // Debug checkbox to test rendering
    static bool debugRandomColors = false;
    ImGui::Checkbox("Debug: Random Colors", &debugRandomColors);
    
    // Memory map visualization
    if (pageStates.empty() && !debugRandomColors) {
        ImGui::Text("No memory scanned yet. Click 'Scan Memory Layout' first.");
    } else {
        if (!debugRandomColors) {
            ImGui::Text("Showing only populated regions:");
            ImGui::Text("Range: 0x%llx - 0x%llx", startAddress, endAddress);
            ImGui::Text("Each pixel = %d KB", (int)(bytesPerPixel / 1024));
            ImGui::Text("Total: %.1f GB covered", (endAddress - startAddress) / (1024.0 * 1024.0 * 1024.0));
        } else {
            ImGui::Text("DEBUG MODE: Showing random colors");
            // Make sure we have some data to display
            if (pageStates.empty()) {
                pageStates.resize(256 * 128, PageState::Unknown); // Create a test grid
            }
        }
        
        // Update pixel buffer
        pixelBuffer.resize(pageStates.size());
        
        if (debugRandomColors) {
            // Fill with random colors for testing
            for (size_t i = 0; i < pixelBuffer.size(); i++) {
                // Create a pattern of colors
                int pattern = i % 7;
                switch(pattern) {
                    case 0: pixelBuffer[i] = IM_COL32(255, 0, 0, 255); break;     // Red
                    case 1: pixelBuffer[i] = IM_COL32(0, 255, 0, 255); break;     // Green
                    case 2: pixelBuffer[i] = IM_COL32(0, 0, 255, 255); break;     // Blue
                    case 3: pixelBuffer[i] = IM_COL32(255, 255, 0, 255); break;   // Yellow
                    case 4: pixelBuffer[i] = IM_COL32(255, 0, 255, 255); break;   // Magenta
                    case 5: pixelBuffer[i] = IM_COL32(0, 255, 255, 255); break;   // Cyan
                    case 6: pixelBuffer[i] = IM_COL32(128, 128, 128, 255); break; // Gray
                }
            }
        } else {
            for (size_t i = 0; i < pageStates.size(); i++) {
                // Check if this chunk is in a known region
                uint64_t chunkAddr = startAddress + (i * chunkSize);
                bool inRegion = false;
                
                for (const auto& region : regions) {
                    if (chunkAddr >= region.base && chunkAddr < region.base + region.size) {
                        inRegion = true;
                        break;
                    }
                }
                
                // If not in any region, mark as definitely not present
                if (!inRegion && pageStates[i] == PageState::Unknown) {
                    pixelBuffer[i] = IM_COL32(16, 16, 16, 255); // Very dark gray for gaps
                } else {
                    pixelBuffer[i] = StateToColor(pageStates[i]);
                }
            }
        }
    }
    
    // Update texture
    if (!pixelBuffer.empty()) {
        int width = pixelsPerRow;
        int height = (pageStates.size() + pixelsPerRow - 1) / pixelsPerRow;
        
        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, 
                     GL_RGBA, GL_UNSIGNED_BYTE, pixelBuffer.data());
        
        // Display the texture
        ImVec2 size(width * 2, height * 2);  // Scale up for visibility
        ImGui::Image((ImTextureID)(intptr_t)textureID, size);
        
        // Handle clicks
        if (ImGui::IsItemHovered()) {
            ImVec2 mousePos = ImGui::GetMousePos();
            ImVec2 itemPos = ImGui::GetItemRectMin();
            
            int x = (mousePos.x - itemPos.x) / 2;
            int y = (mousePos.y - itemPos.y) / 2;
            
            if (x >= 0 && x < width && y >= 0 && y < height) {
                uint64_t address = GetAddressAt(x, y);
                ImGui::SetTooltip("Address: 0x%llx", address);
                
                if (ImGui::IsMouseClicked(0)) {
                    // Jump to this address in main view
                    std::cout << "Jump to: 0x" << std::hex << address << std::dec << "\n";
                }
            }
        }
    }
    
    ImGui::End();
}

void MemoryOverview::UpdateRegion(uint64_t address, size_t size, const uint8_t* data) {
    // When the main visualizer reads memory, update our overview
    if (!data || size == 0) return;
    
    // Find which chunks this read covers
    if (address < startAddress || address >= endAddress) return;
    
    uint64_t endAddr = address + size;
    uint64_t startChunk = (address - startAddress) / chunkSize;
    uint64_t endChunk = (endAddr - startAddress + chunkSize - 1) / chunkSize;
    
    for (uint64_t chunk = startChunk; chunk < endChunk && chunk < pageStates.size(); chunk++) {
        // Sample some bytes from this chunk's portion
        uint64_t chunkAddr = startAddress + (chunk * chunkSize);
        uint64_t overlapStart = std::max(address, chunkAddr);
        uint64_t overlapEnd = std::min(endAddr, chunkAddr + chunkSize);
        
        if (overlapStart < overlapEnd) {
            size_t offset = overlapStart - address;
            size_t checkSize = std::min(size_t(64), size_t(overlapEnd - overlapStart));
            
            bool allZero = true;
            for (size_t i = 0; i < checkSize; i++) {
                if (data[offset + i] != 0) {
                    allZero = false;
                    break;
                }
            }
            
            // Update state based on what we see
            PageState newState = allZero ? PageState::Zero : PageState::Data;
            if (pageStates[chunk] == PageState::Unknown || pageStates[chunk] == PageState::NotPresent) {
                pageStates[chunk] = newState;
            } else if (pageStates[chunk] != newState && pageStates[chunk] != PageState::Changing) {
                // Data changed since last time
                pageStates[chunk] = PageState::Changing;
            }
        }
    }
}

uint64_t MemoryOverview::GetAddressAt(int x, int y) const {
    int index = y * pixelsPerRow + x;
    if (index >= 0 && index < pageStates.size()) {
        return startAddress + (index * chunkSize);
    }
    return 0;
}

}