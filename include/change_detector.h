#ifndef HAYWIRE_CHANGE_DETECTOR_H
#define HAYWIRE_CHANGE_DETECTOR_H

#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstdint>
#include <chrono>

namespace Haywire {

class MemoryFileReader;
class CrunchedMemoryReader;
class AddressSpaceFlattener;

// Information about a single chunk's state
struct ChunkInfo {
    uint64_t offset;                          // Byte offset in file (set once, never changes)
    std::atomic<uint32_t> checksum;           // Fast rotation-based hash
    std::atomic<uint64_t> last_change_time;   // Milliseconds since epoch (0 = never changed)
    std::atomic<uint64_t> last_scan_time;     // Milliseconds since epoch
    std::atomic<uint16_t> scan_count;         // Number of times scanned
    std::atomic<bool> is_zero;                // True if entire chunk is zeros
    std::atomic<bool> scanned;                // True if scanned at least once

    ChunkInfo() : offset(0), checksum(0), last_change_time(0),
                  last_scan_time(0), scan_count(0), is_zero(false), scanned(false) {}

    // Move constructor (required for std::vector)
    ChunkInfo(ChunkInfo&& other) noexcept
        : offset(other.offset),
          checksum(other.checksum.load()),
          last_change_time(other.last_change_time.load()),
          last_scan_time(other.last_scan_time.load()),
          scan_count(other.scan_count.load()),
          is_zero(other.is_zero.load()),
          scanned(other.scanned.load()) {}

    // Move assignment (required for std::vector)
    ChunkInfo& operator=(ChunkInfo&& other) noexcept {
        offset = other.offset;
        checksum.store(other.checksum.load());
        last_change_time.store(other.last_change_time.load());
        last_scan_time.store(other.last_scan_time.load());
        scan_count.store(other.scan_count.load());
        is_zero.store(other.is_zero.load());
        scanned.store(other.scanned.load());
        return *this;
    }

    // Delete copy constructor and assignment (atomics can't be copied)
    ChunkInfo(const ChunkInfo&) = delete;
    ChunkInfo& operator=(const ChunkInfo&) = delete;
};

// Change detection with background patrol thread
class ChangeDetector {
public:
    explicit ChangeDetector(MemoryFileReader* reader, size_t chunk_size = 65536);
    ~ChangeDetector();

    // Control
    void Start();
    void Stop();
    bool IsRunning() const { return running_; }

    // Configuration
    void SetScanInterval(int milliseconds) { scan_interval_ms_ = milliseconds; }
    void SetVisibleRange(size_t start_chunk, size_t end_chunk);
    void SetHeatMapRange(size_t start_chunk, size_t end_chunk);

    // VA mode support
    void SetVAMode(bool enabled, CrunchedMemoryReader* crunchedReader = nullptr);
    bool IsVAMode() const { return va_mode_enabled_; }

    // Query chunk state (thread-safe reads, no lock needed)
    size_t GetChunkCount() const { return chunks_.size(); }
    const ChunkInfo& GetChunk(size_t chunk_idx) const;
    bool HasChanged(size_t chunk_idx) const;

    // Statistics
    uint64_t GetTotalScans() const { return total_scans_; }
    uint64_t GetChangesDetected() const { return changes_detected_; }

private:
    // Memory access
    MemoryFileReader* memory_reader_;
    CrunchedMemoryReader* crunched_reader_;
    std::atomic<bool> va_mode_enabled_;
    size_t chunk_size_;
    std::vector<ChunkInfo> chunks_;

    // Patrol thread
    std::thread patrol_thread_;
    std::atomic<bool> running_;
    std::atomic<int> scan_interval_ms_;

    // Visible ranges (protected by mutex)
    std::mutex range_mutex_;
    size_t visible_start_;
    size_t visible_end_;
    size_t heatmap_start_;
    size_t heatmap_end_;

    // Statistics
    std::atomic<uint64_t> total_scans_;
    std::atomic<uint64_t> changes_detected_;

    // Background scan position
    size_t background_scan_pos_;

    // Internal methods
    void PatrolLoop();
    void ScanChunk(size_t chunk_idx);
    void RebuildChunkArray();  // Rebuild when switching PA/VA mode
    uint32_t CalculateChecksum(const uint8_t* data, size_t size);
    bool TestChunkZero(const uint8_t* data, size_t size);
    uint64_t GetMilliseconds() const;
};

} // namespace Haywire

#endif // HAYWIRE_CHANGE_DETECTOR_H