#pragma once

#include "common.h"
#include "autocorrelator.h"
#include "guest_agent.h"  // For GuestMemoryRegion
#include "bitmap_viewer.h"
#include <imgui.h>
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
#include <deque>

namespace Haywire {

class QemuConnection;
class ViewportTranslator;
class BeaconTranslator;
class AddressSpaceFlattener;
class CrunchedRangeNavigator;
class CrunchedMemoryReader;
class GuestAgent;

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
    
    // VA to PA translation
    void SetTranslator(std::shared_ptr<ViewportTranslator> translator);
    void SetBeaconTranslator(std::shared_ptr<BeaconTranslator> beaconTranslator);
    void SetProcessPid(int pid);
    void SetGuestAgent(GuestAgent* agent) { guestAgent = agent; }
    
    // Load memory map for navigation
    void LoadMemoryMap(const std::vector<GuestMemoryRegion>& regions);
    
    // Callback when process map is loaded
    std::function<void(int, const std::vector<GuestMemoryRegion>&)> onProcessMapLoaded;
    
    // Temporary public access for wiring (should refactor later)
    std::unique_ptr<AddressSpaceFlattener>& GetFlattener() { return addressFlattener; }
    GuestAgent* GetGuestAgent() { return guestAgent; }
    
    // Process name display
    void SetCurrentProcessName(const std::string& name) { currentProcessName = name; }
    const std::string& GetCurrentProcessName() const { return currentProcessName; }
    
    // PNG export
    bool ExportToPNG(const std::string& filename);
    
    // Draw bitmap viewers (floating windows)
    void DrawBitmapViewers();
    
    // Set beacon reader for bitmap viewers
    void SetBeaconReader(std::shared_ptr<class BeaconReader> reader);
    
    // Set QEMU connection for bitmap viewers
    void SetQemuConnection(class QemuConnection* qemu);
    
    // Set memory mapper for bitmap viewers
    void SetMemoryMapper(std::shared_ptr<class MemoryMapper> mapper);
    
    // Check if any bitmap viewer anchor is being dragged
    bool IsBitmapAnchorDragging() const;
    
private:
    void DrawControls();
    void DrawMemoryView();
    void DrawNavigator();
    void DrawVerticalAddressSlider();
    void HandleInput();
    
    void CreateTexture();
    void UpdateTexture();
    std::vector<uint32_t> ConvertMemoryToPixels(const MemoryBlock& memory);
    std::vector<uint32_t> ConvertMemoryToHexPixels(const MemoryBlock& memory);
    std::vector<uint32_t> ConvertMemoryToCharPixels(const MemoryBlock& memory);
    
    ViewportSettings viewport;
    MemoryBlock currentMemory;
    
    GLuint memoryTexture;
    std::vector<uint32_t> pixelBuffer;
    
    bool needsUpdate;
    bool autoRefresh;
    bool autoRefreshInitialized;
    float refreshRate;
    std::chrono::steady_clock::time_point lastRefresh;
    
    bool showHexOverlay;
    bool showNavigator;
    bool showCorrelation;
    bool showChangeHighlight;
    bool showMagnifier;
    
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
    
    // Magnifier
    void DrawMagnifier();
    int magnifierZoom;  // Magnification level (2, 4, 8, etc.)
    bool magnifierLocked;  // Lock position vs follow mouse
    int magnifierSize;  // Size of area to magnify (in source pixels)
    ImVec2 magnifierLockPos;  // Locked position if not following mouse
    ImVec2 memoryViewPos;  // Position of memory view canvas for magnifier
    ImVec2 memoryViewSize;  // Size of memory view canvas
    
    // Search feature (integrated into magnifier)
    void PerformSearch();
    void PerformFullRangeSearch();
    void FindNext();
    void FindPrevious();
    void ScrollToResult(uint64_t resultAddr);
    bool InitializeMemoryMap();
    void CleanupMemoryMap();
    
    enum SearchType { SEARCH_ASCII, SEARCH_HEX };
    SearchType searchType;
    char searchPattern[256];
    std::vector<uint64_t> searchResults;  // Store absolute addresses, not offsets
    size_t currentSearchResult;
    bool searchActive;
    bool searchFromCurrent;  // If true, search from current position; if false, from beginning
    bool searchFullRange;  // If true, search entire memory space
    
    // Memory-mapped access for full-range search
    int memFd;
    void* memBase;
    size_t memSize;
    
    // Change tracking
    struct ChangeRegion {
        int x, y;
        int width, height;
        std::chrono::steady_clock::time_point detectedTime;
    };
    std::vector<ChangeRegion> changedRegions;
    
    // Ring buffer for accumulating changes
    static constexpr size_t CHANGE_HISTORY_SIZE = 10;
    std::deque<std::vector<ChangeRegion>> changeHistory;
    std::chrono::steady_clock::time_point lastChangeTime;
    
    // VA to PA translation
    std::shared_ptr<ViewportTranslator> viewportTranslator;
    int targetPid;
    bool useVirtualAddresses;  // Toggle between VA and PA
    
    // Address space flattening for navigation
    std::unique_ptr<AddressSpaceFlattener> addressFlattener;
    std::unique_ptr<CrunchedRangeNavigator> crunchedNavigator;
    std::unique_ptr<CrunchedMemoryReader> crunchedReader;
    
    // Guest agent for loading memory maps
    GuestAgent* guestAgent;
    
    std::chrono::steady_clock::time_point changeDetectedTime;
    float marchingAntsPhase;  // For animating the marching ants pattern
    
    // Current process information
    std::string currentProcessName;
    
    // Mini bitmap viewers
    std::unique_ptr<BitmapViewerManager> bitmapViewerManager;
    uint64_t contextMenuAddress;
    ImVec2 contextMenuPos;
};

}