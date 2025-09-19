#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace Haywire {

// Memory mapping information from /proc/pid/maps
struct MemoryMapping {
    uint64_t start_addr;
    uint64_t end_addr;
    std::string permissions;  // "rwxp" format
    uint64_t offset;
    std::string device;       // "fd:02" format
    uint64_t inode;
    std::string pathname;      // File or [heap], [stack], etc.

    size_t GetSize() const { return end_addr - start_addr; }
    bool IsReadable() const { return permissions.find('r') != std::string::npos; }
    bool IsWritable() const { return permissions.find('w') != std::string::npos; }
    bool IsExecutable() const { return permissions.find('x') != std::string::npos; }
    bool IsPrivate() const { return permissions.find('p') != std::string::npos; }
    bool IsShared() const { return permissions.find('s') != std::string::npos; }
};

// File descriptor information from /proc/pid/fd
struct FileDescriptor {
    int fd;
    std::string target;        // Symlink target (file path, socket:[id], pipe:[id], etc.)
    std::string type;          // "file", "socket", "pipe", "anon_inode", etc.
    uint64_t inode;

    bool IsSocket() const { return type == "socket"; }
    bool IsFile() const { return type == "file"; }
    bool IsPipe() const { return type == "pipe"; }
};

// Network connection information
struct NetworkConnection {
    std::string protocol;      // "tcp", "udp", "unix"
    std::string local_addr;
    uint16_t local_port;
    std::string remote_addr;
    uint16_t remote_port;
    std::string state;         // "ESTABLISHED", "LISTEN", etc.
    uint64_t inode;           // Socket inode for matching with FDs
};

// Thread information
struct ThreadInfo {
    uint64_t tid;
    std::string name;
    std::string state;        // "R", "S", "D", etc.
    uint64_t cpu_time;       // CPU time in jiffies
};

// Extended process information
struct ProcessInfoExtended {
    // Basic info (from kernel structures)
    uint64_t pid;
    std::string name;
    uint64_t task_struct_addr;
    uint64_t page_table_base;
    uint64_t mm_struct_addr;
    uint64_t parent_pid;

    // Status info (from /proc/pid/status)
    std::string state;         // "R (running)", "S (sleeping)", etc.
    uint64_t uid;
    uint64_t gid;
    uint64_t vm_peak;         // Peak virtual memory size
    uint64_t vm_size;         // Current virtual memory size
    uint64_t vm_rss;          // Resident set size
    uint64_t vm_data;         // Data segment size
    uint64_t vm_stack;        // Stack size
    uint64_t vm_exe;          // Executable size
    uint64_t vm_lib;          // Shared library size
    uint64_t threads;         // Number of threads
    uint64_t fd_count;        // Number of open file descriptors

    // Command line (from /proc/pid/cmdline)
    std::vector<std::string> cmdline;

    // Environment variables (from /proc/pid/environ)
    std::map<std::string, std::string> environment;

    // Memory mappings (from /proc/pid/maps)
    std::vector<MemoryMapping> memory_maps;

    // File descriptors (from /proc/pid/fd)
    std::vector<FileDescriptor> file_descriptors;

    // Network connections
    std::vector<NetworkConnection> network_connections;

    // Threads (from /proc/pid/task)
    std::vector<ThreadInfo> threads_list;

    // Executable path (from /proc/pid/exe)
    std::string exe_path;

    // Current working directory (from /proc/pid/cwd)
    std::string cwd;

    // Root directory (from /proc/pid/root)
    std::string root;

    // Helper functions
    size_t GetTotalMappedMemory() const;
    size_t GetExecutableMemory() const;
    size_t GetHeapSize() const;
    size_t GetStackSize() const;
    std::vector<MemoryMapping> GetLibraryMappings() const;
    std::vector<FileDescriptor> GetSocketDescriptors() const;
    std::vector<FileDescriptor> GetFileDescriptors() const;
    bool HasOpenFile(const std::string& path) const;
    bool HasNetworkConnection(const std::string& addr) const;
};

}