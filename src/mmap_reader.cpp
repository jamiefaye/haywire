#include "mmap_reader.h"
#include "qemu_connection.h"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

namespace Haywire {

MMapReader::MMapReader() : mappedData(nullptr), mappedSize(0), fd(-1) {
}

MMapReader::~MMapReader() {
    Unmap();
}

bool MMapReader::DumpAndMap(QemuConnection& qemu, uint64_t address, size_t size) {
    // Use QMP to dump memory to a file
    std::string dumpPath = "/tmp/haywire_mem.dump";
    
    nlohmann::json cmd = {
        {"execute", "pmemsave"},
        {"arguments", {
            {"val", address},
            {"size", size},
            {"filename", dumpPath}
        }}
    };
    
    nlohmann::json response;
    if (!qemu.SendQMPCommand(cmd, response)) {
        std::cerr << "Failed to dump memory via QMP\n";
        return false;
    }
    
    // Check for error in response
    if (response.contains("error")) {
        std::cerr << "QMP error: " << response["error"].dump() << "\n";
        return false;
    }
    
    // Wait a bit for dump to complete
    usleep(100000); // 100ms
    
    // Now mmap the dump file
    return MapFile(dumpPath, size);
}

bool MMapReader::MapFile(const std::string& path, size_t size) {
    Unmap();
    
    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open " << path << ": " << strerror(errno) << "\n";
        return false;
    }
    
    // Get actual file size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        fd = -1;
        return false;
    }
    
    size_t fileSize = st.st_size;
    if (size > 0 && size < fileSize) {
        fileSize = size;
    }
    
    mappedData = (uint8_t*)mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mappedData == MAP_FAILED) {
        std::cerr << "mmap failed: " << strerror(errno) << "\n";
        close(fd);
        fd = -1;
        mappedData = nullptr;
        return false;
    }
    
    mappedSize = fileSize;
    // std::cerr << "Mapped " << mappedSize << " bytes from " << path << "\n";
    return true;
}

bool MMapReader::Read(uint64_t offset, size_t size, std::vector<uint8_t>& buffer) {
    if (!mappedData || offset >= mappedSize) {
        return false;
    }
    
    size_t available = mappedSize - offset;
    size_t toRead = std::min(size, available);
    
    buffer.resize(toRead);
    memcpy(buffer.data(), mappedData + offset, toRead);
    
    return true;
}

void MMapReader::Unmap() {
    if (mappedData && mappedSize > 0) {
        munmap(mappedData, mappedSize);
        mappedData = nullptr;
        mappedSize = 0;
    }
    
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

}