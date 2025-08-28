#include <iostream>
#include <chrono>

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "qemu_connection.h"
#include "memory_visualizer.h"
#include "hex_overlay.h"

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
    HexOverlay hexOverlay;
    
    bool show_demo_window = false;
    bool show_metrics = true;
    bool show_memory_view = true;
    bool show_connection_window = true;
    
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
                ImGui::MenuItem("Memory Visualizer", nullptr, &show_memory_view);
                ImGui::MenuItem("Metrics", nullptr, &show_metrics);
                ImGui::MenuItem("Demo Window", nullptr, &show_demo_window);
                ImGui::EndMenu();
            }
            
            ImGui::Text("FPS: %.1f", fps);
            
            ImGui::EndMainMenuBar();
        }
        
        if (show_connection_window) {
            ImGui::Begin("QEMU Connection", &show_connection_window);
            qemu.DrawConnectionUI();
            ImGui::End();
        }
        
        if (show_memory_view) {
            ImGui::Begin("Memory Visualizer", &show_memory_view);
            visualizer.Draw(qemu);
            if (visualizer.IsHexOverlayEnabled()) {
                hexOverlay.Draw(visualizer);
            }
            ImGui::End();
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