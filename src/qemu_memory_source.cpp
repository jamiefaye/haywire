#include "qemu_memory_source.h"
#include "qemu_connection.h"
#include "crunched_memory_reader.h"
#include "guest_agent.h"
#include <algorithm>
#include <cstring>

namespace Haywire {

QemuMemorySource::QemuMemorySource(QemuConnection* qemu)
    : qemu_(qemu),
      crunchedReader_(nullptr),
      guestAgent_(nullptr),
      processMode_(false),
      processPid_(0) {
}

bool QemuMemorySource::ReadMemory(uint64_t address, uint8_t* buffer, size_t size) {
    if (!qemu_ || !qemu_->IsConnected()) {
        return false;
    }

    if (processMode_ && crunchedReader_) {
        // In process mode, use crunched memory reader for VA to PA translation
        std::vector<uint8_t> tempBuffer;
        size_t bytesRead = crunchedReader_->ReadCrunchedMemory(address, size, tempBuffer);
        if (bytesRead > 0 && !tempBuffer.empty()) {
            std::memcpy(buffer, tempBuffer.data(), std::min(size, tempBuffer.size()));
            return true;
        }
        return false;
    } else {
        // Direct physical memory read
        std::vector<uint8_t> tempBuffer;
        if (qemu_->ReadMemory(address, size, tempBuffer)) {
            std::memcpy(buffer, tempBuffer.data(), std::min(size, tempBuffer.size()));
            return true;
        }
        return false;
    }
}

uint64_t QemuMemorySource::GetMemorySize() const {
    // Return a large default size for VM memory
    // This could be improved by querying actual VM memory size
    return 8ULL * 1024 * 1024 * 1024; // 8GB default
}

bool QemuMemorySource::IsValidAddress(uint64_t address, size_t size) const {
    // For VM memory, we generally allow any address and let the read fail if invalid
    // This could be improved by checking against actual memory regions
    return true;
}

std::string QemuMemorySource::GetSourceName() const {
    if (!qemu_) {
        return "QEMU (disconnected)";
    }

    std::string name = "QEMU";
    if (qemu_->IsConnected()) {
        name += " (connected)";
        if (processMode_) {
            name += " PID: " + std::to_string(processPid_);
        }
    } else {
        name += " (disconnected)";
    }
    return name;
}

std::vector<QemuMemorySource::MemoryRegion> QemuMemorySource::GetMemoryRegions() const {
    return cachedRegions_;
}

bool QemuMemorySource::TranslateAddress(uint64_t virtualAddress, uint64_t& physicalAddress) {
    if (processMode_ && crunchedReader_) {
        // Use crunched reader's translation
        // For now, just return the address as-is since crunched reader handles it internally
        physicalAddress = virtualAddress;
        return true;
    }

    // No translation in physical mode
    physicalAddress = virtualAddress;
    return true;
}

bool QemuMemorySource::IsAvailable() const {
    return qemu_ && qemu_->IsConnected();
}

void QemuMemorySource::SetProcessMode(bool enabled, int pid) {
    processMode_ = enabled;
    processPid_ = pid;
}

void QemuMemorySource::SetCrunchedReader(CrunchedMemoryReader* reader) {
    crunchedReader_ = reader;
}

void QemuMemorySource::SetGuestAgent(GuestAgent* agent) {
    guestAgent_ = agent;
}

void QemuMemorySource::UpdateMemoryRegions() {
    cachedRegions_.clear();

    if (!guestAgent_ || processPid_ <= 0) {
        return;
    }

    // Get memory regions for the current process
    std::vector<GuestMemoryRegion> guestRegions;
    if (guestAgent_->GetMemoryMap(processPid_, guestRegions)) {
        for (const auto& gr : guestRegions) {
            MemoryRegion region;
            region.start = gr.start;
            region.end = gr.end;
            region.name = gr.name;
            region.permissions = gr.permissions;
            cachedRegions_.push_back(region);
        }
    }
}

} // namespace Haywire