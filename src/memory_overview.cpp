#include "memory_overview.h"
#include "qemu_connection.h"
#include "guest_agent.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <functional>
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
      textureID(0),
      processMode(false),
      targetPid(-1),
      flattener(nullptr) {
    
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
    
    // Switch between physical and process mode
    if (processMode) {
        DrawProcessMap();
    } else {
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
    }  // End of else block (physical mode)
}

void MemoryOverview::Draw() {
    ImGui::Begin("Memory Overview");
    
    // Switch between physical and process mode
    if (processMode) {
        DrawProcessMap();
    } else {
        // Original physical memory view
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
    }  // End of else block (physical mode)
    
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

void MemoryOverview::SetProcessMode(bool enabled, int pid) {
    processMode = enabled;
    targetPid = pid;
}

void MemoryOverview::LoadProcessMap(GuestAgent* agent) {
    if (agent && targetPid > 0) {
        processMap.LoadProcess(targetPid, agent);
    }
}

void MemoryOverview::LoadProcessSections(const std::vector<GuestMemoryRegion>& regions) {
    // Store sections for display
    processSections = regions;
    
    // If we have a flattener, update it with the new regions
    if (flattener) {
        flattener->BuildFromRegions(regions);
    }
}

void MemoryOverview::DrawProcessMap() {
    if (targetPid <= 0) {
        ImGui::Text("No process selected");
        ImGui::Text("Enable VA Mode and load a process memory map");
        return;
    }
    
    ImGui::Text("Process %d Memory Map", targetPid);
    ImGui::Separator();
    
    // Show sections list if available
    if (!processSections.empty()) {
        if (ImGui::CollapsingHeader("Memory Sections", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginChild("SectionsList", ImVec2(0, 150), true, 
                            ImGuiWindowFlags_HorizontalScrollbar);
            
            // Table with columns for address range, permissions, size, and name
            if (ImGui::BeginTable("Sections", 4, 
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | 
                                ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Address Range", ImGuiTableColumnFlags_WidthFixed, 250);
                ImGui::TableSetupColumn("Perms", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                
                for (const auto& section : processSections) {
                    ImGui::TableNextRow();
                    
                    // Make the entire row selectable
                    ImGui::TableSetColumnIndex(0);
                    char label[64];
                    snprintf(label, sizeof(label), "##section_%llx", section.start);
                    if (ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns)) {
                        if (navCallback) {
                            navCallback(section.start);
                        }
                    }
                    
                    // Draw the actual content
                    ImGui::SameLine();
                    ImGui::Text("0x%llx-0x%llx", section.start, section.end);
                    
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", section.permissions.c_str());
                    
                    ImGui::TableSetColumnIndex(2);
                    uint64_t size = section.end - section.start;
                    if (size >= 1024*1024) {
                        ImGui::Text("%.1f MB", size / (1024.0 * 1024.0));
                    } else if (size >= 1024) {
                        ImGui::Text("%.1f KB", size / 1024.0);
                    } else {
                        ImGui::Text("%llu B", size);
                    }
                    
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%s", section.name.c_str());
                }
                
                ImGui::EndTable();
            }
            
            ImGui::EndChild();
        }
        
        ImGui::Separator();
        ImGui::Text("Crunched Address Space View:");
    }
    
    if (!flattener) {
        ImGui::Text("No address space flattener available");
        return;
    }
    
    // Show stats
    uint64_t totalMapped = flattener->GetMappedSize();
    float compression = 1.0f / flattener->GetCompressionRatio();
    ImGui::Text("Total Mapped: %.2f MB", totalMapped / (1024.0 * 1024.0));
    ImGui::Text("Compression: %.1f:1", compression);
    ImGui::Separator();
    
    // Draw the crunched memory map
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    
    if (canvasSize.x < 100) canvasSize.x = 250;  // Min width
    if (canvasSize.y < 200) canvasSize.y = 400;  // Min height
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(canvasPos, 
                           ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                           IM_COL32(20, 20, 20, 255));
    
    // Draw regions as horizontal bars
    const auto& regions = flattener->GetRegions();
    uint64_t totalFlat = flattener->GetFlatSize();
    
    if (totalFlat > 0) {
        for (const auto& region : regions) {
            // Calculate position in canvas
            float y1 = canvasPos.y + (region.flatStart / (float)totalFlat) * canvasSize.y;
            float y2 = canvasPos.y + (region.flatEnd / (float)totalFlat) * canvasSize.y;
            
            // Minimum height for visibility
            if (y2 - y1 < 1.0f) y2 = y1 + 1.0f;
            
            // Color based on region name
            uint32_t color = IM_COL32(100, 100, 100, 255);  // Default gray
            if (region.name.find("stack") != std::string::npos) {
                color = IM_COL32(255, 100, 255, 255);  // Magenta for stack
            } else if (region.name.find("heap") != std::string::npos) {
                color = IM_COL32(255, 255, 100, 255);  // Yellow for heap
            } else if (region.name.find(".so") != std::string::npos) {
                color = IM_COL32(100, 255, 255, 255);  // Cyan for libraries
            } else if (region.name[0] == '/' && region.name.find("bin") != std::string::npos) {
                color = IM_COL32(100, 100, 255, 255);  // Blue for executables
            } else if (region.name.empty() || region.name[0] == '[') {
                color = IM_COL32(150, 150, 150, 255);  // Light gray for anonymous
            }
            
            // Draw the region bar
            drawList->AddRectFilled(ImVec2(canvasPos.x + 10, y1),
                                   ImVec2(canvasPos.x + canvasSize.x - 10, y2),
                                   color);
            
            // Hover detection
            if (ImGui::IsMouseHoveringRect(ImVec2(canvasPos.x, y1), 
                                         ImVec2(canvasPos.x + canvasSize.x, y2))) {
                // Highlight
                drawList->AddRect(ImVec2(canvasPos.x + 10, y1),
                                ImVec2(canvasPos.x + canvasSize.x - 10, y2),
                                IM_COL32(255, 255, 255, 255), 0, 0, 2);
                
                // Tooltip
                ImGui::BeginTooltip();
                ImGui::Text("VA: 0x%llx - 0x%llx", region.virtualStart, region.virtualEnd);
                ImGui::Text("Size: %.2f MB", region.Size() / (1024.0 * 1024.0));
                if (!region.name.empty()) {
                    ImGui::Text("Name: %s", region.name.c_str());
                }
                ImGui::Text("Position in flat space: %llu MB", 
                           region.flatStart / (1024 * 1024));
                ImGui::EndTooltip();
                
                // Click to navigate
                if (ImGui::IsMouseClicked(0) && navCallback) {
                    navCallback(region.virtualStart);
                }
            }
        }
    }
    
    // Make the canvas interactive
    ImGui::InvisibleButton("ProcessMapCanvas", canvasSize);
    
    // Legend
    ImGui::Separator();
    ImGui::Text("Legend:");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.4f, 0.4f, 1.0f, 1), "Executable");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1), "Heap");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.4f, 1.0f, 1), "Stack");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1), "Libraries");
}

}