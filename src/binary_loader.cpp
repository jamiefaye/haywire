#include "binary_loader.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <algorithm>

// ELF structures (simplified)
#define ELF_MAGIC 0x464C457F  // "\x7FELF"

// Mach-O structures (simplified)
#define MH_MAGIC_64 0xFEEDFACF
#define MH_CIGAM_64 0xCFFAEDFE  // Little-endian (swapped)

struct mach_header_64 {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};

struct load_command {
    uint32_t cmd;
    uint32_t cmdsize;
};

struct segment_command_64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};

#define LC_SEGMENT_64 0x19

struct Elf64_Ehdr {
    uint32_t magic;
    uint8_t ident[12];
    uint16_t type;      // 2=ET_EXEC, 3=ET_DYN, 4=ET_CORE
    uint16_t machine;   // 183=EM_AARCH64, 62=EM_X86_64
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;     // Program header offset
    uint64_t shoff;     // Section header offset
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;     // Number of program headers
    uint16_t shentsize;
    uint16_t shnum;     // Number of section headers
    uint16_t shstrndx;
};

struct Elf64_Phdr {
    uint32_t type;      // 1=PT_LOAD, 4=PT_NOTE
    uint32_t flags;     // 1=X, 2=W, 4=R
    uint64_t offset;    // File offset
    uint64_t vaddr;     // Virtual address
    uint64_t paddr;     // Physical address (usually same as vaddr)
    uint64_t filesz;    // Size in file
    uint64_t memsz;     // Size in memory
    uint64_t align;
};

struct Elf64_Shdr {
    uint32_t name;      // Offset into string table
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
};

namespace Haywire {

BinaryLoader::BinaryLoader() {
    info.type = BinaryType::UNKNOWN;
    info.entry_point = 0;
    info.is_64bit = false;
    info.is_little_endian = true;
    info.pid = 0;
    info.signal = 0;
}

BinaryLoader::~BinaryLoader() {
}

bool BinaryLoader::LoadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file: " << path << "\n";
        return false;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    // For large files (> 100MB), use memory mapping instead
    const size_t LARGE_FILE_THRESHOLD = 100 * 1024 * 1024; // 100MB

    if (size > LARGE_FILE_THRESHOLD) {
        std::cout << "Large file detected (" << (size / (1024*1024)) << " MB), using memory mapping...\n";

        // Just read the first 64KB for type detection
        const size_t HEADER_SIZE = 65536;
        rawData.resize(std::min(size, HEADER_SIZE));
        file.read(reinterpret_cast<char*>(rawData.data()), rawData.size());

        // Store the file path and size for later memory-mapped access
        filePath = path;
        fileSize = size;
        useMmap = true;

        // Detect type from header only
        info.type = DetectType(rawData.data(), rawData.size());

        // For non-binary formats, treat as raw
        if (info.type == BinaryType::UNKNOWN) {
            info.type = BinaryType::RAW_BINARY;
        }

        // Create a single segment for the entire file
        BinarySegment seg;
        seg.name = "mmap";
        seg.virtual_addr = 0;
        seg.file_offset = 0;
        seg.file_size = size;
        seg.memory_size = size;
        seg.permissions = 0x4; // Read-only
        segments.push_back(seg);

        return true;
    }

    // For smaller files, read entire file as before
    rawData.resize(size);
    file.read(reinterpret_cast<char*>(rawData.data()), size);

    return LoadFromMemory(rawData.data(), rawData.size());
}

