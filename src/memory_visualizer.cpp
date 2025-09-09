#include "memory_visualizer.h"
#include "qemu_connection.h"
#include "viewport_translator.h"
#include "address_space_flattener.h"
#include "crunched_memory_reader.h"
#include "guest_agent.h"
#include "imgui.h"
#include <cstring>
#include <string.h>  // For memmem
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <chrono>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace Haywire {

MemoryVisualizer::MemoryVisualizer() 
    : memoryTexture(0), needsUpdate(true), autoRefresh(false), autoRefreshInitialized(false),
      refreshRate(10.0f), showHexOverlay(false), showNavigator(true), showCorrelation(false),
      showChangeHighlight(true), showMagnifier(false),  // Magnifier off by default
      widthInput(640), heightInput(480), strideInput(640),  // Back to reasonable video dimensions
      pixelFormatIndex(0), mouseX(0), mouseY(0), isDragging(false),
      dragStartX(0), dragStartY(0), isReading(false), readComplete(false),
      marchingAntsPhase(0.0f), magnifierZoom(8), magnifierLocked(false),
      magnifierSize(32), magnifierLockPos(0, 0), memoryViewPos(0, 0), memoryViewSize(0, 0),
      targetPid(-1), useVirtualAddresses(false), guestAgent(nullptr),
      searchType(SEARCH_ASCII), currentSearchResult(0), searchActive(false), searchFromCurrent(false),
      searchFullRange(true), memFd(-1), memBase(nullptr), memSize(0) {
    
    memset(searchPattern, 0, sizeof(searchPattern));
    
    addressFlattener = std::make_unique<AddressSpaceFlattener>();
    crunchedNavigator = std::make_unique<CrunchedRangeNavigator>();
    crunchedNavigator->SetFlattener(addressFlattener.get());
    crunchedReader = std::make_unique<CrunchedMemoryReader>();
    crunchedReader->SetFlattener(addressFlattener.get());
    
    // Set navigation callback
    crunchedNavigator->SetNavigationCallback([this](uint64_t virtualAddr) {
        NavigateToAddress(virtualAddr);
    });
    
    strcpy(addressInput, "0x0");  // Start at 0 where boot ROM lives
    viewport.baseAddress = 0x0;  // Initialize the actual address!
    viewport.width = widthInput;
    viewport.height = heightInput;
    viewport.stride = strideInput;
    
    // Start with smaller viewport in VA mode to reduce translation overhead
    if (useVirtualAddresses) {
        viewport.width = 256;
        viewport.height = 256;
        viewport.stride = 256;
        widthInput = 256;
        heightInput = 256;
        strideInput = 256;
    }
    CreateTexture();
    lastRefresh = std::chrono::steady_clock::now();
}

MemoryVisualizer::~MemoryVisualizer() {
    // Stop any ongoing read
    if (readThread.joinable()) {
        isReading = false;
        readThread.join();
    }
    
    if (memoryTexture) {
        glDeleteTextures(1, &memoryTexture);
    }
    
    // Cleanup memory map
    CleanupMemoryMap();
}

void MemoryVisualizer::SetTranslator(std::shared_ptr<ViewportTranslator> translator) {
    viewportTranslator = translator;
    if (crunchedReader) {
        crunchedReader->SetTranslator(translator);
    }
}

void MemoryVisualizer::SetBeaconTranslator(std::shared_ptr<BeaconTranslator> beaconTranslator) {
    if (crunchedReader) {
        crunchedReader->SetBeaconTranslator(beaconTranslator);
    }
}

void MemoryVisualizer::SetProcessPid(int pid) {
    targetPid = pid;
    if (crunchedReader) {
        crunchedReader->SetPID(pid);
    }
}

void MemoryVisualizer::CreateTexture() {
    if (memoryTexture) {
        glDeleteTextures(1, &memoryTexture);
    }
    
    glGenTextures(1, &memoryTexture);
    glBindTexture(GL_TEXTURE_2D, memoryTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void MemoryVisualizer::DrawControlBar(QemuConnection& qemu) {
    // Ensure crunched reader has the connection
    if (crunchedReader && !crunchedReader->GetConnection()) {
        crunchedReader->SetConnection(&qemu);
    }
    
    // Horizontal layout for controls
    DrawControls();
    
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    // Check if async read completed
    if (readComplete.exchange(false)) {
        std::lock_guard<std::mutex> lock(memoryMutex);
        currentMemory = std::move(pendingMemory);  // Just replace atomically
        needsUpdate = true;  // Mark for update in main thread
        
        // Debug: confirm texture was updated
        static int updateCount = 0;
        if (++updateCount % 10 == 0) {
            std::cerr << "Texture updated " << updateCount << " times\n";
        }
    }
    
    // Update texture in main thread (OpenGL context)
    if (needsUpdate && !currentMemory.data.empty()) {
        UpdateTexture();
        needsUpdate = false;
        glFlush();
    }
    
    // Auto-enable refresh - always on for zero-config experience
    if (!autoRefreshInitialized) {
        autoRefresh = true;
        // Set refresh rate based on connection type
        // Memory backend can handle much higher refresh rates
        refreshRate = qemu.IsUsingMemoryBackend() ? 30.0f : 5.0f;
        autoRefreshInitialized = true;
        
        // std::cerr << "Auto-refresh enabled at " << refreshRate << " Hz"
        //           << (qemu.IsUsingMemoryBackend() ? " (memory backend)" : " (QMP/GDB)") << "\n";
    }
    
    // Simple synchronous refresh for testing
    if (qemu.IsConnected()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<float>(now - lastRefresh).count();
        
        if (elapsed >= 1.0f / refreshRate) {
            // Do a simple, direct read - no threading
            size_t size = viewport.stride * viewport.height;
            uint64_t addr = viewport.baseAddress;
            
            std::vector<uint8_t> buffer;
            bool readSuccess = false;
            
            // Use crunched reader if in VA mode with a process
            if (useVirtualAddresses && crunchedReader && targetPid > 0) {
                size_t bytesRead = crunchedReader->ReadCrunchedMemory(addr, size, buffer);
                readSuccess = (bytesRead > 0);
                if (!readSuccess) {
                    static int failCount = 0;
                    if (++failCount % 10 == 0) {
                        std::cerr << "CrunchedMemoryReader failed at flat addr 0x" 
                                  << std::hex << addr << std::dec 
                                  << " (fail #" << failCount << ")" << std::endl;
                    }
                }
            } else {
                // Direct physical memory read
                readSuccess = qemu.ReadMemory(addr, size, buffer);
            }
            
            if (readSuccess) {
                // Performance monitoring for problematic addresses
                static int volatileCount = 0;
                static uint64_t lastVolatileAddr = 0;
                
                // Just show the actual memory - we know it's changing
                static std::vector<uint8_t> lastBuffer;
                static uint64_t lastAddr = 0;
                
                // Only compare if we're at the same address
                if (!lastBuffer.empty() && lastBuffer.size() == buffer.size() && lastAddr == addr) {
                    int changedBytes = 0;
                    int firstChangeOffset = -1;
                    std::vector<ChangeRegion> currentChanges;  // Changes for this frame
                    
                    for (size_t i = 0; i < buffer.size(); i++) {
                        if (buffer[i] != lastBuffer[i]) {
                            changedBytes++;
                            
                            // Calculate X,Y position in the viewport
                            int x = (i % viewport.stride) / viewport.format.bytesPerPixel;
                            int y = i / viewport.stride;
                            
                            // Add to current frame's changes
                            bool foundAdjacentRegion = false;
                            for (auto& region : currentChanges) {
                                // Merge adjacent changes into single box
                                if (region.y == y && 
                                    (region.x + region.width == x || region.x == x + 1)) {
                                    region.width = std::max(region.x + region.width, x + 1) - std::min(region.x, x);
                                    region.x = std::min(region.x, x);
                                    foundAdjacentRegion = true;
                                    break;
                                }
                            }
                            if (!foundAdjacentRegion) {
                                // Limit number of regions to prevent performance issues
                                if (currentChanges.size() < 100) {
                                    currentChanges.push_back({x, y, 1, 1, std::chrono::steady_clock::now()});
                                } else if (currentChanges.size() == 100) {
                                    // Too many changes - likely hit volatile memory
                                    // std::cerr << "Warning: >100 change regions at 0x" << std::hex << addr << " - limiting\n" << std::dec;
                                }
                            }
                            
                            if (firstChangeOffset == -1) {
                                firstChangeOffset = i;
                            }
                        }
                    }
                    
                    if (changedBytes > 0) {
                        // Detect highly volatile memory regions
                        float changeRatio = (float)changedBytes / buffer.size();
                        if (changeRatio > 0.5f) {  // More than 50% changed
                            volatileCount++;
                            if (volatileCount == 10 && addr != lastVolatileAddr) {
                                std::cerr << "Warning: Highly volatile memory at 0x" << std::hex << addr 
                                         << " (" << std::dec << (int)(changeRatio * 100) << "% changing)\n"
                                         << "Consider reducing refresh rate or skipping this region\n";
                                lastVolatileAddr = addr;
                                
                                // Reduce refresh rate for this region
                                refreshRate = std::min(refreshRate, 5.0f);
                            }
                        } else {
                            volatileCount = 0;
                        }
                        
                        // Add to ring buffer
                        changeHistory.push_back(currentChanges);
                        if (changeHistory.size() > CHANGE_HISTORY_SIZE) {
                            changeHistory.pop_front();
                        }
                        lastChangeTime = std::chrono::steady_clock::now();
                        
                        // Reduced logging - only report significant changes
                        // if (changedBytes > 100) {
                        //     std::cerr << changedBytes << " bytes changed in " 
                        //              << currentChanges.size() << " regions\n";
                        // }
                    }
                } else if (lastAddr != addr) {
                    changeHistory.clear();  // Clear history when address changes
                    // std::cerr << "Address changed to 0x" << std::hex << addr 
                    //          << " - starting fresh comparison\n" << std::dec;
                }
                
                lastBuffer = buffer;
                lastAddr = addr;
                
                currentMemory.address = addr;
                currentMemory.data = std::move(buffer);
                currentMemory.stride = viewport.stride;
                
                // Skip texture updates if memory is thrashing
                if (volatileCount < 20) {  // Only update if not excessively volatile
                    UpdateTexture();  // Update immediately
                } else {
                    static int skipCount = 0;
                    if (++skipCount % 10 == 0) {  // Update every 10th frame when volatile
                        UpdateTexture();
                    }
                }
                lastRefresh = now;
                
                // std::cerr << "Direct read and texture update at " << std::hex << addr << std::dec << "\n";
            }
        }
    }
    
    // Comment out all the async thread code for now
    /*
            readThread = std::thread([this, &qemu, addr, size, stride]() {
                std::vector<uint8_t> buffer;
                static int readCount = 0;
                readCount++;
                
                // Log every 10 reads to verify we're actually reading
                if (readCount % 10 == 0) {
                    std::cerr << "Read #" << readCount << " from 0x" << std::hex << addr 
                              << " size: " << std::dec << size;
                    
                    // Force a timestamp to ensure we're getting fresh data
                    auto now = std::chrono::system_clock::now();
                    auto time_t = std::chrono::system_clock::to_time_t(now);
                    std::cerr << " at " << std::ctime(&time_t);
                }
                
                if (qemu.ReadMemory(addr, size, buffer)) {
                    // Calculate a simple checksum of the entire buffer
                    uint32_t checksum = 0;
                    for (size_t i = 0; i < buffer.size(); i += 4) {
                        if (i + 3 < buffer.size()) {
                            checksum ^= *(uint32_t*)&buffer[i];
                        }
                    }
                    
                    static uint32_t lastChecksum = 0;
                    static uint64_t lastAddr = 0;
                    
                    if (lastAddr == addr && lastChecksum != checksum && lastChecksum != 0) {
                        std::cerr << "CHECKSUM CHANGED! Old: 0x" << std::hex << lastChecksum 
                                 << " New: 0x" << checksum << " at address 0x" << addr << std::dec << "\n";
                    }
                    
                    lastChecksum = checksum;
                    
                    // Sample more bytes to detect changes (check multiple regions)
                    static std::vector<uint8_t> lastSample;
                    
                    // Check up to 4KB or 10% of buffer, whichever is smaller
                    size_t sampleSize = std::min({size_t(4096), buffer.size(), buffer.size() / 10});
                    std::vector<uint8_t> sample(buffer.begin(), buffer.begin() + sampleSize);
                    
                    // Only compare if we're still looking at the same address
                    if (!lastSample.empty() && lastAddr == addr && sample != lastSample) {
                        // Count how many bytes changed
                        int changedBytes = 0;
                        for (size_t i = 0; i < sampleSize; i++) {
                            if (i < lastSample.size() && sample[i] != lastSample[i]) {
                                changedBytes++;
                            }
                        }
                        
                        // Report every change, with more detail
                        if (changedBytes > 0) {
                            // Track change rate for UI indicator
                            static auto lastChangeTime = std::chrono::steady_clock::now();
                            lastChangeTime = std::chrono::steady_clock::now();
                            
                            std::cerr << "Memory changed: " << changedBytes << "/" << sampleSize 
                                     << " bytes at 0x" << std::hex << addr 
                                     << " (read #" << std::dec << readCount << ")\n";
                            
                            // Show first few changed bytes
                            if (changedBytes <= 10) {
                                for (size_t i = 0; i < sampleSize && changedBytes > 0; i++) {
                                    if (i < lastSample.size() && sample[i] != lastSample[i]) {
                                        std::cerr << "  [" << i << "]: 0x" << std::hex 
                                                 << (int)lastSample[i] << " -> 0x" 
                                                 << (int)sample[i] << std::dec << "\n";
                                        changedBytes--;
                                    }
                                }
                            }
                        }
                    }
                    // Silently reset detection when address changes (scrolling)
                    
                    lastSample = sample;
                    lastAddr = addr;
                    
                    std::lock_guard<std::mutex> lock(memoryMutex);
                    pendingMemory.address = addr;
                    pendingMemory.data = std::move(buffer);
                    pendingMemory.stride = stride;
                    readComplete = true;
                }
                
                isReading = false;
            });
            
            lastRefresh = now;
        }
    }
    */
    
}

void MemoryVisualizer::DrawMemoryBitmap() {
    // Get available size (already constrained by parent)
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    
    // Use full available height
    float maxHeight = std::max(50.0f, availSize.y);
    
    // Ensure we have enough space
    if (maxHeight < 50) {
        ImGui::Text("Window too small - please resize");
        return;
    }
    
    heightInput = (int)(maxHeight / viewport.zoom);  // Visible rows
    viewport.height = std::max(1, heightInput);
    
    // Layout: Vertical slider on left, memory view on right
    float sliderWidth = 200;
    float memoryWidth = std::max(100.0f, availSize.x - sliderWidth - 10);  // -10 for spacing
    
    // Vertical address slider on the left - use constrained height
    ImGui::BeginChild("AddressSlider", ImVec2(sliderWidth, maxHeight), true);
    DrawVerticalAddressSlider();
    ImGui::EndChild();
    
    ImGui::SameLine();
    
    // Memory view on the right - use constrained height
    ImGui::BeginChild("BitmapView", ImVec2(memoryWidth, maxHeight), false);
    DrawMemoryView();
    ImGui::EndChild();
    
    // Texture updates now happen immediately when data arrives
    // This ensures smooth 30Hz refresh with memory backend
}

void MemoryVisualizer::Draw(QemuConnection& qemu) {
    // Legacy method for compatibility - combines both
    ImGui::Columns(2, "VisualizerColumns", true);
    ImGui::SetColumnWidth(0, 300);
    
    DrawControlBar(qemu);
    
    ImGui::NextColumn();
    
    DrawMemoryBitmap();
    
    ImGui::Columns(1);
}

void MemoryVisualizer::DrawControls() {
    // First row: Address, Width, Height, Format, Refresh
    ImGui::Text("Addr:");
    ImGui::SameLine();
    ImGui::PushItemWidth(120);
    if (ImGui::InputText("##Address", addressInput, sizeof(addressInput),
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::stringstream ss;
        ss << std::hex << addressInput;
        uint64_t inputAddress;
        ss >> inputAddress;
        // Use NavigateToAddress to handle VA translation if needed
        NavigateToAddress(inputAddress);
    }
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    ImGui::Text("W:");
    ImGui::SameLine();
    ImGui::PushItemWidth(80);  // Bigger width field
    if (ImGui::InputInt("##Width", &widthInput)) {
        viewport.width = std::max(1, widthInput);
        viewport.stride = viewport.width;
        strideInput = viewport.stride;
        needsUpdate = true;  // Immediate update
    }
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    ImGui::Text("H:");
    ImGui::SameLine();
    ImGui::PushItemWidth(80);  // Shows visible window height
    ImGui::Text("%d", heightInput);  // Display only, not editable
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Visible window height in pixels");
    }
    
    ImGui::SameLine();
    const char* formats[] = { "RGB888", "RGBA8888", "BGR888", "BGRA8888", 
                              "ARGB8888", "ABGR8888", "RGB565", "Grayscale", "Binary" };
    ImGui::PushItemWidth(100);
    if (ImGui::Combo("##Format", &pixelFormatIndex, formats, IM_ARRAYSIZE(formats))) {
        viewport.format = PixelFormat(static_cast<PixelFormat::Type>(pixelFormatIndex));
        needsUpdate = true;  // Immediate update
    }
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    ImGui::Checkbox("Hex", &showHexOverlay);
    
    ImGui::SameLine();
    ImGui::Checkbox("Corr", &showCorrelation);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show autocorrelation for width detection");
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("Changes", &showChangeHighlight);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Highlight memory changes with yellow boxes");
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("Inspector", &showMagnifier);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show magnifier & search tool (Press 'M' to bring to front, Ctrl+F to search)");
    }
    
    // VA/PA translation controls
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    
    if (ImGui::Checkbox("VA Mode", &useVirtualAddresses)) {
        if (useVirtualAddresses) {
            // Switching to VA mode
            if (addressFlattener && addressFlattener->GetFlatSize() > 0) {
                // Get the first valid region's flat start
                const auto* firstRegion = addressFlattener->GetRegionForFlat(0);
                if (!firstRegion) {
                    // Try to find the first region
                    for (uint64_t testFlat = 0; testFlat < addressFlattener->GetFlatSize(); testFlat += 4096) {
                        firstRegion = addressFlattener->GetRegionForFlat(testFlat);
                        if (firstRegion) {
                            viewport.baseAddress = firstRegion->flatStart;
                            break;
                        }
                    }
                } else {
                    viewport.baseAddress = 0;
                }
                needsUpdate = true;
                
                // Update address display to show first VA
                uint64_t firstVA = addressFlattener->FlatToVirtual(viewport.baseAddress);
                std::stringstream ss;
                ss << "0x" << std::hex << firstVA;
                strcpy(addressInput, ss.str().c_str());
                
                std::cerr << "Switched to VA mode, flat size: " 
                          << addressFlattener->GetFlatSize() / (1024*1024) << " MB"
                          << ", starting at flat 0x" << std::hex << viewport.baseAddress << std::dec << std::endl;
            } else {
                std::cerr << "VA mode enabled but no memory map loaded" << std::endl;
            }
        } else {
            // Switching back to physical mode
            viewport.baseAddress = 0;
            strcpy(addressInput, "0x0");
            needsUpdate = true;
            std::cerr << "Switched to physical mode" << std::endl;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enter virtual addresses (auto-translates to physical)");
    }
    
    if (useVirtualAddresses) {
        ImGui::SameLine();
        ImGui::Text("PID:");
        ImGui::SameLine();
        ImGui::PushItemWidth(80);  // Increased from 60 to 80 for 5-digit PIDs
        int oldPid = targetPid;
        if (ImGui::InputInt("##PID", &targetPid, 0, 0)) {  // Added 0,0 to disable +/- buttons
            if (targetPid > 0) {
                // Update the crunched reader with new PID
                SetProcessPid(targetPid);
                
                if (viewportTranslator) {
                    viewportTranslator->ClearCache(targetPid);
                }
                
                std::cerr << "Set PID to " << targetPid << std::endl;
            }
        }
        ImGui::PopItemWidth();
        
        // Load memory map button
        ImGui::SameLine();
        if (ImGui::Button("Load Map") && targetPid > 0 && guestAgent) {
            // Check if we have everything needed
            if (!viewportTranslator) {
                std::cerr << "ERROR: No viewport translator! Guest agent may not be connected." << std::endl;
                std::cerr << "Please ensure QEMU is connected with guest agent." << std::endl;
            }
            
            std::vector<GuestMemoryRegion> regions;
            if (guestAgent->GetMemoryMap(targetPid, regions)) {
                LoadMemoryMap(regions);
                
                // Make sure crunched reader has the PID
                SetProcessPid(targetPid);
                
                std::cerr << "Loaded memory map for PID " << targetPid 
                         << " (" << regions.size() << " regions)" << std::endl;
                
                // Notify any listeners that we've loaded a process map
                if (onProcessMapLoaded) {
                    onProcessMapLoaded(targetPid, regions);
                }
            } else {
                std::cerr << "Failed to load memory map for PID " << targetPid << std::endl;
            }
        }
        
        // Show cache stats if available
        if (viewportTranslator) {
            auto stats = viewportTranslator->GetStats();
            if (stats.totalEntries > 0) {
                ImGui::SameLine();
                ImGui::Text("Cache: %.1f%% (%zu entries)", 
                           stats.hitRate * 100.0f, stats.totalEntries);
            }
        }
    }
    
    // Refresh rate is now automatic based on connection type
    // Memory backend: 30Hz, Others: 5Hz
    
    if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 windowPos = ImGui::GetWindowPos();
        int x = (mousePos.x - windowPos.x - viewport.panX) / viewport.zoom;
        int y = (mousePos.y - windowPos.y - viewport.panY) / viewport.zoom;
        
        if (x >= 0 && x < viewport.width && y >= 0 && y < viewport.height) {
            uint64_t addr = GetAddressAt(x, y);
            ImGui::SetTooltip("Address: 0x%llx", addr);
        }
    }
}

void MemoryVisualizer::DrawVerticalAddressSlider() {
    ImGui::Text("Memory Navigation");
    ImGui::Separator();
    
    uint64_t sliderUnit;
    uint64_t maxAddress;
    uint64_t currentPos;
    
    if (useVirtualAddresses && addressFlattener) {
        // Crunched mode - slider covers flattened range
        sliderUnit = 4096;  // Page size for alignment
        maxAddress = addressFlattener->GetFlatSize();
        currentPos = viewport.baseAddress;  // This is flat position in VA mode
        
        // Show current VA and region info
        uint64_t currentVA = addressFlattener->FlatToVirtual(currentPos);
        ImGui::Text("VA: 0x%llx", currentVA);
        ImGui::Text("Flat: %llu MB", currentPos / (1024*1024));
        
        // Show current region info
        const auto* region = addressFlattener->GetRegionForFlat(currentPos);
        if (region) {
            ImGui::Separator();
            ImGui::Text("Region: %.32s", region->name.c_str());
            if (region->name.length() > 32) {
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", region->name.c_str());
                }
            }
            uint64_t regionSize = region->virtualEnd - region->virtualStart;
            ImGui::Text("Size: %.1f MB", regionSize / (1024.0 * 1024.0));
            uint64_t offsetInRegion = currentVA - region->virtualStart;
            ImGui::Text("Offset: +0x%llx", offsetInRegion);
        }
    } else {
        // Physical mode - original behavior
        sliderUnit = 65536;  // 64K units
        maxAddress = 0x200000000ULL;  // 8GB physical range
        currentPos = viewport.baseAddress;
    }
    
    // Vertical slider
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    // Need more room in VA mode for region info (but not too much!)
    float reservedHeight = useVirtualAddresses ? 150 : 80;
    float sliderHeight = std::max(50.0f, availSize.y - reservedHeight);  // Minimum height for slider
    
    // Convert address to vertical slider position (inverted - top is 0)
    uint64_t sliderValue = currentPos / sliderUnit;
    uint64_t maxSliderValue = maxAddress / sliderUnit;
    
    ImGui::PushItemWidth(-1);  // Full width
    
    // - button
    const char* buttonLabel = useVirtualAddresses ? "-Page" : "-64K";
    if (ImGui::Button(buttonLabel, ImVec2(-1, 30))) {
        if (viewport.baseAddress >= sliderUnit) {
            viewport.baseAddress -= sliderUnit;
            
            if (useVirtualAddresses && addressFlattener) {
                uint64_t virtualAddr = addressFlattener->FlatToVirtual(viewport.baseAddress);
                std::stringstream ss;
                ss << "0x" << std::hex << virtualAddr;
                strcpy(addressInput, ss.str().c_str());
            } else {
                std::stringstream ss;
                ss << "0x" << std::hex << viewport.baseAddress;
                strcpy(addressInput, ss.str().c_str());
            }
            needsUpdate = true;
        }
    }
    
    // Vertical slider
    uint64_t minSliderValue = 0;
    if (ImGui::VSliderScalar("##VAddr", ImVec2(180, sliderHeight), 
                            ImGuiDataType_U64, &sliderValue,
                            &maxSliderValue, &minSliderValue,  // Max at top, 0 at bottom
                            "0x%llx")) {
        viewport.baseAddress = sliderValue * sliderUnit;
        
        // Update address field
        if (useVirtualAddresses && addressFlattener) {
            // Show the VA that corresponds to this flat position
            uint64_t virtualAddr = addressFlattener->FlatToVirtual(viewport.baseAddress);
            std::stringstream ss;
            ss << "0x" << std::hex << virtualAddr;
            strcpy(addressInput, ss.str().c_str());
        } else {
            // Physical mode - show physical address
            std::stringstream ss;
            ss << "0x" << std::hex << viewport.baseAddress;
            strcpy(addressInput, ss.str().c_str());
        }
        
        needsUpdate = true;
    }
    
    // + button  
    const char* plusLabel = useVirtualAddresses ? "+Page" : "+64K";
    if (ImGui::Button(plusLabel, ImVec2(-1, 30))) {
        if (viewport.baseAddress + sliderUnit <= maxAddress) {
            viewport.baseAddress += sliderUnit;
            
            if (useVirtualAddresses && addressFlattener) {
                uint64_t virtualAddr = addressFlattener->FlatToVirtual(viewport.baseAddress);
                std::stringstream ss;
                ss << "0x" << std::hex << virtualAddr;
                strcpy(addressInput, ss.str().c_str());
            } else {
                std::stringstream ss;
                ss << "0x" << std::hex << viewport.baseAddress;
                strcpy(addressInput, ss.str().c_str());
            }
            needsUpdate = true;
        }
    }
    
    ImGui::PopItemWidth();
    
    // Current address display
    ImGui::Separator();
    ImGui::Text("Current:");
    ImGui::Text("0x%llx", viewport.baseAddress);
}

void MemoryVisualizer::DrawMemoryView() {
    // Create a scrollable child region
    ImVec2 availSize = ImGui::GetContentRegionAvail();
    
    // Reserve space for correlation stripe if enabled
    float correlationHeight = showCorrelation ? 100.0f : 0.0f;
    float memoryHeight = availSize.y - correlationHeight;
    
    // Add vertical scrollbar on the right - use memoryHeight not availSize.y!
    ImGui::BeginChild("MemoryScrollRegion", ImVec2(availSize.x, memoryHeight), false, 
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
    
    // The actual canvas size should be larger than viewport for scrolling
    float canvasHeight = std::max(availSize.y, (float)(viewport.height * viewport.zoom));
    float canvasWidth = std::max(availSize.x - 20, (float)(viewport.width * viewport.zoom)); // -20 for scrollbar
    
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(canvasPos, 
                            ImVec2(canvasPos.x + canvasWidth, canvasPos.y + canvasHeight),
                            IM_COL32(50, 50, 50, 255));
    
    if (memoryTexture && !pixelBuffer.empty()) {
        float texW = viewport.width * viewport.zoom;
        float texH = viewport.height * viewport.zoom;
        
        ImVec2 imgPos(canvasPos.x, canvasPos.y);
        ImVec2 imgSize(imgPos.x + texW, imgPos.y + texH);
        
        // Save position for magnifier
        memoryViewPos = imgPos;
        memoryViewSize = ImVec2(texW, texH);
        
        drawList->PushClipRect(canvasPos, 
                              ImVec2(canvasPos.x + canvasWidth, canvasPos.y + canvasHeight),
                              true);
        
        // Use current texture state (force refresh)
        glBindTexture(GL_TEXTURE_2D, memoryTexture);
        
        drawList->AddImage((ImTextureID)(intptr_t)memoryTexture,
                          imgPos, imgSize,
                          ImVec2(0, 0), ImVec2(1, 1),
                          IM_COL32(255, 255, 255, 255));
        
        // Draw marching ants around accumulated changed regions (if enabled)
        if (showChangeHighlight && !changeHistory.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<float>(now - lastChangeTime).count();
            
            // Clear old history after 3 seconds of no changes
            if (elapsed > 3.0f) {
                changeHistory.clear();
            } else {
                // Calculate fade based on time since last change
                // Show for 2 seconds, then fade for 1 second
                float alpha = 1.0f;
                if (elapsed > 2.0f) {
                    alpha = 1.0f - ((elapsed - 2.0f) / 1.0f);
                }
                
                // Draw yellow boxes around each individual region
                // Collect all regions from history and merge overlapping ones
                std::vector<ChangeRegion> allRegions;
                for (const auto& frame : changeHistory) {
                    for (const auto& region : frame) {
                        // Check if this region overlaps with existing ones and merge if so
                        bool merged = false;
                        for (auto& existing : allRegions) {
                            // Check for overlap or adjacency (within 2 pixels to avoid crowding)
                            int gap = 2;  // Minimum gap between separate boxes
                            bool closeY = abs(existing.y - region.y) <= gap && 
                                         abs((existing.y + existing.height) - (region.y + region.height)) <= gap;
                            bool closeX = abs(existing.x - region.x) <= gap && 
                                         abs((existing.x + existing.width) - (region.x + region.width)) <= gap;
                            
                            if (closeY && closeX) {
                                // Merge nearby regions to avoid visual clutter
                                int newX = std::min(existing.x, region.x);
                                int newY = std::min(existing.y, region.y);
                                int newRight = std::max(existing.x + existing.width, region.x + region.width);
                                int newBottom = std::max(existing.y + existing.height, region.y + region.height);
                                existing.x = newX;
                                existing.y = newY;
                                existing.width = newRight - newX;
                                existing.height = newBottom - newY;
                                merged = true;
                                break;
                            }
                        }
                        if (!merged) {
                            allRegions.push_back(region);
                        }
                    }
                }
                
                // Draw yellow highlight boxes around each region
                for (const auto& region : allRegions) {
                    // Draw box OUTSIDE the changed pixels with small margin
                    float margin = 1.0f;  // Single pixel margin to not cover content
                    float boxX = imgPos.x + (region.x * viewport.zoom) - margin;
                    float boxY = imgPos.y + (region.y * viewport.zoom) - margin;
                    float boxW = (region.width * viewport.zoom) + (2 * margin);
                    float boxH = (region.height * viewport.zoom) + (2 * margin);
                    
                    // Yellow color with current alpha
                    uint32_t color = IM_COL32(255, 255, 0, (uint8_t)(255 * alpha));
                    
                    // Draw rectangle outline
                    drawList->AddRect(
                        ImVec2(boxX, boxY),
                        ImVec2(boxX + boxW, boxY + boxH),
                        color,
                        0.0f,  // No rounding
                        0,     // All corners
                        2.0f   // 2 pixel thick border
                    );
                }  // End of for each region
            }
        }
        
        // Highlight search results
        if (searchActive && !searchResults.empty()) {
            for (uint64_t resultAddr : searchResults) {
                // Check if this result is within the current viewport
                if (resultAddr >= viewport.baseAddress && 
                    resultAddr < viewport.baseAddress + currentMemory.data.size()) {
                    
                    // Calculate position of this result in the viewport
                    size_t resultOffset = resultAddr - viewport.baseAddress;
                    int x = (resultOffset % viewport.stride) / viewport.format.bytesPerPixel;
                    int y = resultOffset / viewport.stride;
                    
                    // Only draw if visible
                    if (x >= 0 && x < viewport.width && y >= 0 && y < viewport.height) {
                        // Calculate pattern size
                    size_t patternSize = 0;
                    if (searchType == SEARCH_ASCII) {
                        patternSize = strlen(searchPattern);
                    } else {
                        // Hex pattern size
                        std::string hexStr = searchPattern;
                        hexStr.erase(std::remove(hexStr.begin(), hexStr.end(), ' '), hexStr.end());
                        patternSize = hexStr.length() / 2;
                    }
                    
                    // Calculate width in pixels
                    int pixelWidth = (patternSize + viewport.format.bytesPerPixel - 1) / viewport.format.bytesPerPixel;
                    
                    // Draw highlight box in cyan for search results
                    float boxX = imgPos.x + (x * viewport.zoom);
                    float boxY = imgPos.y + (y * viewport.zoom);
                    float boxW = pixelWidth * viewport.zoom;
                    float boxH = viewport.zoom;
                    
                        // Cyan color for search results, red for current result
                        bool isCurrent = (resultAddr == searchResults[currentSearchResult]);
                        uint32_t color = isCurrent ? 
                        IM_COL32(255, 0, 0, 200) :  // Red for current
                        IM_COL32(0, 255, 255, 150);  // Cyan for others
                    
                    drawList->AddRectFilled(
                        ImVec2(boxX, boxY),
                        ImVec2(boxX + boxW, boxY + boxH),
                        color
                    );
                    
                    // Add border for current result
                    if (isCurrent) {
                        drawList->AddRect(
                            ImVec2(boxX - 1, boxY - 1),
                            ImVec2(boxX + boxW + 1, boxY + boxH + 1),
                            IM_COL32(255, 255, 255, 255),
                            0.0f, 0, 2.0f
                        );
                    }
                    }  // End if visible
                }  // End if in viewport
            }  // End for each search result
        }
        
        drawList->PopClipRect();
    }
    
    // Make the invisible button the size of the content for scrolling
    ImGui::InvisibleButton("canvas", ImVec2(canvasWidth, canvasHeight));
    
    HandleInput();
    
    // Handle vertical scrollbar
    float scrollY = ImGui::GetScrollY();
    float maxScrollY = ImGui::GetScrollMaxY();
    if (maxScrollY > 0) {
        // Map scroll position to memory address
        float scrollRatio = scrollY / maxScrollY;
        int64_t addressRange = 0x100000; // 1MB view range for now
        int64_t scrollOffset = (int64_t)(scrollRatio * addressRange);
        
        // This will be used to offset the memory view
        // For now just track it, actual implementation would update base address
    }
    
    ImGui::EndChild();
    
    // Draw correlation stripe at bottom if enabled
    if (showCorrelation && !currentMemory.data.empty()) {
        DrawCorrelationStripe();
    }
    
    // Draw magnifier window if enabled
    if (showMagnifier) {
        DrawMagnifier();
    }
    
    // Note: Search is now integrated into magnifier window
}

void MemoryVisualizer::DrawMagnifier() {
    // Create a floating window for the magnifier - resizable
    // Always use NoNavInputs to prevent arrow key focus issues
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavInputs;
    
    static bool bringToFront = false;
    
    // Check for 'M' key or Ctrl+F to bring magnifier to front (only when not typing)
    if (!ImGui::GetIO().WantTextInput && 
        (ImGui::IsKeyPressed(ImGuiKey_M, false) || 
         (ImGui::IsKeyPressed(ImGuiKey_F, false) && (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl)))) {
        bringToFront = true;
        if (!showMagnifier && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            showMagnifier = true;  // Open if closed when using Ctrl+F
        }
    }
    
    // Set initial size but allow resizing (wider to accommodate hex display)
    ImGui::SetNextWindowSize(ImVec2(550, 500), ImGuiCond_FirstUseEver);
    
    // Bring to front when requested
    if (bringToFront) {
        ImGui::SetNextWindowFocus();
        bringToFront = false;
    }
    
    if (!ImGui::Begin("Magnifier", &showMagnifier, windowFlags)) {
        ImGui::End();
        return;
    }
    
    // Compact search bar at top
    ImGui::PushItemWidth(100);
    if (ImGui::RadioButton("ASCII", searchType == SEARCH_ASCII)) searchType = SEARCH_ASCII;
    ImGui::SameLine();
    if (ImGui::RadioButton("Hex", searchType == SEARCH_HEX)) searchType = SEARCH_HEX;
    ImGui::SameLine();
    ImGui::Checkbox("Full", &searchFullRange);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Search entire memory space (slower but comprehensive)\n"
                         "Requires QEMU memory-backend-file");
    }
    ImGui::SameLine();
    if (!searchFullRange) {
        ImGui::Checkbox("From here", &searchFromCurrent);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Search from current position instead of beginning\n"
                             "Note: Viewport search only covers %zu KB",
                             (size_t)(viewport.stride * viewport.height) / 1024);
        }
        ImGui::SameLine();
    }
    
    ImGui::PushItemWidth(150);
    bool searchEntered = false;
    if (searchType == SEARCH_ASCII) {
        searchEntered = ImGui::InputText("##search", searchPattern, sizeof(searchPattern), 
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    } else {
        searchEntered = ImGui::InputText("##search", searchPattern, sizeof(searchPattern),
                                         ImGuiInputTextFlags_CharsHexadecimal | 
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    }
    ImGui::PopItemWidth();
    
    ImGui::SameLine();
    static bool userNavigated = false;  // Declare here so it's accessible
    if (ImGui::Button("Find") || searchEntered) {
        if (searchFullRange) {
            PerformFullRangeSearch();
        } else {
            PerformSearch();
        }
        if (!searchResults.empty()) {
            magnifierLocked = true;  // Lock onto first result
            userNavigated = false;  // Reset navigation flag for new search
        }
    }
    
    // Search results navigation on same line
    if (searchActive && !searchResults.empty()) {
        ImGui::SameLine();
        if (searchResults.size() >= 1000) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "[%zu/%zu+]", 
                              currentSearchResult + 1, searchResults.size());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Limited to first 1000 results");
            }
        } else {
            ImGui::Text("[%zu/%zu]", currentSearchResult + 1, searchResults.size());
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) {
            FindPrevious();
            magnifierLocked = true;  // Lock onto search result
            userNavigated = false;  // Reset to show the new result
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##next", ImGuiDir_Right)) {
            FindNext();
            magnifierLocked = true;  // Lock onto search result
            userNavigated = false;  // Reset to show the new result
        }
    } else if (searchActive) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "Not found");
    }
    
    // Check if clicking outside window to unlock from search result
    // Only unlock if we're actively clicking outside, not just unfocused
    if (magnifierLocked && !ImGui::IsWindowFocused() && !ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
        // Make sure we're not clicking on any ImGui window
        if (!ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive()) {
            std::cerr << "UNLOCKING magnifier due to outside click" << std::endl;
            magnifierLocked = false;
        }
    }
    
    ImGui::Separator();
    
    // Calculate magnified area size first to know source pixels
    ImVec2 contentRegion = ImGui::GetContentRegionAvail();
    float magnifiedAreaHeight = contentRegion.y - 250; // Leave space for hex display
    float magnifiedAreaWidth = contentRegion.x - 10;
    magnifiedAreaHeight = std::max(magnifiedAreaHeight, 100.0f); // Minimum height
    magnifiedAreaWidth = std::max(magnifiedAreaWidth, 100.0f); // Minimum width
    
    // Calculate how many source pixels we can show
    int sourcePixelsX = magnifiedAreaWidth / magnifierZoom;
    int sourcePixelsY = magnifiedAreaHeight / magnifierZoom;
    
    // Zoom control as a combo/popup
    ImGui::Text("Zoom:");
    ImGui::SameLine();
    
    char zoomLabel[16];
    snprintf(zoomLabel, sizeof(zoomLabel), "%dx", magnifierZoom);
    
    if (ImGui::BeginCombo("##zoom", zoomLabel, ImGuiComboFlags_HeightLarge)) {
        int zoomLevels[] = {2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 20, 24, 32, 48, 64};
        for (int i = 0; i < sizeof(zoomLevels)/sizeof(zoomLevels[0]); i++) {
            bool isSelected = (magnifierZoom == zoomLevels[i]);
            char itemLabel[16];
            snprintf(itemLabel, sizeof(itemLabel), "%dx", zoomLevels[i]);
            
            if (ImGui::Selectable(itemLabel, isSelected)) {
                magnifierZoom = zoomLevels[i];
            }
            
            // Set the initial focus when opening the combo
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    
    ImGui::SameLine();
    ImGui::Text("Viewing %dx%d pixels", sourcePixelsX, sourcePixelsY);
    
    ImGui::SameLine();
    if (ImGui::Checkbox("Lock", &magnifierLocked)) {
        if (magnifierLocked) {
            // When locking, save current position
            ImVec2 mousePos = ImGui::GetMousePos();
            std::cerr << "Mouse: (" << mousePos.x << ", " << mousePos.y << "), MemView: (" 
                     << memoryViewPos.x << ", " << memoryViewPos.y << "), Zoom: " << viewport.zoom << std::endl;
            magnifierLockPos.x = (mousePos.x - memoryViewPos.x) / viewport.zoom;
            magnifierLockPos.y = (mousePos.y - memoryViewPos.y) / viewport.zoom;
            std::cerr << "LOCKING at position: (" << magnifierLockPos.x << ", " << magnifierLockPos.y << ")" << std::endl;
            // No clamping - allow any position
        } else {
            std::cerr << "UNLOCKING via checkbox" << std::endl;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lock position and use WASD or Arrow keys to navigate");
    }
    
    // Track if user has manually navigated away from search result
    // (userNavigated is declared above near the Find button)
    
    // Handle WASD keys for manual navigation when magnifier is locked
    if (magnifierLocked) {
        ImGuiIO& io = ImGui::GetIO();
        bool shiftPressed = io.KeyShift;
        int stepSize = shiftPressed ? 10 : 1;  // Shift for faster scrolling
        
        bool moved = false;
        
        // WASD and Arrow navigation with key repeat
        bool leftKey = ImGui::IsKeyPressed(ImGuiKey_A, true) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
        bool rightKey = ImGui::IsKeyPressed(ImGuiKey_D, true) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
        bool upKey = ImGui::IsKeyPressed(ImGuiKey_W, true) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
        bool downKey = ImGui::IsKeyPressed(ImGuiKey_S, true) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
        
        float oldX = magnifierLockPos.x;
        float oldY = magnifierLockPos.y;
        
        if (leftKey) {
            magnifierLockPos.x -= stepSize;
            moved = true;
        }
        if (rightKey) {
            magnifierLockPos.x += stepSize;
            moved = true;
        }
        if (upKey) {
            magnifierLockPos.y -= stepSize;
            moved = true;
        }
        if (downKey) {
            magnifierLockPos.y += stepSize;
            moved = true;
        }
        
        if (moved) {
            userNavigated = true;  // Mark that user has manually navigated
        }
        
        // Don't clamp - allow navigation anywhere
        // We'll draw black for out-of-bounds pixels instead
    }
    
    // Determine what to magnify - search result or mouse position
    int srcX, srcY;
    
    // Only show search result if user hasn't manually navigated away
    if (magnifierLocked && searchActive && !searchResults.empty() && currentSearchResult < searchResults.size() && !userNavigated) {
        // Show the current search result
        uint64_t resultAddr = searchResults[currentSearchResult];
        
        // Calculate the offset of this result within the current memory buffer
        if (resultAddr >= viewport.baseAddress && 
            resultAddr < viewport.baseAddress + currentMemory.data.size()) {
            
            // Result is within current view
            size_t resultOffset = resultAddr - viewport.baseAddress;
            
            // For search results, we want to center on the BYTE position, not pixel position
            // Calculate the exact byte position in the viewport
            int byteX = resultOffset % viewport.stride;
            int byteY = resultOffset / viewport.stride;
            
            // Convert byte position to pixel position
            // This centers on the start of the found pattern
            srcX = byteX / viewport.format.bytesPerPixel;
            srcY = byteY;
            
            // Also update magnifierLockPos to match the search result position
            // This ensures arrow keys start from the correct position
            magnifierLockPos.x = srcX;
            magnifierLockPos.y = srcY;
            
            // Add indicator that we're showing search result
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Showing search result %zu of %zu", 
                              currentSearchResult + 1, searchResults.size());
            ImGui::Text("Address: 0x%llx", resultAddr);
            ImGui::Text("Pattern starts at byte offset %d in line", byteX);
        } else {
            // Result is not in current view - should navigate to it
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Result not in current view");
            ImGui::Text("Press navigation arrows to jump to result");
            
            // Fall back to mouse position
            ImVec2 mousePos = ImGui::GetMousePos();
            srcX = (mousePos.x - memoryViewPos.x) / viewport.zoom;
            srcY = (mousePos.y - memoryViewPos.y) / viewport.zoom;
        }
        
        ImGui::Text("Click outside window to unlock and follow mouse");
    } else if (magnifierLocked) {
        // Use locked position (can be adjusted with WASD keys)
        srcX = magnifierLockPos.x;
        srcY = magnifierLockPos.y;
        if (srcX != (int)magnifierLockPos.x || srcY != (int)magnifierLockPos.y) {
            std::cerr << "POSITION CONVERSION: float(" << magnifierLockPos.x << ", " << magnifierLockPos.y 
                     << ") -> int(" << srcX << ", " << srcY << ")" << std::endl;
        }
        ImGui::Text("Locked at (%d, %d) - use WASD or Arrows", srcX, srcY);
        ImGui::TextDisabled("Hold Shift for 10x speed");
    } else {
        // Follow mouse position
        ImVec2 mousePos = ImGui::GetMousePos();
        srcX = (mousePos.x - memoryViewPos.x) / viewport.zoom;
        srcY = (mousePos.y - memoryViewPos.y) / viewport.zoom;
        
        if (searchActive && !searchResults.empty()) {
            ImGui::TextDisabled("Following mouse (navigate to result to lock)");
        }
    }
    
    // Half sizes for centering
    int halfSizeX = sourcePixelsX / 2;
    int halfSizeY = sourcePixelsY / 2;
    
    // Draw the magnified view
    ImVec2 drawPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(drawPos, 
                            ImVec2(drawPos.x + magnifiedAreaWidth, drawPos.y + magnifiedAreaHeight),
                            IM_COL32(30, 30, 30, 255));
    
    // Calculate pattern bounds if showing search result
    int patternStartX = -1, patternEndX = -1;
    if (magnifierLocked && searchActive && !searchResults.empty()) {
        size_t patternSize = 0;
        if (searchType == SEARCH_ASCII) {
            patternSize = strlen(searchPattern);
        } else {
            std::string hexStr = searchPattern;
            hexStr.erase(std::remove(hexStr.begin(), hexStr.end(), ' '), hexStr.end());
            patternSize = hexStr.length() / 2;
        }
        
        // Calculate start and end positions of pattern
        patternStartX = 0;  // Pattern starts at center
        patternEndX = (patternSize + viewport.format.bytesPerPixel - 1) / viewport.format.bytesPerPixel;
    }
    
    // Log if we're drawing from negative position
    static int lastLoggedSrcX = 999999;
    static int lastLoggedSrcY = 999999;
    if (srcX != lastLoggedSrcX || srcY != lastLoggedSrcY) {
        if (srcX < 0 || srcY < 0) {
            std::cerr << "DRAWING from negative src: (" << srcX << ", " << srcY << ")" << std::endl;
        }
        lastLoggedSrcX = srcX;
        lastLoggedSrcY = srcY;
    }
    
    // Draw each pixel magnified
    for (int dy = -halfSizeY; dy < halfSizeY; dy++) {
        for (int dx = -halfSizeX; dx < halfSizeX; dx++) {
            int px = srcX + dx;
            int py = srcY + dy;
            
            // Calculate draw position for this pixel
            float pixelSizeX = magnifiedAreaWidth / (halfSizeX * 2);
            float pixelSizeY = magnifiedAreaHeight / (halfSizeY * 2);
            float x1 = drawPos.x + (dx + halfSizeX) * pixelSizeX;
            float y1 = drawPos.y + (dy + halfSizeY) * pixelSizeY;
            float x2 = x1 + pixelSizeX;
            float y2 = y1 + pixelSizeY;
            
            // Get pixel color - black if out of bounds
            uint32_t pixel = IM_COL32(0, 0, 0, 255);  // Default to black
            bool inBounds = (px >= 0 && px < viewport.width && py >= 0 && py < viewport.height);
            
            if (inBounds) {
                pixel = GetPixelAt(px, py);
            }
            
            // Draw the pixel
            drawList->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), pixel);
            
            // Only highlight pattern and draw grid for in-bounds pixels
            if (inBounds) {
                // Highlight the found pattern
                bool isPartOfPattern = magnifierLocked && dy == 0 && dx >= 0 && dx < patternEndX;
                if (isPartOfPattern) {
                    // Draw red border around pattern pixels
                    drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), 
                                     IM_COL32(255, 0, 0, 255), 0.0f, 0, 2.0f);
                }
                
                // Draw grid lines for zoom >= 4x or when pixels are large enough
                float minPixelSize = std::min(pixelSizeX, pixelSizeY);
                if (magnifierZoom >= 4 || minPixelSize >= 4) {
                    uint32_t gridColor = isPartOfPattern ? 
                        IM_COL32(200, 100, 100, 150) :  // Reddish grid for pattern
                        IM_COL32(100, 100, 100, 100);   // Normal grid
                    drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), gridColor, 0.0f, 0, 1.0f);
                }
            } else {
                // Draw subtle grid for out-of-bounds areas
                if (magnifierZoom >= 4) {
                    drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), 
                                     IM_COL32(40, 40, 40, 100), 0.0f, 0, 1.0f);
                }
            }
        }
    }
    
    // Draw center crosshair
    uint32_t crosshairColor = IM_COL32(255, 255, 0, 200);
    float centerX = drawPos.x + magnifiedAreaWidth / 2;
    float centerY = drawPos.y + magnifiedAreaHeight / 2;
    drawList->AddLine(ImVec2(centerX - 10, centerY), ImVec2(centerX + 10, centerY), crosshairColor);
    drawList->AddLine(ImVec2(centerX, centerY - 10), ImVec2(centerX, centerY + 10), crosshairColor);
    
    // Make space for the drawn content
    ImGui::Dummy(ImVec2(magnifiedAreaWidth, magnifiedAreaHeight));
    
    // Show info about center pixel and surrounding hex data
    uint64_t addr = GetAddressAt(srcX, srcY);
    ImGui::Text("Center: (%d, %d)", srcX, srcY);
    ImGui::SameLine(150);
    ImGui::Text("Address: 0x%llx", addr);
    
    // Show hex data for three lines (above, current, below)
    ImGui::Separator();
    ImGui::Text("Hex Data:");
    
    if (!currentMemory.data.empty()) {
        // Display 16 bytes per line, centered on current position
        const int bytesToShow = 16;
        const int halfBytes = bytesToShow / 2;
        
        // Calculate the actual byte position we're looking at
        int centerByteX;
        if (magnifierLocked && searchActive && !searchResults.empty() && currentSearchResult < searchResults.size() && !userNavigated) {
            // For search results (when not manually navigated), use the exact byte position
            size_t resultOffset = searchResults[currentSearchResult];
            centerByteX = resultOffset % viewport.stride;
        } else {
            // For mouse position or manual navigation, use pixel-based calculation
            centerByteX = srcX * viewport.format.bytesPerPixel;
        }
        
        for (int lineOffset = -1; lineOffset <= 1; lineOffset++) {
            int y = srcY + lineOffset;
            if (y >= 0 && y < viewport.height) {
                size_t lineStart = y * viewport.stride + std::max(0, centerByteX - halfBytes);
                
                // Line label
                if (lineOffset == -1) ImGui::Text("Above: ");
                else if (lineOffset == 0) ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Line:  ");
                else ImGui::Text("Below: ");
                
                ImGui::SameLine();
                
                // Display hex bytes
                for (int b = 0; b < bytesToShow; b++) {
                    size_t byteOffset = lineStart + b;
                    if (byteOffset < currentMemory.data.size()) {
                        // Highlight the pattern bytes for search results or center position
                        bool isCenter = false;
                        if (magnifierLocked && searchActive && !searchResults.empty() && lineOffset == 0 && !userNavigated) {
                            // For search results (when not manually navigated), highlight the actual pattern
                            uint64_t resultAddr = searchResults[currentSearchResult];
                            size_t patternLen = (searchType == SEARCH_ASCII) ? 
                                strlen(searchPattern) : 
                                (strlen(searchPattern) + 1) / 2;  // Hex pattern length
                            
                            // Check if this byte is part of the pattern
                            // byteOffset is relative to current viewport, need to convert to absolute
                            uint64_t absoluteByteAddr = viewport.baseAddress + byteOffset;
                            isCenter = (absoluteByteAddr >= resultAddr && absoluteByteAddr < resultAddr + patternLen);
                        } else if (lineOffset == 0) {
                            // For manual navigation or mouse position, highlight the center bytes
                            isCenter = (b >= halfBytes && b < halfBytes + viewport.format.bytesPerPixel);
                        }
                        
                        if (isCenter) {
                            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%02X", 
                                             currentMemory.data[byteOffset]);
                        } else {
                            ImGui::Text("%02X", currentMemory.data[byteOffset]);
                        }
                        
                        if (b < bytesToShow - 1) {
                            ImGui::SameLine(0, 2);  // Small spacing between bytes
                        }
                        
                        // Add extra space after 8 bytes for readability
                        if (b == 7) {
                            ImGui::SameLine(0, 8);
                        }
                    }
                }
                
                // ASCII representation
                ImGui::SameLine(400);
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                
                for (int b = 0; b < bytesToShow; b++) {
                    size_t byteOffset = lineStart + b;
                    if (byteOffset < currentMemory.data.size()) {
                        uint8_t ch = currentMemory.data[byteOffset];
                        bool isCenter = false;
                        if (magnifierLocked && searchActive && !searchResults.empty() && lineOffset == 0) {
                            // For search results, highlight the actual pattern
                            uint64_t resultAddr = searchResults[currentSearchResult];
                            size_t patternLen = (searchType == SEARCH_ASCII) ? 
                                strlen(searchPattern) : 
                                (strlen(searchPattern) + 1) / 2;
                            // byteOffset is relative to current viewport, need to convert to absolute
                            uint64_t absoluteByteAddr = viewport.baseAddress + byteOffset;
                            isCenter = (absoluteByteAddr >= resultAddr && absoluteByteAddr < resultAddr + patternLen);
                        } else if (!magnifierLocked && lineOffset == 0) {
                            // For mouse position, highlight the pixel's bytes
                            isCenter = (b >= halfBytes && b < halfBytes + viewport.format.bytesPerPixel);
                        }
                        
                        if (ch >= 32 && ch < 127) {
                            if (isCenter) {
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%c", ch);
                            } else {
                                ImGui::Text("%c", ch);
                            }
                        } else {
                            ImGui::TextDisabled(".");
                        }
                        
                        if (b < bytesToShow - 1) {
                            ImGui::SameLine(0, 0);  // No spacing for ASCII
                        }
                    }
                }
            }
        }
    }
    
    ImGui::Separator();
    
    // Get the pixel value at center (keep existing format-specific display)
    if (!currentMemory.data.empty()) {
        size_t offset = srcY * viewport.stride + srcX * viewport.format.bytesPerPixel;
        if (offset < currentMemory.data.size()) {
            switch (viewport.format.type) {
                case PixelFormat::RGB888:
                    if (offset + 2 < currentMemory.data.size()) {
                        ImGui::Text("RGB: %02X %02X %02X", 
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2]);
                    }
                    break;
                case PixelFormat::RGBA8888:
                    if (offset + 3 < currentMemory.data.size()) {
                        ImGui::Text("RGBA: %02X %02X %02X %02X",
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2],
                                   currentMemory.data[offset + 3]);
                    }
                    break;
                case PixelFormat::BGR888:
                    if (offset + 2 < currentMemory.data.size()) {
                        ImGui::Text("BGR: %02X %02X %02X", 
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2]);
                    }
                    break;
                case PixelFormat::BGRA8888:
                    if (offset + 3 < currentMemory.data.size()) {
                        ImGui::Text("BGRA: %02X %02X %02X %02X",
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2],
                                   currentMemory.data[offset + 3]);
                    }
                    break;
                case PixelFormat::ARGB8888:
                    if (offset + 3 < currentMemory.data.size()) {
                        ImGui::Text("ARGB: %02X %02X %02X %02X",
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2],
                                   currentMemory.data[offset + 3]);
                    }
                    break;
                case PixelFormat::ABGR8888:
                    if (offset + 3 < currentMemory.data.size()) {
                        ImGui::Text("ABGR: %02X %02X %02X %02X",
                                   currentMemory.data[offset],
                                   currentMemory.data[offset + 1],
                                   currentMemory.data[offset + 2],
                                   currentMemory.data[offset + 3]);
                    }
                    break;
                case PixelFormat::RGB565:
                    if (offset + 1 < currentMemory.data.size()) {
                        uint16_t val = (currentMemory.data[offset] << 8) | currentMemory.data[offset + 1];
                        ImGui::Text("RGB565: 0x%04X", val);
                    }
                    break;
                default:
                    ImGui::Text("Value: 0x%02X", currentMemory.data[offset]);
                    break;
            }
        }
    }
    
    // Add shortcut hints at bottom
    ImGui::Separator();
    ImGui::TextDisabled("Tips: M = bring to front, Ctrl+F = search, F3/Shift+F3 = next/prev");
    
    ImGui::End();
}

