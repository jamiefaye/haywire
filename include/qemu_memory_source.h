#pragma once

#include "memory_data_source.h"
#include <memory>

namespace Haywire {

class QemuConnection;
class CrunchedMemoryReader;
class GuestAgent;

// Memory data source that reads from QEMU VM
class QemuMemorySource : public MemoryDataSource {
public:
    QemuMemorySource(QemuConnection* qemu);

    // MemoryDataSource interface
    bool ReadMemory(uint64_t address, uint8_t* buffer, size_t size) override;
    uint64_t GetMemorySize() const override;
    bool IsValidAddress(uint64_t address, size_t size) const override;
    std::string GetSourceName() const override;
    std::vector<MemoryRegion> GetMemoryRegions() const override;
    bool TranslateAddress(uint64_t virtualAddress, uint64_t& physicalAddress) override;
    bool IsAvailable() const override;

    // Set process mode (for VA to PA translation)
    void SetProcessMode(bool enabled, int pid = 0);

    // Set crunched memory reader for VA mode
    void SetCrunchedReader(CrunchedMemoryReader* reader);

    // Set guest agent for memory regions
    void SetGuestAgent(GuestAgent* agent);

    // Update memory regions from guest agent
    void UpdateMemoryRegions();

private:
    QemuConnection* qemu_;
    CrunchedMemoryReader* crunchedReader_;
    GuestAgent* guestAgent_;
    bool processMode_;
    int processPid_;
    std::vector<MemoryRegion> cachedRegions_;
};

} // namespace Haywire