bool BinaryLoader::LoadFromMemory(const uint8_t* data, size_t size) {
    if (!data || size < 64) {
        return false;
    }

    // Keep a copy of the data
    rawData.assign(data, data + size);

    // Detect type
    info.type = DetectType(data, size);

    switch (info.type) {
        case BinaryType::ELF_EXECUTABLE:
        case BinaryType::ELF_SHARED_OBJECT:
            return ParseELFExecutable();

        case BinaryType::ELF_CORE_DUMP:
            return ParseELFCore();

        case BinaryType::MACH_O_EXECUTABLE:
        case BinaryType::MACH_O_DYLIB:
            return ParseMachO();

        case BinaryType::RAW_BINARY: {
            // Just load as one big segment
            BinarySegment seg;
            seg.name = "raw";
            seg.virtual_addr = 0;
            seg.file_offset = 0;
            seg.file_size = size;
            seg.memory_size = size;
            seg.permissions = 0x6;  // RW
            seg.data = rawData;
            segments.push_back(seg);
            return true;
        }

        case BinaryType::UNKNOWN:
        default:
            // Treat unknown files as raw binary - load entire file as one blob
            info.type = BinaryType::RAW_BINARY;
            BinarySegment seg;
            seg.name = "raw";
            seg.virtual_addr = 0;
            seg.file_offset = 0;
            seg.file_size = size;
            seg.memory_size = size;
            seg.permissions = 0x6;  // RW
            seg.data = rawData;
            segments.push_back(seg);
            std::cout << "Loaded unknown file as raw binary: " << size << " bytes\n";
            return true;
    }
}

BinaryType BinaryLoader::DetectType(const uint8_t* data, size_t size) {
    if (size < 4) return BinaryType::UNKNOWN;

    uint32_t magic = *reinterpret_cast<const uint32_t*>(data);

    // ELF magic: 0x7F 'E' 'L' 'F'
    if (magic == ELF_MAGIC) {
        if (size < sizeof(Elf64_Ehdr)) return BinaryType::UNKNOWN;

        const Elf64_Ehdr* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
        switch (ehdr->type) {
            case 2: return BinaryType::ELF_EXECUTABLE;
            case 3: return BinaryType::ELF_SHARED_OBJECT;
            case 4: return BinaryType::ELF_CORE_DUMP;
            default: return BinaryType::UNKNOWN;
        }
    }

    // Mach-O magic: 0xFEEDFACF (64-bit) or 0xFEEDFACE (32-bit)
    // Also check for universal binary: 0xCAFEBABE (big-endian) or 0xBEBAFECA (little-endian)
    if (magic == 0xCFFAEDFE || magic == 0xCEFAEDFE ||  // 64-bit little-endian
        magic == 0xFEEDFACF || magic == 0xFEEDFACE ||  // Native endian
        magic == 0xBEBAFECA || magic == 0xCAFEBABE) {  // Universal binary
        return BinaryType::MACH_O_EXECUTABLE;
    }

    // PE/Windows: "MZ" at start
    if ((data[0] == 'M' && data[1] == 'Z')) {
        return BinaryType::PE_EXECUTABLE;
    }

    // Minidump: "MDMP"
    if (magic == 0x504D444D) {
        return BinaryType::MINIDUMP;
    }

    return BinaryType::RAW_BINARY;
}