void MemoryVisualizer::DrawCorrelationStripe() {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(ImGui::GetContentRegionAvail().x, 100);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                            IM_COL32(20, 20, 20, 255));
    
    // Compute correlation if we have memory
    if (!currentMemory.data.empty()) {
        auto correlation = correlator.Correlate(currentMemory.data.data(), 
                                               currentMemory.data.size(), 
                                               pixelFormatIndex);
        
        if (!correlation.empty()) {
            // Draw correlation graph
            float xScale = size.x / std::min((size_t)2048, correlation.size());
            float yScale = size.y * 0.8f;  // Use 80% of height
            float baseline = pos.y + size.y - 10;
            
            // Draw grid lines
            for (int x = 64; x < 2048; x += 64) {
                float xPos = pos.x + x * xScale;
                drawList->AddLine(ImVec2(xPos, pos.y), 
                                 ImVec2(xPos, pos.y + size.y),
                                 IM_COL32(40, 40, 40, 255));
                
                // Label major widths
                if (x % 256 == 0) {
                    char label[32];
                    snprintf(label, sizeof(label), "%d", x);
                    drawList->AddText(ImVec2(xPos - 10, pos.y + size.y - 8),
                                     IM_COL32(128, 128, 128, 255), label);
                }
            }
            
            // Draw correlation curve
            ImVec2 prevPoint(pos.x, baseline);
            for (size_t i = 0; i < std::min((size_t)2048, correlation.size()); i++) {
                float x = pos.x + i * xScale;
                float y = baseline - correlation[i] * yScale;
                
                ImVec2 curPoint(x, y);
                drawList->AddLine(prevPoint, curPoint, IM_COL32(0, 255, 128, 255), 1.5f);
                prevPoint = curPoint;
            }
            
            // Find and mark peaks
            auto peaks = correlator.FindPeaks(correlation, 0.3);
            for (int peak : peaks) {
                float x = pos.x + peak * xScale;
                drawList->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y),
                                 IM_COL32(255, 255, 0, 128), 2.0f);
                
                // Label the peak
                char label[32];
                snprintf(label, sizeof(label), "%d", peak);
                drawList->AddText(ImVec2(x + 2, pos.y + 2),
                                 IM_COL32(255, 255, 0, 255), label);
            }
        }
    }
    
    // Label
    drawList->AddText(ImVec2(pos.x + 5, pos.y + 5),
                     IM_COL32(200, 200, 200, 255), "Autocorrelation (Width Detection)");
}

