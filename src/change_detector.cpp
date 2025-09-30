#include "change_detector.h"
#include "memory_file_reader.h"
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstring>
#include <iostream>

#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace Haywire {

ChangeDetector::ChangeDetector(MemoryFileReader* reader, size_t chunk_size)
    : memory_reader_(reader), chunk_size_(chunk_size), running_(false),
      scan_interval_ms_(10), visible_start_(0), visible_end_(0),
      heatmap_start_(0), heatmap_end_(0), total_scans_(0), changes_detected_(0),
      background_scan_pos_(0) {

    if (!reader) {
        return;
    }

    size_t file_size = reader->GetMemorySize();
    size_t num_chunks = (file_size + chunk_size - 1) / chunk_size;

    // Pre-allocate chunk info array
    chunks_.resize(num_chunks);
    for (size_t i = 0; i < num_chunks; i++) {
        chunks_[i].offset = i * chunk_size;
    }
}

ChangeDetector::~ChangeDetector() {
    Stop();
}

void ChangeDetector::Start() {
    if (running_) {
        return;
    }

    std::cout << "ChangeDetector: Starting patrol thread...\n";
    running_ = true;
    patrol_thread_ = std::thread(&ChangeDetector::PatrolLoop, this);
}

void ChangeDetector::Stop() {
    if (!running_) {
        return;
    }

    running_ = false;
    if (patrol_thread_.joinable()) {
        patrol_thread_.join();
    }
}

void ChangeDetector::SetVisibleRange(size_t start_chunk, size_t end_chunk) {
    std::lock_guard<std::mutex> lock(range_mutex_);
    visible_start_ = start_chunk;
    visible_end_ = std::min(end_chunk, chunks_.size());
}

void ChangeDetector::SetHeatMapRange(size_t start_chunk, size_t end_chunk) {
    std::lock_guard<std::mutex> lock(range_mutex_);
    heatmap_start_ = start_chunk;
    heatmap_end_ = std::min(end_chunk, chunks_.size());
}

const ChunkInfo& ChangeDetector::GetChunk(size_t chunk_idx) const {
    static ChunkInfo dummy;
    if (chunk_idx >= chunks_.size()) {
        return dummy;
    }
    return chunks_[chunk_idx];
}

bool ChangeDetector::HasChanged(size_t chunk_idx) const {
    if (chunk_idx >= chunks_.size()) {
        return false;
    }
    return chunks_[chunk_idx].last_change_time > 0;
}

uint64_t ChangeDetector::GetMilliseconds() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// Fast rotation-based checksum (matches web version)
uint32_t ChangeDetector::CalculateChecksum(const uint8_t* data, size_t size) {
    uint32_t hash = 0;

#ifdef __x86_64__
    // SIMD version for x86-64
    // Process 32 bytes at a time with AVX2
    size_t simd_size = size & ~31;  // Round down to multiple of 32

    __m256i hash_vec = _mm256_setzero_si256();

    for (size_t i = 0; i < simd_size; i += 32) {
        __m256i data_vec = _mm256_loadu_si256((__m256i*)(data + i));

        // Rotate and XOR (simplified for SIMD)
        // Note: Full rotation in SIMD is complex, use simpler mixing
        hash_vec = _mm256_xor_si256(hash_vec, data_vec);
    }

    // Combine lanes
    uint8_t temp[32];
    _mm256_storeu_si256((__m256i*)temp, hash_vec);
    for (int i = 0; i < 32; i++) {
        hash = ((hash << 5) | (hash >> 27)) ^ temp[i];
    }

    // Process remaining bytes
    for (size_t i = simd_size; i < size; i++) {
        hash = ((hash << 5) | (hash >> 27)) ^ data[i];
    }
#else
    // Scalar version
    for (size_t i = 0; i < size; i++) {
        hash = ((hash << 5) | (hash >> 27)) ^ data[i];
    }
#endif

    return hash;
}

// Test if chunk is all zeros (using optimized method from MemoryVisualizer)
bool ChangeDetector::TestChunkZero(const uint8_t* data, size_t size) {
    // Use 64-bit OR accumulation for speed
    const uint64_t* data64 = reinterpret_cast<const uint64_t*>(data);
    size_t count64 = size / 8;

    uint64_t acc = 0;

    // Process 8 qwords at a time (unrolled loop)
    size_t i = 0;
    for (; i + 7 < count64; i += 8) {
        acc |= data64[i] | data64[i+1] | data64[i+2] | data64[i+3] |
               data64[i+4] | data64[i+5] | data64[i+6] | data64[i+7];
    }

    // Process remaining qwords
    for (; i < count64; i++) {
        acc |= data64[i];
    }

    if (acc != 0) {
        return false;
    }

    // Check remaining bytes
    size_t remaining = size % 8;
    for (size_t j = 0; j < remaining; j++) {
        if (data[size - remaining + j] != 0) {
            return false;
        }
    }

    return true;
}

