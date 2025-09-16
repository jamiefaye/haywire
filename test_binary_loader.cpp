// Test program for binary loader
#include "binary_loader.h"
#include <iostream>
#include <iomanip>

using namespace Haywire;

void TestBinaryFile(const std::string& path) {
    std::cout << "\n=== Testing: " << path << " ===\n";

    BinaryLoader loader;
    if (!loader.LoadFile(path)) {
        std::cerr << "Failed to load file\n";
        return;
    }

    const auto& info = loader.GetInfo();
    const auto& segments = loader.GetSegments();

    std::cout << "Binary Type: ";
    switch (info.type) {
        case BinaryType::ELF_EXECUTABLE: std::cout << "ELF Executable\n"; break;
        case BinaryType::ELF_SHARED_OBJECT: std::cout << "ELF Shared Object\n"; break;
        case BinaryType::ELF_CORE_DUMP: std::cout << "ELF Core Dump\n"; break;
        case BinaryType::RAW_BINARY: std::cout << "Raw Binary\n"; break;
        default: std::cout << "Unknown\n";
    }

    std::cout << "Architecture: " << info.architecture << "\n";
    std::cout << "Entry Point: 0x" << std::hex << info.entry_point << std::dec << "\n";
    std::cout << "Segments: " << segments.size() << "\n";

    for (const auto& seg : segments) {
        std::cout << "  " << std::setw(10) << seg.name
                  << " @ 0x" << std::hex << std::setw(12) << seg.virtual_addr
                  << " size: " << std::dec << std::setw(8) << seg.memory_size
                  << " perms: "
                  << (seg.is_readable() ? "R" : "-")
                  << (seg.is_writable() ? "W" : "-")
                  << (seg.is_code() ? "X" : "-")
                  << " data: " << seg.data.size() << " bytes\n";

        // Show first few bytes as hex
        if (seg.data.size() > 0) {
            std::cout << "    First 32 bytes: ";
            for (size_t i = 0; i < std::min(size_t(32), seg.data.size()); i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                         << (int)seg.data[i] << " ";
                if (i % 16 == 15) std::cout << "\n                    ";
            }
            std::cout << "\n";
        }
    }

    // For visualization
    auto flattened = loader.GetFlattenedMemory();
    std::cout << "\nTotal flattened size: " << flattened.size() << " bytes\n";
}

int main(int argc, char* argv[]) {
    std::cout << "=== Binary File Loader Test ===\n";

    if (argc > 1) {
        // Test with provided files
        for (int i = 1; i < argc; i++) {
            TestBinaryFile(argv[i]);
        }
    } else {
        // Test with common system files
        std::cout << "\nTesting system binaries...\n";

        // Test an executable
        TestBinaryFile("/bin/ls");

        // Test a shared library
        TestBinaryFile("/usr/lib/libc.so.6");
        TestBinaryFile("/usr/lib/libc.dylib");  // macOS

        // Test our own executable
        TestBinaryFile(argv[0]);
    }

    std::cout << "\n=== Creating a test core dump ===\n";
    std::cout << "To test core dump loading, run:\n";
    std::cout << "  sleep 100 &\n";
    std::cout << "  gcore $!\n";
    std::cout << "  ./test_binary_loader core.*\n";

    return 0;
}