#pragma once

#include <vector>
#include <string>
#include "imgui.h"
#include "common.h"
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace Haywire {

struct BitmapViewer {
    // Basic identification
    int id;
    std::string name;
    bool active = true;
    
    // Window position and size
    ImVec2 windowPos = ImVec2(100, 100);
    ImVec2 windowSize = ImVec2(256, 256);
    
    // Leader line anchor point (in main memory view)
    ImVec2 anchorPos = ImVec2(0, 0);
    
    // Screen position of the image in the window
    ImVec2 imageScreenPos = ImVec2(0, 0);
    bool showLeader = true;
    
    // Memory configuration
    uint64_t memoryAddress = 0;
    int memWidth = 256;
    int memHeight = 256;
    int stride = 256;
    PixelFormat format = PixelFormat::RGB888;
    
    // Rendering
    GLuint texture = 0;
    std::vector<uint32_t> pixels;
    bool needsUpdate = true;
    
    // Interaction state
    bool isDragging = false;
    bool isResizing = false;
    bool isDraggingAnchor = false;
    bool isPinned = false;  // When pinned, window goes to background
    
    // For dragging
    ImVec2 dragOffset;
};

class BitmapViewerManager {
public:
    BitmapViewerManager();
    ~BitmapViewerManager();
    
    // Set the beacon reader for memory access
    void SetBeaconReader(std::shared_ptr<class BeaconReader> reader) { 
        beaconReader = reader; 
    }
    
    // Create a new viewer at the specified memory location
    void CreateViewer(uint64_t address, ImVec2 anchorPos);
    
    // Remove a viewer
    void RemoveViewer(int id);
    
    // Draw all viewers (called from main render loop)
    void DrawViewers();
    
    // Update memory for all viewers
    void UpdateViewers();
    
    // Handle right-click context menu
    void HandleContextMenu(uint64_t clickAddress, ImVec2 clickPos);
    
    // Get number of viewers
    size_t GetViewerCount() const { return viewers.size(); }
    
    // Check if any anchor is currently being dragged
    bool IsAnyAnchorDragging() const {
        for (const auto& viewer : viewers) {
            if (viewer.isDraggingAnchor) return true;
        }
        return false;
    }
    
private:
    std::vector<BitmapViewer> viewers;
    int nextId = 1;
    
    void DrawViewer(BitmapViewer& viewer);
    void DrawLeaderLine(BitmapViewer& viewer);
    void UpdateViewerTexture(BitmapViewer& viewer);
    void ExtractMemory(BitmapViewer& viewer);
    
    // Convert memory position to screen position
    ImVec2 MemoryToScreen(uint64_t address);
    uint64_t ScreenToMemoryAddress(ImVec2 screenPos);
    
    // Beacon reader for memory access
    std::shared_ptr<class BeaconReader> beaconReader;
    class MemoryVisualizer* memoryVisualizer = nullptr;
};

}  // namespace Haywire