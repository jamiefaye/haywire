/*
 * memory_utils.h - Centralized memory reading utilities
 * 
 * Provides common memory reading functions that handle fallback
 * between different memory access methods.
 */

#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include "address_parser.h"

namespace Haywire {

// Forward declarations
class BeaconReader;
class QemuConnection;
class MemoryMapper;
class CrunchedMemoryReader;

class MemoryUtils {
public:
    // Read memory with automatic fallback between different methods
    // Tries in order:
    // 1. Direct beacon reader (mmap'd file) for SHARED addresses
    // 2. CrunchedReader for CRUNCHED/VIRTUAL addresses (if available)
    // 3. QemuConnection (which itself tries MemoryBackend then QMP)
    static bool ReadMemoryWithFallback(
        const TypedAddress& address,
        size_t size,
        std::vector<uint8_t>& buffer,
        BeaconReader* beacon,
        QemuConnection* qemu,
        MemoryMapper* mapper,
        CrunchedMemoryReader* crunchedReader = nullptr,
        int currentPid = -1
    );
    
    // Convert address to appropriate space for reading
    // Returns the best address space for the available readers
    static TypedAddress ConvertForReading(
        const TypedAddress& address,
        MemoryMapper* mapper,
        bool preferShared = true
    );
    
    // Check if an address is readable with current resources
    static bool IsAddressReadable(
        const TypedAddress& address,
        BeaconReader* beacon,
        QemuConnection* qemu,
        MemoryMapper* mapper
    );
};

}  // namespace Haywire