bool BinaryLoader::ParseELFExecutable() {
    if (rawData.size() < sizeof(Elf64_Ehdr)) {
        return false;
    }

    const Elf64_Ehdr* ehdr = reinterpret_cast<const Elf64_Ehdr*>(rawData.data());

    // Set binary info
    info.is_64bit = true;
    info.entry_point = ehdr->entry;

    // Determine architecture
    switch (ehdr->machine) {
        case 183: info.architecture = "aarch64"; break;
        case 62:  info.architecture = "x86_64"; break;
        case 3:   info.architecture = "i386"; break;
        case 40:  info.architecture = "arm"; break;
        default:  info.architecture = "unknown"; break;
    }

    info.os = "linux";  // ELF is typically Linux/Unix

    // Parse program headers (segments)
    for (int i = 0; i < ehdr->phnum; i++) {
        size_t ph_offset = ehdr->phoff + i * ehdr->phentsize;
        if (ph_offset + sizeof(Elf64_Phdr) > rawData.size()) {
            break;
        }

        const Elf64_Phdr* phdr = reinterpret_cast<const Elf64_Phdr*>(
            rawData.data() + ph_offset);

        // Only interested in LOAD segments for executables
        if (phdr->type != 1) continue;  // PT_LOAD = 1

        BinarySegment seg;
        seg.virtual_addr = phdr->vaddr;
        seg.file_offset = phdr->offset;
        seg.file_size = phdr->filesz;
        seg.memory_size = phdr->memsz;

        // Parse permissions from flags
        seg.permissions = 0;
        if (phdr->flags & 0x1) seg.permissions |= 0x1;  // Execute
        if (phdr->flags & 0x2) seg.permissions |= 0x2;  // Write
        if (phdr->flags & 0x4) seg.permissions |= 0x4;  // Read

        // Load actual data
        if (phdr->filesz > 0 && phdr->offset + phdr->filesz <= rawData.size()) {
            seg.data.assign(
                rawData.begin() + phdr->offset,
                rawData.begin() + phdr->offset + phdr->filesz
            );
        }

        // Determine segment name based on permissions and address
        if (seg.permissions & 0x1) {
            seg.name = ".text";  // Executable = code
        } else if (seg.permissions & 0x2) {
            if (seg.file_size == 0) {
                seg.name = ".bss";  // Writable, no file data
            } else {
                seg.name = ".data";  // Writable with data
            }
        } else {
            seg.name = ".rodata";  // Read-only data
        }

        segments.push_back(seg);
    }

    // Parse section headers for more detailed names (optional)
    if (ehdr->shnum > 0 && ehdr->shoff > 0) {
        // Get string table
        size_t strtab_offset = ehdr->shoff + ehdr->shstrndx * ehdr->shentsize;
        if (strtab_offset + sizeof(Elf64_Shdr) <= rawData.size()) {
            const Elf64_Shdr* strtab_hdr = reinterpret_cast<const Elf64_Shdr*>(
                rawData.data() + strtab_offset);

            // Parse each section
            for (int i = 0; i < ehdr->shnum; i++) {
                size_t sh_offset = ehdr->shoff + i * ehdr->shentsize;
                if (sh_offset + sizeof(Elf64_Shdr) > rawData.size()) break;

                const Elf64_Shdr* shdr = reinterpret_cast<const Elf64_Shdr*>(
                    rawData.data() + sh_offset);

                // Get section name
                if (strtab_hdr->offset + shdr->name < rawData.size()) {
                    const char* name = reinterpret_cast<const char*>(
                        rawData.data() + strtab_hdr->offset + shdr->name);

                    // Update segment name if we find a match
                    for (auto& seg : segments) {
                        if (shdr->addr >= seg.virtual_addr &&
                            shdr->addr < seg.virtual_addr + seg.memory_size) {
                            // This section is within this segment
                            if (strcmp(name, ".text") == 0 ||
                                strcmp(name, ".data") == 0 ||
                                strcmp(name, ".rodata") == 0 ||
                                strcmp(name, ".bss") == 0) {
                                seg.name = name;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    std::cout << "Loaded ELF executable: " << segments.size() << " segments\n";
    for (const auto& seg : segments) {
        std::cout << "  " << seg.name
                  << ": VA 0x" << std::hex << seg.virtual_addr
                  << " Size: " << std::dec << seg.memory_size << " bytes"
                  << " Perms: " << (seg.is_readable() ? "R" : "-")
                  << (seg.is_writable() ? "W" : "-")
                  << (seg.is_code() ? "X" : "-") << "\n";
    }

    return true;
}

bool BinaryLoader::ParseELFCore() {
    if (rawData.size() < sizeof(Elf64_Ehdr)) {
        return false;
    }

    const Elf64_Ehdr* ehdr = reinterpret_cast<const Elf64_Ehdr*>(rawData.data());

    info.is_64bit = true;
    info.architecture = (ehdr->machine == 183) ? "aarch64" : "x86_64";
    info.os = "linux";

    // Parse program headers to find memory segments and notes
    for (int i = 0; i < ehdr->phnum; i++) {
        size_t ph_offset = ehdr->phoff + i * ehdr->phentsize;
        if (ph_offset + sizeof(Elf64_Phdr) > rawData.size()) {
            break;
        }

        const Elf64_Phdr* phdr = reinterpret_cast<const Elf64_Phdr*>(
            rawData.data() + ph_offset);

        if (phdr->type == 1) {  // PT_LOAD - memory segment
            BinarySegment seg;
            seg.name = "memory";
            seg.virtual_addr = phdr->vaddr;
            seg.file_offset = phdr->offset;
            seg.file_size = phdr->filesz;
            seg.memory_size = phdr->memsz;

            // Core dumps usually have RW permissions
            seg.permissions = 0x6;  // RW

            // Load actual memory data
            if (phdr->filesz > 0 && phdr->offset + phdr->filesz <= rawData.size()) {
                seg.data.assign(
                    rawData.begin() + phdr->offset,
                    rawData.begin() + phdr->offset + phdr->filesz
                );
            }

            segments.push_back(seg);
        } else if (phdr->type == 4) {  // PT_NOTE - process info
            // Parse notes for process information
            size_t note_offset = phdr->offset;
            size_t note_end = phdr->offset + phdr->filesz;

            while (note_offset < note_end && note_offset < rawData.size()) {
                // Note structure: namesz, descsz, type, name, desc
                uint32_t namesz = *reinterpret_cast<const uint32_t*>(
                    rawData.data() + note_offset);
                uint32_t descsz = *reinterpret_cast<const uint32_t*>(
                    rawData.data() + note_offset + 4);
                uint32_t type = *reinterpret_cast<const uint32_t*>(
                    rawData.data() + note_offset + 8);

                // NT_PRPSINFO = 3, contains process info
                if (type == 3 && note_offset + 12 + namesz + descsz <= rawData.size()) {
                    // Extract command line (simplified)
                    const char* cmd = reinterpret_cast<const char*>(
                        rawData.data() + note_offset + 12 + namesz + 44);
                    info.command_line = std::string(cmd);
                }

                // Move to next note (aligned to 4 bytes)
                note_offset += 12 + ((namesz + 3) & ~3) + ((descsz + 3) & ~3);
            }
        }
    }

    std::cout << "Loaded ELF core dump: " << segments.size() << " memory segments\n";
    if (!info.command_line.empty()) {
        std::cout << "  Command: " << info.command_line << "\n";
    }

    return true;
}

std::vector<uint8_t> BinaryLoader::GetFlattenedMemory() const {
    std::vector<uint8_t> result;

    for (const auto& seg : segments) {
        result.insert(result.end(), seg.data.begin(), seg.data.end());
    }

    return result;
}

std::vector<uint8_t> BinaryLoader::GetMemoryLayout(uint64_t total_size) const {
    std::vector<uint8_t> result(total_size, 0);

    for (const auto& seg : segments) {
        if (seg.virtual_addr < total_size) {
            size_t copy_size = std::min(
                seg.data.size(),
                static_cast<size_t>(total_size - seg.virtual_addr)
            );

            std::copy(seg.data.begin(),
                     seg.data.begin() + copy_size,
                     result.begin() + seg.virtual_addr);
        }
    }

    return result;
}

const BinarySegment* BinaryLoader::FindSegment(const std::string& name) const {
    for (const auto& seg : segments) {
        if (seg.name == name) {
            return &seg;
        }
    }
    return nullptr;
}

const BinarySegment* BinaryLoader::FindSegmentByAddress(uint64_t addr) const {
    for (const auto& seg : segments) {
        if (addr >= seg.virtual_addr &&
            addr < seg.virtual_addr + seg.memory_size) {
            return &seg;
        }
    }
    return nullptr;
}

bool BinaryLoader::ParseMachO() {
    if (rawData.size() < sizeof(mach_header_64)) {
        return false;
    }

    // Check for universal binary first
    uint32_t magic = *reinterpret_cast<const uint32_t*>(rawData.data());
    size_t arch_offset = 0;

    if (magic == 0xCAFEBABE || magic == 0xBEBAFECA) {
        // Universal binary - find the right architecture
        // Fat binaries are always big-endian
        struct fat_header {
            uint32_t magic;
            uint32_t nfat_arch;
        };
        struct fat_arch {
            uint32_t cputype;
            uint32_t cpusubtype;
            uint32_t offset;
            uint32_t size;
            uint32_t align;
        };

        // Read number of architectures (always big-endian)
        uint32_t narch = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(rawData.data() + 4));

        // Look for ARM64 or x86_64 architecture
        std::cout << "Universal binary with " << narch << " architectures\n";
        for (uint32_t i = 0; i < narch && i < 10; i++) {
            const fat_arch* arch = reinterpret_cast<const fat_arch*>(
                rawData.data() + sizeof(fat_header) + i * sizeof(fat_arch));

            // Fat arch entries are always big-endian
            uint32_t cputype = __builtin_bswap32(arch->cputype);
            uint32_t offset = __builtin_bswap32(arch->offset);

            std::cout << "  Arch " << i << ": cputype=0x" << std::hex << cputype
                      << " offset=0x" << offset << std::dec << "\n";

            // CPU_TYPE_ARM64 = 0x0100000c, CPU_TYPE_X86_64 = 0x01000007
            if (cputype == 0x0100000c || cputype == 0x01000007) {
                arch_offset = offset;
                std::cout << "  Selected architecture at offset 0x" << std::hex << offset << std::dec << "\n";
                break;
            }
        }

        if (arch_offset == 0 || arch_offset >= rawData.size()) {
            std::cerr << "Could not find supported architecture in universal binary\n";
            return false;
        }
    }

    const mach_header_64* header = reinterpret_cast<const mach_header_64*>(
        rawData.data() + arch_offset);

    // Set binary info
    info.is_64bit = true;
    info.is_little_endian = true;
    info.os = "macos";

    // Determine architecture from cputype
    // CPU_TYPE_ARM64 = 0x0100000c, CPU_TYPE_X86_64 = 0x01000007
    if ((header->cputype & 0xff) == 0x0c) {
        info.architecture = "arm64";
    } else if ((header->cputype & 0xff) == 0x07) {
        info.architecture = "x86_64";
    } else {
        info.architecture = "unknown";
    }

    // Determine file type
    switch (header->filetype) {
        case 2:  // MH_EXECUTE
            info.type = BinaryType::MACH_O_EXECUTABLE;
            break;
        case 6:  // MH_DYLIB
            info.type = BinaryType::MACH_O_DYLIB;
            break;
        default:
            break;
    }

    // Parse load commands to find segments
    size_t cmd_offset = arch_offset + sizeof(mach_header_64);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (cmd_offset + sizeof(load_command) > rawData.size()) {
            break;
        }

        const load_command* cmd = reinterpret_cast<const load_command*>(
            rawData.data() + cmd_offset);

        if (cmd->cmd == LC_SEGMENT_64) {
            const segment_command_64* seg_cmd = reinterpret_cast<const segment_command_64*>(cmd);

            BinarySegment seg;
            seg.name = std::string(seg_cmd->segname, strnlen(seg_cmd->segname, 16));
            seg.virtual_addr = seg_cmd->vmaddr;
            seg.file_offset = seg_cmd->fileoff;
            seg.file_size = seg_cmd->filesize;
            seg.memory_size = seg_cmd->vmsize;

            // Parse protection flags (1=R, 2=W, 4=X)
            seg.permissions = 0;
            if (seg_cmd->initprot & 0x1) seg.permissions |= 0x4;  // Read
            if (seg_cmd->initprot & 0x2) seg.permissions |= 0x2;  // Write
            if (seg_cmd->initprot & 0x4) seg.permissions |= 0x1;  // Execute

            // Load actual data
            if (seg_cmd->filesize > 0 &&
                seg_cmd->fileoff + seg_cmd->filesize <= rawData.size()) {
                seg.data.assign(
                    rawData.begin() + seg_cmd->fileoff,
                    rawData.begin() + seg_cmd->fileoff + seg_cmd->filesize
                );
            }

            segments.push_back(seg);

            // Set entry point from __TEXT segment (usually first)
            if (seg.name == "__TEXT" && info.entry_point == 0) {
                info.entry_point = seg.virtual_addr;
            }
        }

        cmd_offset += cmd->cmdsize;
    }

    std::cout << "Loaded Mach-O binary: " << segments.size() << " segments\n";
    for (const auto& seg : segments) {
        std::cout << "  " << seg.name
                  << ": VA 0x" << std::hex << seg.virtual_addr
                  << " Size: " << std::dec << seg.memory_size << " bytes"
                  << " Perms: " << (seg.is_readable() ? "R" : "-")
                  << (seg.is_writable() ? "W" : "-")
                  << (seg.is_code() ? "X" : "-") << "\n";
    }

    return true;
}

}