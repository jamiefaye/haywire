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
    
    // Set QemuConnection for VA->PA translation
    void SetQemuConnection(class QemuConnection* qemu) {
        qemuConnection = qemu;
    }
    
    // Set MemoryMapper for GPA->file offset translation
    void SetMemoryMapper(std::shared_ptr<class MemoryMapper> mapper) {
        memoryMapper = mapper;
    }
    
    // Set current PID for VA mode
    void SetCurrentPID(int pid) {
        currentPid = pid;
    }
    
    // Set whether we're in VA mode
    void SetVAMode(bool vaMode) {
        useVirtualAddresses = vaMode;
    }
    
    // Set memory visualizer viewport info for coordinate conversion
    void SetMemoryViewInfo(ImVec2 viewPos, ImVec2 viewSize, uint64_t baseAddr, int width, int height, int bytesPerPixel) {
        memoryViewPos = viewPos;
        memoryViewSize = viewSize;
        viewportBaseAddress = baseAddr;
        viewportWidth = width;
        viewportHeight = height;
        viewportBytesPerPixel = bytesPerPixel;
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
    
    // Additional dependencies for proper memory access
    class QemuConnection* qemuConnection = nullptr;
    std::shared_ptr<class MemoryMapper> memoryMapper;
    int currentPid = -1;
    bool useVirtualAddresses = false;
    
    // Memory visualizer viewport info for coordinate conversion
    ImVec2 memoryViewPos;
    ImVec2 memoryViewSize;
    uint64_t viewportBaseAddress = 0;
    int viewportWidth = 256;
    int viewportHeight = 256;
    int viewportBytesPerPixel = 1;
};

}  // namespace Haywire