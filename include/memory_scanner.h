#pragma once

#include <vector>
#include <cstdint>
#include <string>

namespace Haywire {

class QemuConnection;

struct MemoryRegion {
    uint64_t start;
    uint64_t size;
    std::string description;
    float entropy;  // 0-1, measure of randomness
    bool isLikelyVideo;
};

class MemoryScanner {
public:
    MemoryScanner();
    ~MemoryScanner();
    
    // Scan for valid memory regions
    void ScanMemoryMap(QemuConnection& qemu);
    
    // Quick scan to find regions with data
    void QuickScan(QemuConnection& qemu);
    
    // Analyze a region to see if it looks like video
    bool AnalyzeForVideo(QemuConnection& qemu, uint64_t address, size_t size);
    
    // Get list of found regions
    const std::vector<MemoryRegion>& GetRegions() const { return regions; }
    
    // Draw overview map
    void DrawOverview();
    
    // Calculate entropy of data (0=uniform, 1=random)
    static float CalculateEntropy(const std::vector<uint8_t>& data);
    
    // Check if data looks like video frame
    static bool LooksLikeVideo(const std::vector<uint8_t>& data);
    
private:
    std::vector<MemoryRegion> regions;
    bool scanning;
    float scanProgress;
    
    // Common memory ranges to check on different architectures
    static const uint64_t commonRanges[][2];
};

}