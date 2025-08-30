#pragma once

#include "common.h"
#include "autocorrelator.h"
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>

namespace Haywire {

class QemuConnection;

class MemoryVisualizer {
public:
    MemoryVisualizer();
    ~MemoryVisualizer();
    
    void Draw(QemuConnection& qemu);
    void DrawControlBar(QemuConnection& qemu);
    void DrawMemoryBitmap();
    
    void SetViewport(const ViewportSettings& settings);
    ViewportSettings GetViewport() const { return viewport; }
    
    void UpdateMemoryTexture(const MemoryBlock& memory);
    
    bool IsHexOverlayEnabled() const { return showHexOverlay; }
    void SetHexOverlayEnabled(bool enabled) { showHexOverlay = enabled; }
    
    uint32_t GetPixelAt(int x, int y) const;
    uint64_t GetAddressAt(int x, int y) const;
    
    void NavigateToAddress(uint64_t address);
    
    const MemoryBlock& GetCurrentMemory() const { return currentMemory; }
    bool HasMemory() const { return !currentMemory.data.empty(); }
    
private:
    void DrawControls();
    void DrawMemoryView();
    void DrawNavigator();
    void DrawVerticalAddressSlider();
    void HandleInput();
    
    void CreateTexture();
    void UpdateTexture();
    std::vector<uint32_t> ConvertMemoryToPixels(const MemoryBlock& memory);
    
    ViewportSettings viewport;
    MemoryBlock currentMemory;
    
    GLuint memoryTexture;
    std::vector<uint32_t> pixelBuffer;
    
    bool needsUpdate;
    bool autoRefresh;
    float refreshRate;
    std::chrono::steady_clock::time_point lastRefresh;
    
    bool showHexOverlay;
    bool showNavigator;
    bool showCorrelation;
    
    char addressInput[32];
    int widthInput;
    int heightInput;
    int strideInput;
    int pixelFormatIndex;
    
    float mouseX, mouseY;
    bool isDragging;
    float dragStartX, dragStartY;
    
    // Async reading support
    std::thread readThread;
    std::atomic<bool> isReading;
    std::atomic<bool> readComplete;
    std::mutex memoryMutex;
    MemoryBlock pendingMemory;
    std::string readStatus;
    
    // Autocorrelation
    Autocorrelator correlator;
    void DrawCorrelationStripe();
};

}