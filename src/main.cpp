#include <iostream>
#include <iomanip>
#include <chrono>
#include <unordered_map>
#include <unistd.h>

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "qemu_connection.h"
#include "memory_visualizer.h"
#include "memory_overview.h"
#include "hex_overlay.h"
#include "viewport_translator.h"
#include "beacon_reader.h"
#include "beacon_decoder.h"
#include "beacon_translator.h"
#include "pid_selector.h"
#include "memory_mapper.h"

using namespace Haywire;

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

int main(int argc, char** argv) {
    glfwSetErrorCallback(glfw_error_callback);
    
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return -1;
    }
    
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Haywire - Memory Visualizer", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    // Disable ImGui keyboard navigation to use our own shortcuts
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    ImGui::StyleColorsDark();
    
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    
    ImVec4 clear_color = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    
    QemuConnection qemu;
    MemoryVisualizer visualizer;
    MemoryOverview overview;
    HexOverlay hexOverlay;
    
    // Create memory mapper for address translation
    auto memoryMapper = std::make_shared<MemoryMapper>();
    
    // Auto-connect to QEMU first (before beacon reader)
    bool autoConnected = qemu.AutoConnect();
    
    // If connected, initialize memory mapper
    if (autoConnected) {
        memoryMapper->DiscoverMemoryMap("localhost", 4444);
        memoryMapper->LogRegions();
    }
    
    // Beacon reader and PID selector
    auto beaconReader = std::make_shared<BeaconReader>();
    std::shared_ptr<BeaconTranslator> beaconTranslator;
    PIDSelector pidSelector;
    
    // Initialize beacon reader
    if (beaconReader->Initialize()) {
        std::cout << "Beacon reader initialized successfully\n";
        
        // Try to start companion automatically if beacon data is not found
        if (!beaconReader->FindDiscovery()) {
            std::cout << "No beacon data found - attempting to start companion...\n";
            if (qemu.IsGuestAgentConnected()) {
                beaconReader->StartCompanion(qemu.GetGuestAgent());
                // Give companion time to start and create beacon data
                sleep(2);
                beaconReader->FindDiscovery();
            } else {
                std::cout << "Guest agent not connected - cannot auto-start companion\n";
                std::cout << "You can manually start the companion in the VM or ensure QGA is running\n";
            }
        }
        
        pidSelector.SetBeaconReader(beaconReader);
        
        // Create beacon translator using the reader
        beaconTranslator = std::make_shared<BeaconTranslator>(beaconReader);
        visualizer.SetBeaconTranslator(beaconTranslator);
        visualizer.SetBeaconReader(beaconReader);  // Also set beacon reader for bitmap viewers
        visualizer.SetQemuConnection(&qemu);  // Pass QEMU connection for VA->PA translation
        visualizer.SetMemoryMapper(memoryMapper);  // Pass memory mapper for GPA->offset translation
        std::cout << "Beacon translator created and connected to visualizer\n";
        
        // Set callback to switch to process mode when PID is selected
        pidSelector.SetSelectionCallback([&](uint32_t pid, const std::string& processName) {
            std::cout << "\n=== Main: PID Selection Callback ===\n";
            std::cout << "Switching to process " << pid << " (" << processName << ") mode\n";
            overview.SetProcessMode(true, pid);
            
            // Store process name in visualizer for display
            visualizer.SetCurrentProcessName(processName);
            
            // First, tell the camera to focus on this PID
            beaconReader->SetCameraFocus(1, pid);
            
            // Load process sections from camera beacon data with retry
            std::vector<SectionEntry> sections;
            std::cout << "Waiting for camera to scan PID " << pid << "...\n";
            
            // Retry for up to 3 seconds for camera data to become available
            int retries = 30;  // 30 * 100ms = 3 seconds
            bool gotSections = false;
            while (retries > 0) {
                if (beaconReader->GetCameraProcessSections(1, pid, sections) && !sections.empty()) {
                    gotSections = true;
                    break;
                }
                usleep(100000);  // Wait 100ms
                retries--;
                
                // Re-scan beacon data to pick up new camera writes
                beaconReader->FindDiscovery();
            }
            
            if (gotSections) {
                std::cout << "\n=== Memory Map for PID " << pid << " ===\n";
                std::cout << "Loaded " << sections.size() << " memory sections from camera beacon\n";
                std::cout << "------------------------------------------------\n";
                
                // Print section details
                for (const auto& section : sections) {
                    // Convert permissions bitfield to string
                    std::string perms;
                    perms += (section.perms & 0x1) ? 'r' : '-';  // PROT_READ
                    perms += (section.perms & 0x2) ? 'w' : '-';  // PROT_WRITE
                    perms += (section.perms & 0x4) ? 'x' : '-';  // PROT_EXEC
                    perms += (section.perms & 0x8) ? 'p' : 's';  // MAP_PRIVATE/SHARED
                    
                    // Calculate size in KB/MB
                    uint64_t size = section.va_end - section.va_start;
                    std::string sizeStr;
                    if (size >= 1024*1024*1024) {
                        sizeStr = std::to_string(size / (1024*1024*1024)) + " GB";
                    } else if (size >= 1024*1024) {
                        sizeStr = std::to_string(size / (1024*1024)) + " MB";
                    } else if (size >= 1024) {
                        sizeStr = std::to_string(size / 1024) + " KB";
                    } else {
                        sizeStr = std::to_string(size) + " B";
                    }
                    
                    std::cout << std::hex << std::setfill('0') 
                              << "  0x" << std::setw(12) << section.va_start 
                              << "-0x" << std::setw(12) << section.va_end 
                              << std::dec << std::setfill(' ')
                              << " " << perms 
                              << " " << std::setw(8) << sizeStr
                              << " " << section.path << "\n";
                }
                std::cout << "------------------------------------------------\n";
                
                // Also get and display PTEs
                std::unordered_map<uint64_t, uint64_t> ptes;
                if (beaconReader->GetCameraPTEs(1, pid, ptes)) {
                    std::cout << "\n=== Page Table Entries for PID " << pid << " ===\n";
                    std::cout << "Found " << ptes.size() << " PTEs\n";
                    
                    // Show all PTEs
                    for (const auto& [va, pa] : ptes) {
                        std::cout << "  VA: 0x" << std::hex << va 
                                  << " -> PA: 0x" << pa << std::dec << "\n";
                    }
                    std::cout << "------------------------------------------------\n";
                }
                
                // Convert SectionEntry to GuestMemoryRegion for visualization
                std::vector<GuestMemoryRegion> regions;
                for (const auto& section : sections) {
                    GuestMemoryRegion region;
                    region.start = section.va_start;
                    region.end = section.va_end;
                    
                    // Convert permissions bitfield to string
                    std::string perms;
                    perms += (section.perms & 0x1) ? 'r' : '-';  // PROT_READ
                    perms += (section.perms & 0x2) ? 'w' : '-';  // PROT_WRITE
                    perms += (section.perms & 0x4) ? 'x' : '-';  // PROT_EXEC
                    perms += (section.perms & 0x8) ? 'p' : 's';  // MAP_PRIVATE/SHARED
                    region.permissions = perms;
                    
                    region.name = section.path;
                    regions.push_back(region);
                }
                std::cout << "------------------------------------------------\n";
                
                // Load the memory map into the visualizer
                visualizer.LoadMemoryMap(regions);
                visualizer.SetProcessPid(pid);
                
                // Also load sections into the overview
                overview.LoadProcessSections(regions);
                
                // Navigate to first executable or readable region to avoid 0x0 errors
                uint64_t startAddr = 0;
                for (const auto& region : regions) {
                    // Look for first executable region (likely the main binary)
                    if (region.permissions.find('x') != std::string::npos) {
                        startAddr = region.start;
                        break;
                    }
                }
                // If no executable found, use first readable region
                if (startAddr == 0) {
                    for (const auto& region : regions) {
                        if (region.permissions.find('r') != std::string::npos) {
                            startAddr = region.start;
                            break;
                        }
                    }
                }
                
                if (startAddr != 0) {
                    visualizer.NavigateToAddress(startAddr);
                    std::cout << "Navigated to address 0x" << std::hex << startAddr << std::dec << "\n";
                }
                
                // Also trigger the onProcessMapLoaded callback
                if (visualizer.onProcessMapLoaded) {
                    visualizer.onProcessMapLoaded(pid, regions);
                }
            } else {
                std::cout << "Waiting for camera data for PID " << pid << "\n";
            }
        });
    } else {
        std::cerr << "Failed to initialize beacon reader - PID selector disabled\n";
    }
    
    // Create viewport translator using guest agent
    std::shared_ptr<ViewportTranslator> translator;
    
    // If connected and guest agent available, create translator
    if (autoConnected && qemu.IsGuestAgentConnected()) {
        translator = std::make_shared<ViewportTranslator>(qemu.GetGuestAgentPtr());
        visualizer.SetTranslator(translator);
        visualizer.SetGuestAgent(qemu.GetGuestAgent());
        std::cerr << "Viewport translator initialized with guest agent" << std::endl;
    }
    
    // Connect visualizer to overview for process map display
    visualizer.onProcessMapLoaded = [&overview, &visualizer](int pid, const std::vector<GuestMemoryRegion>& regions) {
        overview.SetProcessMode(true, pid);
        overview.SetFlattener(visualizer.GetFlattener().get());
        overview.LoadProcessMap(visualizer.GetGuestAgent());
        
        // Set navigation callback to update visualizer when clicking in overview
        overview.SetNavigationCallback([&visualizer](uint64_t va) {
            visualizer.NavigateToAddress(va);
        });
    };
    
    bool show_demo_window = false;
    bool show_metrics = false;  // Hidden by default
    bool show_memory_view = true;
    bool show_overview = false;
    bool show_connection_window = !autoConnected;  // Only show if auto-connect failed
    
    float fps = 0.0f;
    auto lastTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    
    // Track beacon refresh timing
    auto lastBeaconRefresh = std::chrono::high_resolution_clock::now();
    const auto beaconRefreshInterval = std::chrono::seconds(2); // Refresh every 2 seconds
    
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        
        auto currentTime = std::chrono::high_resolution_clock::now();
        frameCount++;
        auto delta = std::chrono::duration<float>(currentTime - lastTime).count();
        if (delta >= 1.0f) {
            fps = frameCount / delta;
            frameCount = 0;
            lastTime = currentTime;
        }
        
        // Periodically refresh beacon data
        if (currentTime - lastBeaconRefresh > beaconRefreshInterval) {
            lastBeaconRefresh = currentTime;
            
            // Rescan beacon data
            if (beaconReader->FindDiscovery()) {
                // Beacon data refreshed - process list will update automatically
                // PIDSelector will see the new data next time it's opened
            }
        }
        
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Connect to QEMU")) {
                    show_connection_window = true;
                }
                if (ImGui::MenuItem("Load Memory Dump")) {
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) {
                    glfwSetWindowShouldClose(window, true);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("QEMU Connection", nullptr, &show_connection_window);
                ImGui::Separator();
                ImGui::MenuItem("Memory Visualizer", nullptr, &show_memory_view);
                ImGui::MenuItem("Memory Sections", nullptr, &show_overview);
                ImGui::Separator();
                if (ImGui::MenuItem("Process Selector", "P")) {
                    pidSelector.Show();
                }
                ImGui::Separator();
                ImGui::MenuItem("Metrics", nullptr, &show_metrics);
                ImGui::MenuItem("Demo Window", nullptr, &show_demo_window);
                ImGui::EndMenu();
            }
            
            ImGui::Text("FPS: %.1f", fps);
            
            ImGui::EndMainMenuBar();
        }
        
        // Main Haywire window with classic layout (draw first so connection appears on top)
        if (show_memory_view) {
            ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(50, 100), ImGuiCond_FirstUseEver);  // Start below menu bar
            ImGui::Begin("Haywire Memory Visualizer", &show_memory_view, 
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
            
            // Top bar with controls (full width, compact)
            ImGui::BeginChild("ControlBar", ImVec2(0, 60), true);
            visualizer.DrawControlBar(qemu);
            
            // Add Process Selector button
            ImGui::SameLine();
            ImGui::Separator();
            ImGui::SameLine();
            
            if (ImGui::Button("Process Selector [P]")) {
                pidSelector.Show();
            }
            
            // Handle 'P' hotkey
            if (ImGui::IsKeyPressed(ImGuiKey_P) && !ImGui::GetIO().WantTextInput) {
                pidSelector.ToggleVisible();
            }
            
            ImGui::EndChild();
            
            // Bottom section with two panes
            float availableHeight = ImGui::GetContentRegionAvail().y;
            // Ensure minimum height to prevent overflow on first frame
            availableHeight = std::max(100.0f, availableHeight);
            
            if (show_overview) {
                // Left pane: Memory Sections
                ImGui::BeginChild("SectionsPane", ImVec2(300, availableHeight), true);
                overview.DrawCompact();
                ImGui::EndChild();
                
                ImGui::SameLine();
                
                // Right pane: Memory dump
                ImGui::BeginChild("MemoryPane", ImVec2(0, availableHeight), false);
                visualizer.DrawMemoryBitmap();
                ImGui::EndChild();
            } else {
                // Full width memory dump when overview is hidden
                ImGui::BeginChild("MemoryPane", ImVec2(0, availableHeight), false);
                visualizer.DrawMemoryBitmap();
                ImGui::EndChild();
            }
            
            // Update overview with any memory the visualizer just read
            if (show_overview && visualizer.HasMemory()) {
                auto& mem = visualizer.GetCurrentMemory();
                if (!mem.data.empty()) {
                    overview.UpdateRegion(mem.address, mem.data.size(), mem.data.data());
                }
            }
            
            if (visualizer.IsHexOverlayEnabled()) {
                hexOverlay.Draw(visualizer);
            }
            
            ImGui::End();
            
            // Draw bitmap viewers (they render as floating windows)
            // This must be outside the main window context
            visualizer.DrawBitmapViewers();
        }
        
        // QEMU Connection window (draw after main window so it appears on top)
        if (show_connection_window) {
            ImGui::SetNextWindowPos(ImVec2(400, 200), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(400, 150), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowFocus();  // Bring to front
            ImGui::Begin("QEMU Connection", &show_connection_window, 
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
            qemu.DrawConnectionUI();
            ImGui::End();
            
            // Create translator when newly connected
            if (qemu.IsConnected() && qemu.IsGuestAgentConnected() && !translator) {
                translator = std::make_shared<ViewportTranslator>(qemu.GetGuestAgentPtr());
                visualizer.SetTranslator(translator);
                visualizer.SetGuestAgent(qemu.GetGuestAgent());
                std::cerr << "Viewport translator initialized with guest agent" << std::endl;
            }
        }
        
        // Handle overview scanning when connected
        if (show_overview && qemu.IsConnected()) {
            static bool layoutScanned = false;
            if (!layoutScanned) {
                overview.ScanMemoryLayout(qemu);
                layoutScanned = true;
            }
        }
        
        if (show_metrics) {
            ImGui::Begin("Performance Metrics", &show_metrics);
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 
                       1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
            ImGui::Text("Connected: %s", qemu.IsConnected() ? "Yes" : "No");
            if (qemu.IsConnected()) {
                ImGui::Text("Memory Read Speed: %.2f MB/s", qemu.GetReadSpeed());
            }
            ImGui::End();
        }
        
        if (show_demo_window) {
            ImGui::ShowDemoWindow(&show_demo_window);
        }
        
        // Draw PID selector window if visible
        pidSelector.Draw();
        
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        glfwSwapBuffers(window);
    }
    
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    return 0;
}