#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace Haywire {

// Types of binary files we can load
enum class BinaryType {
    UNKNOWN,
    ELF_EXECUTABLE,      // Linux/Unix executable
    ELF_SHARED_OBJECT,   // .so library
    ELF_CORE_DUMP,       // Core dump
    MACH_O_EXECUTABLE,   // macOS executable
    MACH_O_DYLIB,        // .dylib library
    MACH_O_CORE,         // macOS core dump
    PE_EXECUTABLE,       // Windows .exe
    PE_DLL,              // Windows .dll
    MINIDUMP,            // Windows minidump
    RAW_BINARY,          // Unknown format, treat as raw
};

// Memory segment from a binary file
struct BinarySegment {
    std::string name;        // Segment name (.text, .data, etc.)
    uint64_t virtual_addr;   // Virtual address where it would be loaded
    uint64_t file_offset;    // Offset in the file
    uint64_t file_size;      // Size in file (might be 0 for .bss)
    uint64_t memory_size;    // Size in memory when loaded
    uint32_t permissions;    // Read/Write/Execute flags
    std::vector<uint8_t> data; // Actual data (if loaded)

    bool is_code() const { return permissions & 0x1; }
    bool is_writable() const { return permissions & 0x2; }
    bool is_readable() const { return permissions & 0x4; }
};

// Symbol information (if available)
struct BinarySymbol {
    std::string name;
    uint64_t address;
    uint64_t size;
    std::string type;  // "FUNC", "OBJECT", etc.
};

// Metadata about the binary
struct BinaryInfo {
    BinaryType type;
    std::string architecture;  // "x86_64", "arm64", etc.
    std::string os;            // "linux", "macos", "windows"
    uint64_t entry_point;      // Entry point address
    bool is_64bit;
    bool is_little_endian;
    std::vector<std::string> needed_libraries; // Dependencies

    // For core dumps
    uint32_t pid;              // Process ID (if core dump)
    std::string command_line;  // Command that crashed
    uint32_t signal;           // Signal that caused dump
};

// Main loader class
class BinaryLoader {
public:
    BinaryLoader();
    ~BinaryLoader();

    // Load a binary file
    bool LoadFile(const std::string& path);
    bool LoadFromMemory(const uint8_t* data, size_t size);

    // Get information about the loaded binary
    const BinaryInfo& GetInfo() const { return info; }
    const std::vector<BinarySegment>& GetSegments() const { return segments; }
    const std::vector<BinarySymbol>& GetSymbols() const { return symbols; }
    const std::vector<uint8_t>& GetRawData() const { return rawData; }

    // Find specific segments
    const BinarySegment* FindSegment(const std::string& name) const;
    const BinarySegment* FindSegmentByAddress(uint64_t addr) const;

    // Find symbols
    const BinarySymbol* FindSymbol(const std::string& name) const;
    const BinarySymbol* FindSymbolByAddress(uint64_t addr) const;

    // For visualization
    std::vector<uint8_t> GetFlattenedMemory() const; // All segments as contiguous memory
    std::vector<uint8_t> GetMemoryLayout(uint64_t size) const; // With proper VA spacing

    // Detect file type
    static BinaryType DetectType(const uint8_t* data, size_t size);

    // Check if using memory mapping
    bool IsMemoryMapped() const { return useMmap; }
    const std::string& GetFilePath() const { return filePath; }
    size_t GetFileSize() const { return fileSize; }

private:
    BinaryInfo info;
    std::vector<BinarySegment> segments;
    std::vector<BinarySymbol> symbols;
    std::vector<uint8_t> rawData;

    // For memory-mapped files
    bool useMmap = false;
    std::string filePath;
    size_t fileSize = 0;

    // Parsers for different formats
    bool ParseELF();
    bool ParseELFExecutable();
    bool ParseELFCore();
    bool ParseMachO();
    bool ParsePE();
    bool ParseMinidump();

    // Helper to read various data types
    template<typename T>
    T Read(size_t offset) const;
    std::string ReadString(size_t offset, size_t maxLen = 256) const;
};

}