void ChangeDetector::ScanChunk(size_t chunk_idx) {
    if (chunk_idx >= chunks_.size() || !memory_reader_) {
        return;
    }

    ChunkInfo& chunk = chunks_[chunk_idx];
    uint64_t now = GetMilliseconds();

    // Read chunk data
    size_t size = std::min(chunk_size_, (size_t)(memory_reader_->GetMemorySize() - chunk.offset));
    const uint8_t* data = memory_reader_->GetMemoryPointer(chunk.offset);

    if (!data) {
        return;  // Invalid offset
    }

    // Check if zero
    bool is_zero = TestChunkZero(data, size);

    // Calculate checksum (skip for zero chunks)
    uint32_t checksum = is_zero ? 0 : CalculateChecksum(data, size);

    // Detect changes
    bool is_first_scan = !chunk.scanned;
    bool checksum_changed = !is_first_scan && chunk.checksum != checksum;
    bool zero_changed = !is_first_scan && chunk.is_zero != is_zero;
    bool has_changed = checksum_changed || zero_changed;

    // Changes are being detected successfully

    // Update chunk info
    chunk.checksum = checksum;
    chunk.is_zero = is_zero;
    chunk.scanned = true;
    chunk.scan_count++;
    chunk.last_scan_time = now;

    if (has_changed) {
        chunk.last_change_time = now;
        changes_detected_++;
    }

    total_scans_++;
}

void ChangeDetector::PatrolLoop() {
    std::cout << "ChangeDetector: Patrol loop started, scanning " << chunks_.size() << " chunks\n";

    while (running_) {
        uint64_t now = GetMilliseconds();
        size_t scanned_this_cycle = 0;

        // Get current ranges
        size_t vis_start, vis_end, heat_start, heat_end;
        {
            std::lock_guard<std::mutex> lock(range_mutex_);
            vis_start = visible_start_;
            vis_end = visible_end_;
            heat_start = heatmap_start_;
            heat_end = heatmap_end_;
        }

        // Dynamic limit based on heat map size (with some headroom)
        size_t heat_map_size = (heat_end > heat_start) ? (heat_end - heat_start) : 0;
        size_t MAX_CHUNKS_PER_CYCLE = std::max((size_t)1000, heat_map_size + 100);

        // Debug output every 100 cycles (disabled for production)
        // static int cycle_count = 0;
        // if (++cycle_count % 100 == 0) {
        //     std::cout << "ChangeDetector: Scanned " << total_scans_.load()
        //               << " chunks, detected " << changes_detected_.load() << " changes\n";
        //     std::cout << "  Ranges: vis[" << vis_start << "-" << vis_end << "] "
        //               << "heat[" << heat_start << "-" << heat_end << "] "
        //               << "(span=" << (heat_end - heat_start) << " chunks)\n";
        // }

        // Priority 1: Visible chunks (already in cache from rendering)
        // Scan every 50ms (20 Hz)
        for (size_t i = vis_start; i < vis_end && scanned_this_cycle < MAX_CHUNKS_PER_CYCLE; i++) {
            if (now - chunks_[i].last_scan_time > 50) {
                ScanChunk(i);
                scanned_this_cycle++;
            }
        }

        // Priority 2: Heat map visible chunks
        // Scan every 250ms (4 Hz)
        for (size_t i = heat_start; i < heat_end && scanned_this_cycle < MAX_CHUNKS_PER_CYCLE; i++) {
            // Skip if already scanned as visible
            if (i >= vis_start && i < vis_end) {
                continue;
            }

            if (now - chunks_[i].last_scan_time > 250) {
                ScanChunk(i);
                scanned_this_cycle++;
            }
        }

        // Priority 3: Background random sampling (10 random chunks)
        // This detects external file modifications
        if (scanned_this_cycle < MAX_CHUNKS_PER_CYCLE) {
            for (int i = 0; i < 10 && scanned_this_cycle < MAX_CHUNKS_PER_CYCLE; i++) {
                // Simple pseudo-random scatter
                size_t random_idx = (background_scan_pos_ * 6133 + i * 997) % chunks_.size();
                ScanChunk(random_idx);
                scanned_this_cycle++;
            }
            background_scan_pos_++;
        }

        // Debug: show scan breakdown occasionally (disabled for production)
        // if (cycle_count % 100 == 0) {
        //     std::cout << "  This cycle: P1=" << priority1_scans
        //               << " P2=" << priority2_scans
        //               << " P3=" << priority3_scans << "\n";
        // }

        // Sleep for configured interval
        std::this_thread::sleep_for(std::chrono::milliseconds(scan_interval_ms_.load()));
    }
}

} // namespace Haywire