/*
 * memory_utils.cpp - Centralized memory reading utilities
 */

#include "memory_utils.h"
#include "beacon_reader.h"
#include "qemu_connection.h"
#include "memory_mapper.h"
#include "crunched_memory_reader.h"
#include <cstdio>

namespace Haywire {

bool MemoryUtils::ReadMemoryWithFallback(
    const TypedAddress& address,
    size_t size,
    std::vector<uint8_t>& buffer,
    BeaconReader* beacon,
    QemuConnection* qemu,
    MemoryMapper* mapper,
    CrunchedMemoryReader* crunchedReader,
    int currentPid) {
    
    // Handle CRUNCHED/VIRTUAL addresses with crunched reader
    if ((address.space == AddressSpace::CRUNCHED || 
         address.space == AddressSpace::VIRTUAL) && 
        crunchedReader && currentPid > 0) {
        
        uint64_t readAddr = address.value;
        
        // If VIRTUAL, would need to convert to crunched first
        // For now, assume crunched reader handles it
        
        buffer.resize(size);
        size_t bytesRead = crunchedReader->ReadCrunchedMemory(readAddr, size, buffer);
        if (bytesRead > 0) {
            return true;
        }
    }
    
    // Convert address to physical for other methods
    uint64_t physicalAddr = 0;
    uint64_t fileOffset = 0;
    bool haveFileOffset = false;
    
    switch (address.space) {
        case AddressSpace::SHARED:
            // SHARED is a direct file offset
            fileOffset = address.value;
            haveFileOffset = true;
            
            // Convert to physical for QMP fallback
            if (mapper) {
                // Find which region contains this offset
                const auto& regions = mapper->GetRegions();
                uint64_t accumOffset = 0;
                for (const auto& region : regions) {
                    if (fileOffset >= accumOffset && 
                        fileOffset < accumOffset + region.size) {
                        uint64_t offsetInRegion = fileOffset - accumOffset;
                        physicalAddr = region.gpa_start + offsetInRegion;
                        break;
                    }
                    accumOffset += region.size;
                }
            }
            break;
            
        case AddressSpace::PHYSICAL:
            physicalAddr = address.value;
            
            // Convert to file offset for beacon reader
            if (mapper) {
                int64_t offset = mapper->TranslateGPAToFileOffset(physicalAddr);
                if (offset >= 0) {
                    fileOffset = offset;
                    haveFileOffset = true;
                }
            }
            break;
            
        default:
            // Can't handle other spaces without conversion
            return false;
    }
    
    // Try beacon reader first (fastest - direct mmap)
    if (beacon && haveFileOffset) {
        // Check if within beacon reader's mapped size
        size_t memSize = beacon->GetMemorySize();
        if (fileOffset < memSize && fileOffset + size <= memSize) {
            const uint8_t* memPtr = beacon->GetMemoryPointer(fileOffset);
            if (memPtr) {
                buffer.resize(size);
                memcpy(buffer.data(), memPtr, size);
                return true;
            }
        }
    }
    
    // Fall back to QemuConnection (tries MemoryBackend, then QMP)
    if (qemu && physicalAddr != 0) {
        buffer.resize(size);
        if (qemu->ReadMemory(physicalAddr, size, buffer)) {
            return true;
        }
    }
    
    return false;
}

TypedAddress MemoryUtils::ConvertForReading(
    const TypedAddress& address,
    MemoryMapper* mapper,
    bool preferShared) {
    
    // If already in preferred format, return as-is
    if (preferShared && address.space == AddressSpace::SHARED) {
        return address;
    }
    if (!preferShared && address.space == AddressSpace::PHYSICAL) {
        return address;
    }
    
    // Convert between SHARED and PHYSICAL
    if (address.space == AddressSpace::PHYSICAL && preferShared && mapper) {
        int64_t offset = mapper->TranslateGPAToFileOffset(address.value);
        if (offset >= 0) {
            return TypedAddress::Shared(offset);
        }
    }
    
    if (address.space == AddressSpace::SHARED && !preferShared && mapper) {
        // Convert file offset to physical
        const auto& regions = mapper->GetRegions();
        uint64_t accumOffset = 0;
        for (const auto& region : regions) {
            if (address.value >= accumOffset && 
                address.value < accumOffset + region.size) {
                uint64_t offsetInRegion = address.value - accumOffset;
                return TypedAddress::Physical(region.gpa_start + offsetInRegion);
            }
            accumOffset += region.size;
        }
    }
    
    // Return unchanged if can't convert
    return address;
}

bool MemoryUtils::IsAddressReadable(
    const TypedAddress& address,
    BeaconReader* beacon,
    QemuConnection* qemu,
    MemoryMapper* mapper) {
    
    // Quick check without actually reading
    switch (address.space) {
        case AddressSpace::SHARED:
            if (beacon) {
                size_t memSize = beacon->GetMemorySize();
                return address.value < memSize;
            }
            break;
            
        case AddressSpace::PHYSICAL:
            if (mapper) {
                int64_t offset = mapper->TranslateGPAToFileOffset(address.value);
                if (offset >= 0) {
                    if (beacon) {
                        size_t memSize = beacon->GetMemorySize();
                        return (uint64_t)offset < memSize;
                    }
                    return true;  // Mapper says it's valid
                }
            }
            // QMP can read any physical address
            return qemu != nullptr;
            
        case AddressSpace::VIRTUAL:
        case AddressSpace::CRUNCHED:
            // Would need crunched reader to verify
            return false;
            
        default:
            break;
    }
    
    return false;
}

}  // namespace Haywire