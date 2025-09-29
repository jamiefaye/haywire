/*
 * memory_utils.cpp - Centralized memory reading utilities
 */

#include "memory_utils.h"
#include "kernel_memory_reader.h"
#include "qemu_connection.h"
#include "memory_mapper.h"
#include "crunched_memory_reader.h"
#include <cstdio>
#include <cstring>

namespace Haywire {

bool MemoryUtils::ReadMemoryWithFallback(
    const TypedAddress& address,
    size_t size,
    std::vector<uint8_t>& buffer,
    KernelMemoryReader* kernelReader,
    QemuConnection* qemu,
    MemoryMapper* mapper,
    CrunchedMemoryReader* crunchedReader,
    int currentPid) {

    // Handle CRUNCHED addresses with crunched reader
    if (address.space == AddressSpace::CRUNCHED && crunchedReader && currentPid > 0) {
        buffer.resize(size);
        size_t bytesRead = crunchedReader->ReadCrunchedMemory(address.value, size, buffer);
        if (bytesRead > 0) {
            return true;
        }
    }

    // Handle VIRTUAL addresses with kernel reader
    if (address.space == AddressSpace::VIRTUAL && kernelReader && currentPid > 0) {
        return kernelReader->ReadVirtualMemory(currentPid, address.value, size, buffer);
    }

    // Handle PHYSICAL and SHARED addresses
    uint64_t physicalAddr = 0;

    switch (address.space) {
        case AddressSpace::SHARED:
            // SHARED is a file offset - convert to physical
            if (mapper) {
                // Find which region contains this offset
                const auto& regions = mapper->GetRegions();
                uint64_t accumOffset = 0;
                for (const auto& region : regions) {
                    if (address.value >= accumOffset &&
                        address.value < accumOffset + region.size) {
                        uint64_t offsetInRegion = address.value - accumOffset;
                        physicalAddr = region.gpa_start + offsetInRegion;
                        break;
                    }
                    accumOffset += region.size;
                }
            }
            break;

        case AddressSpace::PHYSICAL:
            physicalAddr = address.value;
            break;

        default:
            // Can't handle other spaces
            return false;
    }

    // Try kernel reader for physical addresses
    if (kernelReader && physicalAddr != 0) {
        if (kernelReader->ReadPhysicalMemory(physicalAddr, size, buffer)) {
            return true;
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
    KernelMemoryReader* kernelReader,
    QemuConnection* qemu,
    MemoryMapper* mapper) {

    // Quick check without actually reading
    switch (address.space) {
        case AddressSpace::SHARED:
            if (kernelReader) {
                // Check if it's within mapped memory size
                size_t memSize = kernelReader->GetMemorySize();
                return address.value < memSize;
            }
            break;

        case AddressSpace::PHYSICAL:
            if (kernelReader) {
                // Check if physical address is in guest RAM range
                const uint64_t GUEST_RAM_BASE = 0x40000000;
                size_t memSize = kernelReader->GetMemorySize();
                return address.value >= GUEST_RAM_BASE &&
                       address.value < GUEST_RAM_BASE + memSize;
            }
            // QMP can read any physical address as fallback
            return qemu != nullptr;

        case AddressSpace::VIRTUAL:
            // Need kernel reader and a PID to verify virtual addresses
            return kernelReader != nullptr;

        case AddressSpace::CRUNCHED:
            // Would need crunched reader to verify
            return false;

        default:
            break;
    }

    return false;
}

}  // namespace Haywire