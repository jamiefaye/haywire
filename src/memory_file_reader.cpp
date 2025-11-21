#include "memory_file_reader.h"
#include <iostream>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/stat.h>
#endif

namespace Haywire {

// Helper function to get default memory file path for the platform
static std::string GetDefaultMemoryFilePath() {
#ifdef _WIN32
    // On Windows, use %TEMP%\haywire-vm-mem
    char tempPath[MAX_PATH];
    DWORD result = GetTempPathA(MAX_PATH, tempPath);
    if (result == 0 || result > MAX_PATH) {
        return "C:\\Temp\\haywire-vm-mem";  // Fallback
    }
    std::string path = tempPath;
    if (!path.empty() && path.back() != '\\') {
        path += '\\';
    }
    return path + "haywire-vm-mem";
#else
    // On POSIX systems (macOS, Linux), use /tmp/haywire-vm-mem
    return "/tmp/haywire-vm-mem";
#endif
}

MemoryFileReader::MemoryFileReader()
    : memoryBase(nullptr), memorySize(0)
#ifdef _WIN32
    , hFile(INVALID_HANDLE_VALUE), hMapping(NULL)
#else
    , memoryFd(-1)
#endif
{
}

MemoryFileReader::~MemoryFileReader() {
    Cleanup();
}

bool MemoryFileReader::Initialize(const std::string& memoryFilePath) {
    if (memoryBase) {
        std::cerr << "MemoryFileReader already initialized\n";
        return true;
    }

    // Use provided path, or get default for this platform
    filePath = memoryFilePath.empty() ? GetDefaultMemoryFilePath() : memoryFilePath;

#ifdef _WIN32
    // Windows implementation using CreateFileMapping/MapViewOfFile

    // Open the memory file for reading
    hFile = CreateFileA(
        filePath.c_str(),
        GENERIC_READ,           // Read access
        FILE_SHARE_READ | FILE_SHARE_WRITE,  // Allow QEMU to continue writing
        NULL,                   // Default security
        OPEN_EXISTING,          // File must exist
        FILE_ATTRIBUTE_NORMAL,  // Normal file
        NULL                    // No template
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open memory file: " << filePath << " (Error: " << GetLastError() << ")\n";
        return false;
    }

    // Get file size
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        std::cerr << "Failed to get file size (Error: " << GetLastError() << ")\n";
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
        return false;
    }

    memorySize = static_cast<size_t>(fileSize.QuadPart);
    std::cout << "Memory file size: " << (memorySize / (1024*1024)) << " MB\n";

    // Create file mapping object
    hMapping = CreateFileMappingA(
        hFile,
        NULL,                   // Default security
        PAGE_READONLY,          // Read-only access
        0,                      // High-order DWORD of size (0 = use file size)
        0,                      // Low-order DWORD of size (0 = use file size)
        NULL                    // No name
    );

    if (hMapping == NULL) {
        std::cerr << "Failed to create file mapping (Error: " << GetLastError() << ")\n";
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
        return false;
    }

    // Map view of file into address space
    memoryBase = (uint8_t*)MapViewOfFile(
        hMapping,
        FILE_MAP_READ,          // Read-only access
        0,                      // High-order DWORD of offset
        0,                      // Low-order DWORD of offset
        0                       // Map entire file
    );

    if (memoryBase == NULL) {
        std::cerr << "Failed to map view of file (Error: " << GetLastError() << ")\n";
        CloseHandle(hMapping);
        CloseHandle(hFile);
        hMapping = NULL;
        hFile = INVALID_HANDLE_VALUE;
        return false;
    }

    std::cout << "Successfully mapped memory file at " << (void*)memoryBase << "\n";
    return true;

#else
    // POSIX implementation using mmap

    // Open the memory file
    memoryFd = open(filePath.c_str(), O_RDONLY);
    if (memoryFd < 0) {
        std::cerr << "Failed to open memory file: " << filePath << "\n";
        return false;
    }

    // Get file size
    struct stat st;
    if (fstat(memoryFd, &st) < 0) {
        std::cerr << "Failed to stat memory file\n";
        close(memoryFd);
        memoryFd = -1;
        return false;
    }

    memorySize = st.st_size;
    std::cout << "Memory file size: " << (memorySize / (1024*1024)) << " MB\n";

    // Memory map the file (MAP_SHARED to see live updates from QEMU)
    memoryBase = (uint8_t*)mmap(nullptr, memorySize, PROT_READ, MAP_SHARED, memoryFd, 0);
    if (memoryBase == MAP_FAILED) {
        std::cerr << "Failed to mmap memory file\n";
        close(memoryFd);
        memoryFd = -1;
        memoryBase = nullptr;
        return false;
    }

    std::cout << "Successfully mapped memory file at " << (void*)memoryBase << "\n";
    return true;
#endif
}

void MemoryFileReader::Cleanup() {
#ifdef _WIN32
    // Windows cleanup
    if (memoryBase) {
        UnmapViewOfFile(memoryBase);
        memoryBase = nullptr;
    }

    if (hMapping != NULL) {
        CloseHandle(hMapping);
        hMapping = NULL;
    }

    if (hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
    }
#else
    // POSIX cleanup
    if (memoryBase && memoryBase != MAP_FAILED) {
        munmap(memoryBase, memorySize);
        memoryBase = nullptr;
    }

    if (memoryFd >= 0) {
        close(memoryFd);
        memoryFd = -1;
    }
#endif

    memorySize = 0;
}

} // namespace Haywire