void MemoryVisualizer::HandleInput() {
    ImGuiIO& io = ImGui::GetIO();
    
    // Check for 'm' key to lock magnifier on current mouse position
    if (ImGui::IsKeyPressed(ImGuiKey_M) && !io.KeyShift && !io.KeyCtrl && !io.KeyAlt) {
        if (ImGui::IsItemHovered()) {
            // Get current mouse position relative to memory view
            ImVec2 mousePos = ImGui::GetMousePos();
            magnifierLockPos.x = (mousePos.x - memoryViewPos.x) / viewport.zoom;
            magnifierLockPos.y = (mousePos.y - memoryViewPos.y) / viewport.zoom;
            
            // No clamping - allow any position
            
            // Enable magnifier and lock it
            showMagnifier = true;
            magnifierLocked = true;
        }
    }
    
    if (ImGui::IsItemHovered()) {
        // Mouse wheel scrolls through memory addresses
        if (io.MouseWheel != 0) {
            // Scroll by rows worth of memory
            int64_t scrollDelta = io.MouseWheel * viewport.stride * 16;  // 16 rows at a time
            
            // Shift for faster scrolling (64K chunks)
            if (io.KeyShift) {
                scrollDelta = io.MouseWheel * 65536;
            }
            
            // Update address - natural scrolling: wheel up = go up (earlier/lower addresses)
            int64_t newAddress = (int64_t)viewport.baseAddress - scrollDelta;  // Natural scrolling
            if (newAddress < 0) {
                std::cerr << "VIEWPORT CLAMPED TO 0 (was trying " << newAddress << ")" << std::endl;
                newAddress = 0;
            }
            if (newAddress > 0x200000000ULL) newAddress = 0x200000000ULL;  // Cap at 8GB
            
            viewport.baseAddress = (uint64_t)newAddress;
            
            // Update the address input field
            std::stringstream ss;
            ss << "0x" << std::hex << viewport.baseAddress;
            strcpy(addressInput, ss.str().c_str());
            
            needsUpdate = true;
        }
        
        // Drag to scroll through memory - "mouse sticks to paper" in both X and Y
        if (ImGui::IsMouseDragging(0)) {
            ImVec2 delta = ImGui::GetMouseDragDelta(0);
            
            if (!isDragging) {
                // Start of drag
                isDragging = true;
                dragStartX = 0;
                dragStartY = 0;
            }
            
            // Calculate memory offset from drag
            float dragDeltaX = delta.x - dragStartX;
            float dragDeltaY = delta.y - dragStartY;
            
            // Vertical: Each pixel of drag = one row of memory
            // Horizontal: Each pixel of drag = one byte (or pixel format size)
            int64_t verticalDelta = (int64_t)dragDeltaY * viewport.stride;
            int64_t horizontalDelta = (int64_t)dragDeltaX * viewport.format.bytesPerPixel;
            
            int64_t totalDelta = verticalDelta + horizontalDelta;
            
            if (totalDelta != 0) {
                int64_t newAddress = (int64_t)viewport.baseAddress - totalDelta;
                if (newAddress < 0) newAddress = 0;
                if (newAddress > 0x200000000ULL) newAddress = 0x200000000ULL;
                
                viewport.baseAddress = (uint64_t)newAddress;
                
                // Update the address input field
                std::stringstream ss;
                ss << "0x" << std::hex << viewport.baseAddress;
                strcpy(addressInput, ss.str().c_str());
                
                dragStartX = delta.x;  // Reset drag start for continuous scrolling
                dragStartY = delta.y;
                needsUpdate = true;
            }
        } else {
            isDragging = false;
        }
        
        if (ImGui::IsMouseClicked(0)) {
            isDragging = true;
            dragStartX = io.MousePos.x - viewport.panX;
            dragStartY = io.MousePos.y - viewport.panY;
        }
    }
    
    if (isDragging) {
        if (ImGui::IsMouseDragging(0)) {
            viewport.panX = io.MousePos.x - dragStartX;
            viewport.panY = io.MousePos.y - dragStartY;
        }
        
        if (ImGui::IsMouseReleased(0)) {
            isDragging = false;
        }
    }
}

