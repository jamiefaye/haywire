#include "mapped_file_memory_source.h"
#include "platform_compat.h"
#include <iostream>
#include <cstring>
#include <sys/stat.h>

namespace Haywire {

MappedFileMemorySource::MappedFileMemorySource() {
}

MappedFileMemorySource::~MappedFileMemorySource() {
    Close();
}

bool MappedFileMemorySource::OpenFile(const std::string& filename) {
    // Close any existing mapping
    Close();

#ifdef _WIN32
    // Windows implementation using CreateFileMapping
    file_handle_ = CreateFileA(
        filename.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (file_handle_ == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open file: " << filename << " - Error: " << GetLastError() << "\n";
        return false;
    }

    // Get file size
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(file_handle_, &fileSize)) {
        std::cerr << "Failed to get file size - Error: " << GetLastError() << "\n";
        CloseHandle(file_handle_);
        file_handle_ = nullptr;
        return false;
    }
    file_size_ = static_cast<size_t>(fileSize.QuadPart);

    // Create file mapping
    mapping_handle_ = CreateFileMappingA(
        file_handle_,
        nullptr,
        PAGE_READONLY,
        0, 0,  // Use entire file
        nullptr
    );

    if (!mapping_handle_) {
        std::cerr << "Failed to create file mapping - Error: " << GetLastError() << "\n";
        CloseHandle(file_handle_);
        file_handle_ = nullptr;
        return false;
    }

    // Map view of file
    mapped_data_ = MapViewOfFile(
        mapping_handle_,
        FILE_MAP_READ,
        0, 0,  // Offset
        0      // Map entire file
    );

    if (!mapped_data_) {
        std::cerr << "Failed to map view of file - Error: " << GetLastError() << "\n";
        CloseHandle(mapping_handle_);
        CloseHandle(file_handle_);
        mapping_handle_ = nullptr;
        file_handle_ = nullptr;
        return false;
    }

#else
    // Unix implementation using mmap
    fd_ = open(filename.c_str(), O_RDONLY);
    if (fd_ == -1) {
        std::cerr << "Failed to open file: " << filename << " - " << strerror(errno) << "\n";
        return false;
    }

    // Get file size
    struct stat st;
    if (fstat(fd_, &st) == -1) {
        std::cerr << "Failed to stat file: " << strerror(errno) << "\n";
        close(fd_);
        fd_ = -1;
        return false;
    }
    file_size_ = st.st_size;

    // Memory map the file
    mapped_data_ = mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped_data_ == MAP_FAILED) {
        std::cerr << "Failed to mmap file: " << strerror(errno) << "\n";
        close(fd_);
        fd_ = -1;
        mapped_data_ = nullptr;
        return false;
    }

    // Advise kernel about our access pattern
    madvise(mapped_data_, file_size_, MADV_RANDOM);
#endif

    filename_ = filename;
    std::cout << "Memory-mapped file: " << filename
              << " (" << (file_size_ / (1024*1024)) << " MB)\n";

    return true;
}

void MappedFileMemorySource::Close() {
#ifdef _WIN32
    if (mapped_data_) {
        UnmapViewOfFile(mapped_data_);
        mapped_data_ = nullptr;
    }
    if (mapping_handle_) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
    }
    if (file_handle_) {
        CloseHandle(file_handle_);
        file_handle_ = nullptr;
    }
#else
    if (mapped_data_ && mapped_data_ != MAP_FAILED) {
        munmap(mapped_data_, file_size_);
        mapped_data_ = nullptr;
    }

    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
#endif

    file_size_ = 0;
    filename_.clear();
}

bool MappedFileMemorySource::ReadMemory(uint64_t address, uint8_t* buffer, size_t size) {
    if (!mapped_data_ || !buffer) {
        return false;
    }

    // Check bounds
    if (address >= file_size_ || address + size > file_size_) {
        // Clear buffer for out-of-bounds reads
        memset(buffer, 0, size);
        return false;
    }

    // Copy data from mapped memory
    const uint8_t* src = static_cast<const uint8_t*>(mapped_data_) + address;
    memcpy(buffer, src, size);

    return true;
}

uint64_t MappedFileMemorySource::GetMemorySize() const {
    return file_size_;
}

bool MappedFileMemorySource::IsValidAddress(uint64_t address, size_t size) const {
    return mapped_data_ && address < file_size_ && address + size <= file_size_;
}

std::string MappedFileMemorySource::GetSourceName() const {
    return filename_;
}

std::vector<MemoryDataSource::MemoryRegion> MappedFileMemorySource::GetMemoryRegions() const {
    std::vector<MemoryDataSource::MemoryRegion> regions;
    if (mapped_data_ && file_size_ > 0) {
        MemoryRegion region;
        region.start = 0;
        region.end = file_size_;
        region.name = filename_;
        region.permissions = "r--";
        regions.push_back(region);
    }
    return regions;
}

bool MappedFileMemorySource::IsAvailable() const {
    return mapped_data_ != nullptr && mapped_data_ != MAP_FAILED;
}

} // namespace Haywire