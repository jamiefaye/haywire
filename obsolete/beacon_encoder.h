/*
 * beacon_encoder.h - Simplified beacon encoder for companion programs
 */

#ifndef BEACON_ENCODER_H
#define BEACON_ENCODER_H

#include <stdint.h>
#include <string.h>
#include <time.h>

// Observer types
#define OBSERVER_PID_SCANNER    1
#define OBSERVER_CAMERA         2
#define OBSERVER_CAMERA_CONTROL 3

// Magic numbers
#define BEACON_MAGIC1 0x3142FACE
#define BEACON_MAGIC2 0xCAFEBABE

// Entry types
#define ENTRY_PID            1
#define ENTRY_SECTION        2
#define ENTRY_PTE            3
#define ENTRY_CAMERA_HEADER  4

// Beacon page header
typedef struct {
    uint32_t magic1;
    uint32_t magic2;
    uint32_t observer_type;
    uint32_t generation;
    uint32_t write_seq;
    uint64_t timestamp_ns;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t entry_count;
} BeaconPageHeader;

// Beacon encoder state
typedef struct {
    uint32_t observer_type;
    uint32_t max_entries;
    void* mem_base;
    size_t mem_size;
    uint32_t current_page;
    uint32_t current_offset;
    uint32_t generation;
} BeaconEncoder;

// Initialize encoder
static inline void beacon_encoder_init(BeaconEncoder* enc, uint32_t observer_type, 
                                       uint32_t max_entries, void* mem_base, size_t mem_size) {
    enc->observer_type = observer_type;
    enc->max_entries = max_entries;
    enc->mem_base = mem_base;
    enc->mem_size = mem_size;
    enc->current_page = 0;
    enc->current_offset = sizeof(BeaconPageHeader);
    enc->generation = 1;
}

// Add PID entry
static inline void beacon_encoder_add_pid(BeaconEncoder* enc, uint32_t pid, uint32_t ppid,
                                          uint64_t start_time, uint64_t utime, uint64_t stime,
                                          const char* comm, char state) {
    // Simplified - just track that we would add this
}

// Add section entry
static inline void beacon_encoder_add_section(BeaconEncoder* enc, uint32_t pid, 
                                              uint64_t vaddr, uint64_t size, 
                                              uint32_t flags, const char* path) {
    // Simplified - just track that we would add this
}

// Add PTE entry
static inline void beacon_encoder_add_pte(BeaconEncoder* enc, uint32_t pid,
                                          uint64_t vaddr, uint64_t paddr) {
    // Simplified - just track that we would add this
}

// Add camera header
static inline void beacon_encoder_add_camera_header(BeaconEncoder* enc, uint32_t camera_id,
                                                    uint32_t target_pid, uint32_t timestamp) {
    // Simplified - just track that we would add this
}

// Flush encoder
static inline void beacon_encoder_flush(BeaconEncoder* enc) {
    // Simplified - ensure data is written
}

#endif // BEACON_ENCODER_H