void MemoryVisualizer::UpdateTexture() {
    auto newPixels = ConvertMemoryToPixels(currentMemory);
    
    if (!newPixels.empty()) {
        // Check if pixels actually changed
        static uint32_t lastPixelChecksum = 0;
        uint32_t checksum = 0;
        for (size_t i = 0; i < newPixels.size(); i++) {
            checksum ^= newPixels[i];
        }
        
        // if (checksum != lastPixelChecksum && lastPixelChecksum != 0) {
        //     std::cerr << "PIXELS CHANGED! Checksum: 0x" << std::hex << checksum << std::dec << "\n";
        // }
        lastPixelChecksum = checksum;
        
        pixelBuffer = std::move(newPixels);
        
        glBindTexture(GL_TEXTURE_2D, memoryTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                    viewport.width, viewport.height,
                    0, GL_RGBA, GL_UNSIGNED_BYTE, pixelBuffer.data());
        glFlush();  // Force GPU to process the texture update
    }
}

std::vector<uint32_t> MemoryVisualizer::ConvertMemoryToPixels(const MemoryBlock& memory) {
    std::vector<uint32_t> pixels(viewport.width * viewport.height, 0xFF000000);
    
    if (memory.data.empty()) {
        return pixels;
    }
    
    size_t dataIndex = 0;
    
    for (size_t y = 0; y < viewport.height; ++y) {
        for (size_t x = 0; x < viewport.width; ++x) {
            size_t pixelIndex = y * viewport.width + x;
            size_t memIndex = y * viewport.stride + x * viewport.format.bytesPerPixel;
            
            if (memIndex >= memory.data.size()) {
                continue;
            }
            
            switch (viewport.format.type) {
                case PixelFormat::RGB888:
                    if (memIndex + 2 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex],
                            memory.data[memIndex + 1],
                            memory.data[memIndex + 2]
                        );
                    }
                    break;
                    
                case PixelFormat::RGBA8888:
                    if (memIndex + 3 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex],
                            memory.data[memIndex + 1],
                            memory.data[memIndex + 2],
                            memory.data[memIndex + 3]
                        );
                    }
                    break;
                    
                case PixelFormat::BGR888:
                    if (memIndex + 2 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex + 2],  // B -> R
                            memory.data[memIndex + 1],  // G -> G
                            memory.data[memIndex]        // R -> B
                        );
                    }
                    break;
                    
                case PixelFormat::BGRA8888:
                    if (memIndex + 3 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex + 2],  // B -> R
                            memory.data[memIndex + 1],  // G -> G
                            memory.data[memIndex],      // R -> B
                            memory.data[memIndex + 3]   // A -> A
                        );
                    }
                    break;
                    
                case PixelFormat::ARGB8888:
                    if (memIndex + 3 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex + 1],  // A R G B -> R
                            memory.data[memIndex + 2],  // G
                            memory.data[memIndex + 3],  // B
                            memory.data[memIndex]        // A
                        );
                    }
                    break;
                    
                case PixelFormat::ABGR8888:
                    if (memIndex + 3 < memory.data.size()) {
                        pixels[pixelIndex] = PackRGBA(
                            memory.data[memIndex + 3],  // A B G R -> R
                            memory.data[memIndex + 2],  // G
                            memory.data[memIndex + 1],  // B
                            memory.data[memIndex]        // A
                        );
                    }
                    break;
                    
                case PixelFormat::GRAYSCALE:
                    if (memIndex < memory.data.size()) {
                        uint8_t gray = memory.data[memIndex];
                        pixels[pixelIndex] = PackRGBA(gray, gray, gray);
                    }
                    break;
                    
                case PixelFormat::RGB565:
                    if (memIndex + 1 < memory.data.size()) {
                        uint16_t rgb565 = (memory.data[memIndex] << 8) | memory.data[memIndex + 1];
                        uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;
                        uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;
                        uint8_t b = (rgb565 & 0x1F) << 3;
                        pixels[pixelIndex] = PackRGBA(r, g, b);
                    }
                    break;
                    
                case PixelFormat::BINARY:
                    if (memIndex / 8 < memory.data.size()) {
                        uint8_t byte = memory.data[memIndex / 8];
                        uint8_t bit = (byte >> (7 - (memIndex % 8))) & 1;
                        uint8_t value = bit ? 255 : 0;
                        pixels[pixelIndex] = PackRGBA(value, value, value);
                    }
                    break;
                    
                case PixelFormat::CUSTOM:
                    // Custom format - just use grayscale for now
                    // TODO: Implement custom format handler
                    if (memIndex < memory.data.size()) {
                        uint8_t gray = memory.data[memIndex];
                        pixels[pixelIndex] = PackRGBA(gray, gray, gray);
                    }
                    break;
            }
        }
    }
    
    return pixels;
}

