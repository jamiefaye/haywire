// Mock MemoryBackend for testing platform abstraction
#include "memory_backend.h"
#include <vector>
#include <cstring>

namespace Haywire {

class MockMemoryBackend : public MemoryBackend {
public:
    bool IsAvailable() override { return false; }

    bool Read(uint64_t addr, size_t size, std::vector<uint8_t>& buffer) override {
        buffer.resize(size);
        memset(buffer.data(), 0, size);
        return false;  // Always fail for testing
    }

    bool Write(uint64_t addr, const std::vector<uint8_t>& data) override {
        return false;
    }

    std::string GetBackendName() const override {
        return "Mock";
    }
};

// Provide the missing implementation
bool MemoryBackend::IsAvailable() { return false; }
bool MemoryBackend::Read(uint64_t, size_t, std::vector<uint8_t>&) { return false; }
bool MemoryBackend::Write(uint64_t, const std::vector<uint8_t>&) { return false; }
std::string MemoryBackend::GetBackendName() const { return "Base"; }

}