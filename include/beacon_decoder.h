#ifndef BEACON_DECODER_H
#define BEACON_DECODER_H

#include <stdint.h>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstring>

namespace Haywire {

// Section entry - still used by BeaconReader for conversion
struct SectionEntry {
    uint32_t type;  // ENTRY_SECTION
    uint32_t pid;
    uint64_t va_start;
    uint64_t va_end;
    uint32_t perms;
    char path[64];
} __attribute__((packed));

#ifdef USE_OLD_BEACON_PROTOCOL
// Match the encoder's constants
#define PAGE_SIZE 4096
#define BEACON_MAGIC1 0x3142FACE
#define BEACON_MAGIC2 0xCAFEBABE
#define HEADER_SIZE 64
#define DATA_AREA_SIZE (PAGE_SIZE - HEADER_SIZE)

// Observer types
enum ObserverType {
    OBSERVER_MASTER = 0,
    OBSERVER_PID_SCANNER = 1,
    OBSERVER_CAMERA = 2,
    OBSERVER_CAMERA_CONTROL = 3,  // h2g control page
};

// Entry types
enum EntryType {
    ENTRY_PID = 0,
    ENTRY_SECTION = 1,
    ENTRY_PTE = 2,
    ENTRY_CAMERA_HEADER = 3,
};

// Beacon page header (64 bytes) - must match encoder exactly
struct BeaconPageHeader {
    uint32_t magic1;
    uint32_t magic2;
    uint32_t observer_type;
    uint32_t observer_id;
    
    uint32_t generation;
    uint32_t write_seq;
    uint32_t page_seq;
    uint32_t reserved1;
    
    uint32_t entry_count;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t continuation;
    
    uint64_t timestamp_ns;
    uint64_t reserved2;
} __attribute__((packed));

// PID entry for PID scanner
struct PIDEntry {
    uint32_t type;  // ENTRY_PID
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    uint64_t rss_kb;
    char comm[16];
    char state;
    uint8_t padding[3];
} __attribute__((packed));

// PTE entry  
struct PTEEntry {
    uint32_t type;  // ENTRY_PTE
    uint32_t reserved;
    uint64_t va;
    uint64_t pa;
    uint32_t flags;
    uint32_t reserved2;
} __attribute__((packed));

// Camera header entry
struct CameraHeaderEntry {
    uint32_t type;  // ENTRY_CAMERA_HEADER
    uint32_t pid;
    uint32_t section_count;
    uint32_t pte_count;
    uint64_t capture_time;
} __attribute__((packed));
#endif // USE_OLD_BEACON_PROTOCOL

// Decoder class - DEPRECATED: Uses old beacon protocol
class BeaconDecoder {
public:
    BeaconDecoder();
    ~BeaconDecoder();
    
    // Scan memory for beacon pages
    bool ScanMemory(void* memBase, size_t memSize);
    
#ifdef USE_OLD_BEACON_PROTOCOL
    // Get PID entries from most recent scan
    const std::vector<PIDEntry>& GetPIDEntries() const { return pidEntries; }
    
    // Get sections for a specific PID from camera data
    std::vector<SectionEntry> GetSectionsForPID(uint32_t pid) const;
    
    // Get PTEs for a specific PID from camera data
    std::vector<PTEEntry> GetPTEsForPID(uint32_t pid) const;
#endif
    
    // Check if we have recent data
    bool HasRecentData(uint64_t maxAgeNs = 5000000000ULL) const; // 5 seconds default
    
    // Get PTEs as a map for a specific camera (1 or 2)
    std::unordered_map<uint64_t, uint64_t> GetCameraPTEs(int camera) const;
    
    // Get target PID for a specific camera
    int GetCameraTargetPID(int camera) const;
    
private:
#ifdef USE_OLD_BEACON_PROTOCOL
    // Decode a single page
    bool DecodePage(const uint8_t* pageData);
    
    // Decode specific entry types
    void DecodePIDEntry(const uint8_t* data);
    void DecodeSectionEntry(const uint8_t* data);
    void DecodePTEEntry(const uint8_t* data);
    void DecodeCameraHeader(const uint8_t* data);
    
    // Storage for decoded data
    std::vector<PIDEntry> pidEntries;
    
    // For camera data: track sections with their sequence numbers
    // Key is a hash of the section (start_va + size), value is section + seq
    std::map<uint64_t, std::pair<SectionEntry, uint32_t>> sectionMap;
    std::vector<PTEEntry> pteEntries;  // PTEs follow section sequence
    std::vector<CameraHeaderEntry> cameraHeaders;
#endif
    
    // Track most recent data
    uint64_t lastTimestamp;
    uint32_t lastGeneration;
    uint32_t lastWriteSeq;
    uint32_t currentPageSeq;  // Sequence number of current page being processed
    
    // Current camera context (for associating sections/PTEs with PIDs)
    uint32_t currentCameraPID;
    uint32_t lastCameraPID;  // To detect when camera focus changes
};

} // namespace Haywire

#endif // BEACON_DECODER_H