uint32_t MemoryVisualizer::GetPixelAt(int x, int y) const {
    if (x < 0 || x >= viewport.width || y < 0 || y >= viewport.height) {
        return 0;
    }
    
    size_t index = y * viewport.width + x;
    if (index < pixelBuffer.size()) {
        return pixelBuffer[index];
    }
    
    return 0;
}

uint64_t MemoryVisualizer::GetAddressAt(int x, int y) const {
    size_t offset = y * viewport.stride + x * viewport.format.bytesPerPixel;
    return viewport.baseAddress + offset;
}

void MemoryVisualizer::NavigateToAddress(uint64_t address) {
    std::cerr << "NAVIGATE TO ADDRESS: 0x" << std::hex << address << std::dec 
              << " (from 0x" << std::hex << viewport.baseAddress << std::dec << ")" << std::endl;
    // If using virtual addresses with crunched view
    if (useVirtualAddresses && addressFlattener && targetPid > 0) {
        // Convert VA to position in flattened space
        uint64_t flatPos = addressFlattener->VirtualToFlat(address);
        
        // Store the flat position as our "base address" for the viewport
        viewport.baseAddress = flatPos;
        
        // Display the actual VA in the address field
        std::stringstream ss;
        ss << "0x" << std::hex << address;
        strcpy(addressInput, ss.str().c_str());
        
        std::cerr << "VA 0x" << std::hex << address 
                  << " -> Flat position 0x" << flatPos << std::dec 
                  << " (" << (flatPos / (1024*1024)) << " MB into crunched space)" << std::endl;
        
        // Prefetch translations for this area
        if (viewportTranslator) {
            size_t viewSize = viewport.width * viewport.height * 4;
            viewportTranslator->SetViewport(targetPid, address, viewSize);
        }
    } else {
        // Original physical address mode
        viewport.baseAddress = address;
        std::stringstream ss;
        ss << "0x" << std::hex << address;
        strcpy(addressInput, ss.str().c_str());
    }
    
    needsUpdate = true;
}

