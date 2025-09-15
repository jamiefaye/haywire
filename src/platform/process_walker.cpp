#include "platform/process_walker.h"
#include "platform/linux/linux_process_walker.h"
#include "platform/windows/windows_process_walker.h"
#include "memory_backend.h"
#include <cstring>
#include <vector>

namespace Haywire {

std::string ProcessWalker::ReadString(uint64_t addr, size_t maxLen) {
    std::vector<uint8_t> buffer;
    if (!memory->Read(addr, maxLen, buffer)) {
        return "";
    }

    // Find null terminator
    size_t len = 0;
    for (size_t i = 0; i < buffer.size(); i++) {
        if (buffer[i] == 0) {
            len = i;
            break;
        }
    }

    if (len == 0) {
        len = buffer.size();
    }

    return std::string(reinterpret_cast<char*>(buffer.data()), len);
}

std::string ProcessWalker::ReadCString(uint64_t addr, size_t maxLen) {
    std::string result;
    result.reserve(maxLen);

    // Read one byte at a time until null or maxLen
    for (size_t i = 0; i < maxLen; i++) {
        std::vector<uint8_t> byteBuf;
        if (!memory->Read(addr + i, 1, byteBuf) || byteBuf.empty()) {
            break;
        }

        if (byteBuf[0] == 0) {
            break;  // Null terminator
        }

        result += static_cast<char>(byteBuf[0]);
    }

    return result;
}

std::unique_ptr<ProcessWalker> CreateProcessWalker(MemoryBackend* backend,
                                                   const std::string& os) {
    if (os == "linux" || os == "Linux") {
        return std::make_unique<LinuxProcessWalker>(backend);
    } else if (os == "windows" || os == "Windows") {
        // Windows implementation stub - not yet implemented
        // return std::make_unique<WindowsProcessWalker>(backend);
        return nullptr;
    }

    // Default to Linux for now
    return std::make_unique<LinuxProcessWalker>(backend);
}

}