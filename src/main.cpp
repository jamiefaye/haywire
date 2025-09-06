#include <iostream>
#include <chrono>

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
#include "pid_selector.h"

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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    ImGui::StyleColorsDark();
    
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    
    ImVec4 clear_color = ImVec4(0.1f, 0.1f, 0.1f, 1.0f);
    
    QemuConnection qemu;
    MemoryVisualizer visualizer;
    MemoryOverview overview;
    HexOverlay hexOverlay;
    
    // Beacon reader and PID selector
    auto beaconReader = std::make_shared<BeaconReader>();
    PIDSelector pidSelector;
    
    // Initialize beacon reader
    if (beaconReader->Initialize()) {
        std::cout << "Beacon reader initialized successfully\n";
        pidSelector.SetBeaconReader(beaconReader);
        
        // Set callback to switch to process mode when PID is selected
        pidSelector.SetSelectionCallback([&](uint32_t pid) {
            std::cout << "Switching to process " << pid << " mode\n";
            overview.SetProcessMode(true, pid);
            // TODO: Load process sections from beacon instead of guest agent
        });
    } else {
        std::cerr << "Failed to initialize beacon reader - PID selector disabled\n";
    }
    
    // Create viewport translator using guest agent
    std::shared_ptr<ViewportTranslator> translator;
    
    // Auto-connect on startup
    bool autoConnected = qemu.AutoConnect();
    
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
                ImGui::MenuItem("Memory Overview", nullptr, &show_overview);
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
            ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
            ImGui::Begin("Haywire Memory Visualizer", &show_memory_view, ImGuiWindowFlags_NoCollapse);
            
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
            
            if (show_overview) {
                // Left pane: Overview
                ImGui::BeginChild("OverviewPane", ImVec2(300, availableHeight), true);
                overview.DrawCompact();
                ImGui::EndChild();
                
                ImGui::SameLine();
                
                // Right pane: Memory dump
                ImGui::BeginChild("MemoryPane", ImVec2(0, availableHeight), true);
                visualizer.DrawMemoryBitmap();
                ImGui::EndChild();
            } else {
                // Full width memory dump when overview is hidden
                ImGui::BeginChild("MemoryPane", ImVec2(0, availableHeight), true);
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