void MemoryVisualizer::SetViewport(const ViewportSettings& settings) {
    viewport = settings;
    widthInput = settings.width;
    heightInput = settings.height;
    strideInput = settings.stride;
    pixelFormatIndex = settings.format.type;
    needsUpdate = true;
}

void MemoryVisualizer::LoadMemoryMap(const std::vector<GuestMemoryRegion>& regions) {
    if (addressFlattener) {
        addressFlattener->BuildFromRegions(regions);
        std::cerr << "Loaded memory map with " << regions.size() << " regions\n";
    }
}

void MemoryVisualizer::DrawNavigator() {
    if (useVirtualAddresses && crunchedNavigator) {
        // Use compressed navigator for virtual addresses
        crunchedNavigator->DrawNavigator();
    } else {
        // Original physical address navigator would go here
        ImGui::Text("Physical Address Navigator");
        ImGui::Text("(Enable VA Mode for compressed navigation)");
    }
}

// DrawSearchDialog removed - search is now integrated into magnifier window

void MemoryVisualizer::PerformSearch() {
    searchResults.clear();
    currentSearchResult = 0;
    searchActive = true;
    
    if (strlen(searchPattern) == 0 || currentMemory.data.empty()) {
        return;
    }
    
    // Limit search results to prevent memory explosion
    const size_t MAX_SEARCH_RESULTS = 1000;
    bool hitLimit = false;
    
    // Determine starting position for search
    size_t searchStartOffset = 0;
    if (searchFromCurrent && !searchResults.empty() && currentSearchResult < searchResults.size()) {
        // Start searching after the current result
        uint64_t currentResultAddr = searchResults[currentSearchResult];
        if (currentResultAddr >= viewport.baseAddress && 
            currentResultAddr < viewport.baseAddress + currentMemory.data.size()) {
            searchStartOffset = (currentResultAddr - viewport.baseAddress) + 1;
        }
    }
    
    if (searchType == SEARCH_ASCII) {
        // Search for ASCII string
        size_t patternLen = strlen(searchPattern);
        for (size_t i = searchStartOffset; i <= currentMemory.data.size() - patternLen; i++) {
            if (memcmp(&currentMemory.data[i], searchPattern, patternLen) == 0) {
                // Store absolute address, not offset
                uint64_t absoluteAddr = viewport.baseAddress + i;
                searchResults.push_back(absoluteAddr);
                
                if (searchResults.size() >= MAX_SEARCH_RESULTS) {
                    hitLimit = true;
                    break;
                }
            }
        }
    } else {
        // Search for hex pattern
        std::vector<uint8_t> hexPattern;
        std::string hexStr = searchPattern;
        
        // Remove spaces if any
        hexStr.erase(std::remove(hexStr.begin(), hexStr.end(), ' '), hexStr.end());
        
        // Convert hex string to bytes
        for (size_t i = 0; i < hexStr.length(); i += 2) {
            if (i + 1 < hexStr.length()) {
                std::string byteStr = hexStr.substr(i, 2);
                try {
                    uint8_t byte = static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16));
                    hexPattern.push_back(byte);
                } catch (...) {
                    // Invalid hex, skip
                    return;
                }
            }
        }
        
        if (hexPattern.empty()) {
            return;
        }
        
        // Warn if searching for very common patterns
        if (hexPattern.size() == 1 && hexPattern[0] == 0x00) {
            std::cerr << "Warning: Searching for single zero byte - limiting to first " 
                      << MAX_SEARCH_RESULTS << " results\n";
        }
        
        // Search for hex pattern
        for (size_t i = searchStartOffset; i <= currentMemory.data.size() - hexPattern.size(); i++) {
            if (memcmp(&currentMemory.data[i], hexPattern.data(), hexPattern.size()) == 0) {
                // Store absolute address, not offset
                uint64_t absoluteAddr = viewport.baseAddress + i;
                searchResults.push_back(absoluteAddr);
                
                if (searchResults.size() >= MAX_SEARCH_RESULTS) {
                    hitLimit = true;
                    break;
                }
            }
        }
    }
    
    if (hitLimit) {
        std::cerr << "Search limit reached - showing first " << MAX_SEARCH_RESULTS 
                  << " results only\n";
    }
    
    // If we found results, scroll to the first one (only if needed)
    if (!searchResults.empty()) {
        ScrollToResult(searchResults[0]);
        std::cerr << "Found " << searchResults.size() << " matches in current viewport "
                  << "(" << (viewport.stride * viewport.height) / 1024 << " KB at 0x" 
                  << std::hex << viewport.baseAddress << std::dec << ")\n";
    } else {
        std::cerr << "No matches found in current viewport "
                  << "(" << (viewport.stride * viewport.height) / 1024 << " KB at 0x" 
                  << std::hex << viewport.baseAddress << std::dec << ")\n";
    }
}

