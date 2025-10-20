#pragma once

#include <cstdint>

namespace Haywire {

// Memory section entry - describes a virtual memory region
struct SectionEntry {
    uint32_t type;  // Entry type (for compatibility)
    uint32_t pid;
    uint64_t va_start;
    uint64_t va_end;
    uint32_t perms;
    uint32_t ownership_type;  // Ownership classification (0=unknown, 1=anon, 2=file, 3=lib, 4=exec, 5=stack, 6=heap, 7=vdso, 8=vvar)
    char path[64];
} __attribute__((packed));

// Process ID entry
struct PIDEntry {
    uint32_t type;  // Entry type (for compatibility)
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t gid;
    uint64_t rss_kb;
    char comm[16];
    char state;
    uint8_t padding[3];
} __attribute__((packed));

// Page table entry
struct PTEEntry {
    uint32_t type;  // Entry type (for compatibility)
    uint32_t reserved;
    uint64_t va;
    uint64_t pa;
    uint32_t flags;
    uint32_t reserved2;
} __attribute__((packed));

} // namespace Haywire