void MemoryVisualizer::ScrollToResult(uint64_t resultAddr) {
    // Calculate the viewport boundaries
    uint64_t viewStart = viewport.baseAddress;
    uint64_t viewEnd = viewport.baseAddress + (viewport.height * viewport.stride);
    
    // Check if result is already visible
    if (resultAddr >= viewStart && resultAddr < viewEnd) {
        // Result is visible - no need to scroll
        // Just ensure it's not too close to edges (keep at least 2 lines of context)
        int bytesPerLine = viewport.stride;
        uint64_t resultLine = (resultAddr - viewStart) / bytesPerLine;
        
        if (resultLine < 2) {
            // Too close to top - scroll up a bit
            uint64_t lineAlignedAddr = (resultAddr / bytesPerLine) * bytesPerLine;
            if (lineAlignedAddr >= 2 * bytesPerLine) {
                NavigateToAddress(lineAlignedAddr - 2 * bytesPerLine);
            } else {
                NavigateToAddress(0);
            }
        } else if (resultLine > viewport.height - 3) {
            // Too close to bottom - scroll down a bit
            uint64_t lineAlignedAddr = (resultAddr / bytesPerLine) * bytesPerLine;
            uint64_t targetAddr = lineAlignedAddr - (viewport.height - 3) * bytesPerLine;
            NavigateToAddress(targetAddr);
        }
        // Otherwise result is nicely visible - don't scroll
    } else {
        // Result is off-screen - need to scroll
        int bytesPerLine = viewport.stride;
        uint64_t lineAlignedAddr = (resultAddr / bytesPerLine) * bytesPerLine;
        
        // Position result at about 1/3 from top for good context
        int linesToCenter = viewport.height / 3;
        if (lineAlignedAddr >= linesToCenter * bytesPerLine) {
            NavigateToAddress(lineAlignedAddr - linesToCenter * bytesPerLine);
        } else {
            NavigateToAddress(0);
        }
    }
}

void MemoryVisualizer::FindNext() {
    if (!searchResults.empty()) {
        currentSearchResult = (currentSearchResult + 1) % searchResults.size();
        ScrollToResult(searchResults[currentSearchResult]);
    }
}

void MemoryVisualizer::FindPrevious() {
    if (!searchResults.empty()) {
        if (currentSearchResult > 0) {
            currentSearchResult--;
        } else {
            currentSearchResult = searchResults.size() - 1;
        }
        ScrollToResult(searchResults[currentSearchResult]);
    }
}

bool MemoryVisualizer::InitializeMemoryMap() {
    if (memBase != nullptr) {
        return true;  // Already initialized
    }
    
    // Try to open the memory file
    memFd = open("/tmp/haywire-vm-mem", O_RDONLY);
    if (memFd < 0) {
        std::cerr << "Failed to open /tmp/haywire-vm-mem - full range search not available\n";
        std::cerr << "Make sure QEMU is started with memory-backend-file\n";
        return false;
    }
    
    // Get file size
    off_t fileSize = lseek(memFd, 0, SEEK_END);
    if (fileSize <= 0) {
        close(memFd);
        memFd = -1;
        return false;
    }
    lseek(memFd, 0, SEEK_SET);
    
    memSize = fileSize;
    std::cerr << "Memory file size: " << (memSize / (1024*1024)) << " MB\n";
    
    // Map the entire memory
    memBase = mmap(nullptr, memSize, PROT_READ, MAP_SHARED, memFd, 0);
    if (memBase == MAP_FAILED) {
        std::cerr << "Failed to mmap memory file\n";
        close(memFd);
        memFd = -1;
        memBase = nullptr;
        return false;
    }
    
    std::cerr << "Successfully mapped " << (memSize / (1024*1024)) << " MB of guest memory\n";
    return true;
}

void MemoryVisualizer::CleanupMemoryMap() {
    if (memBase && memBase != MAP_FAILED) {
        munmap(memBase, memSize);
        memBase = nullptr;
    }
    if (memFd >= 0) {
        close(memFd);
        memFd = -1;
    }
    memSize = 0;
}

void MemoryVisualizer::PerformFullRangeSearch() {
    // Initialize memory map if needed
    if (!InitializeMemoryMap()) {
        std::cerr << "Cannot perform full-range search without memory map\n";
        return;
    }
    
    searchResults.clear();
    currentSearchResult = 0;
    searchActive = true;
    
    if (strlen(searchPattern) == 0) {
        return;
    }
    
    const size_t MAX_SEARCH_RESULTS = 1000;
    bool hitLimit = false;
    
    std::cerr << "Searching " << (memSize / (1024*1024)) << " MB of memory for pattern...\n";
    
    // Prepare search pattern
    std::vector<uint8_t> pattern;
    size_t patternLen = 0;
    
    if (searchType == SEARCH_ASCII) {
        // ASCII search
        patternLen = strlen(searchPattern);
        pattern.assign(searchPattern, searchPattern + patternLen);
    } else {
        // Hex search
        std::string hexStr = searchPattern;
        hexStr.erase(std::remove(hexStr.begin(), hexStr.end(), ' '), hexStr.end());
        
        for (size_t i = 0; i < hexStr.length(); i += 2) {
            if (i + 1 < hexStr.length()) {
                std::string byteStr = hexStr.substr(i, 2);
                try {
                    uint8_t byte = static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16));
                    pattern.push_back(byte);
                } catch (...) {
                    std::cerr << "Invalid hex pattern\n";
                    return;
                }
            }
        }
        patternLen = pattern.size();
    }
    
    if (patternLen == 0) {
        return;
    }
    
    // Search through the entire memory using fast memory search
    uint8_t* memBytes = (uint8_t*)memBase;
    size_t searchStart = searchFromCurrent ? viewport.baseAddress : 0;
    size_t searchEnd = memSize - patternLen;
    
    // Use memmem for fast searching (much faster than byte-by-byte)
    // Note: memmem is available on macOS/Linux but not Windows
    uint8_t* currentPos = memBytes + searchStart;
    size_t remainingSize = searchEnd - searchStart + 1;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    size_t lastProgressPos = 0;
    
    while (remainingSize > 0) {
        // Find next occurrence
        void* found = memmem(currentPos, remainingSize, pattern.data(), patternLen);
        if (!found) {
            break;  // No more matches
        }
        
        // Calculate absolute position
        size_t foundPos = (uint8_t*)found - memBytes;
        searchResults.push_back(foundPos);
        
        if (searchResults.size() >= MAX_SEARCH_RESULTS) {
            hitLimit = true;
            break;
        }
        
        // Move past this match
        currentPos = (uint8_t*)found + 1;
        remainingSize = (memBytes + searchEnd + 1) - currentPos;
        
        // Show progress every 500MB or every 1000 results
        size_t currentSearchPos = currentPos - memBytes;
        if (currentSearchPos - lastProgressPos > 500 * 1024 * 1024 || 
            searchResults.size() % 1000 == 0) {
            float progress = (float)currentSearchPos / memSize * 100.0f;
            auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            std::cerr << "Search: " << std::fixed << std::setprecision(1) 
                     << progress << "% (" << searchResults.size() << " found, " 
                     << ms << "ms)\r" << std::flush;
            lastProgressPos = currentSearchPos;
        }
    }
    
    auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::cerr << "\n";  // New line after progress
    std::cerr << "Search completed in " << ms << "ms\n";
    
    if (hitLimit) {
        std::cerr << "Hit limit - showing first " << MAX_SEARCH_RESULTS << " results\n";
    }
    
    std::cerr << "Found " << searchResults.size() << " matches in " 
              << (memSize / (1024*1024)) << " MB\n";
    
    // Navigate to first result if found
    if (!searchResults.empty()) {
        ScrollToResult(searchResults[0]);
    }
}

}