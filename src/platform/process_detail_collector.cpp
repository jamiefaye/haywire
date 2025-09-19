#include "platform/process_detail_collector.h"
#include "platform/process_walker.h"
#include "memory_backend.h"
#include <sstream>
#include <iomanip>
#include <regex>
#include <algorithm>
#include <iostream>

namespace Haywire {

ProcessDetailCollector::ProcessDetailCollector(MemoryBackend* backend, CollectionMethod method)
    : memory(backend), method(method) {
    if (backend) {
        processWalker = CreateProcessWalker(backend, "linux");
    }
}

ProcessDetailCollector::~ProcessDetailCollector() {
}

void ProcessDetailCollector::SetGuestAgent(std::function<std::string(const std::string&)> execCommand) {
    guestExec = execCommand;
}

bool ProcessDetailCollector::CollectProcessDetails(uint64_t pid, ProcessInfoExtended& info) {
    // Start with basic info from kernel structures
    if (processWalker) {
        ProcessInfo basicInfo;
        if (processWalker->FindProcess(pid, basicInfo)) {
            info.pid = basicInfo.pid;
            info.name = basicInfo.name;
            info.task_struct_addr = basicInfo.task_struct_addr;
            info.page_table_base = basicInfo.page_table_base;
            info.mm_struct_addr = basicInfo.mm_struct_addr;
            info.parent_pid = basicInfo.parent_pid;
        }
    }

    bool success = true;

    // Collect additional details based on method
    if (method == METHOD_GUEST_AGENT || method == METHOD_HYBRID) {
        if (guestExec) {
            success &= CollectStatusFromProc(pid, info);
            success &= CollectMemoryMapsFromProc(pid, info.memory_maps);
            success &= CollectFileDescriptorsFromProc(pid, info.file_descriptors);
            success &= CollectCommandLine(pid, info.cmdline);
        }
    }

    if (method == METHOD_MEMORY_SCAN || method == METHOD_HYBRID) {
        if (info.task_struct_addr != 0) {
            // These would parse kernel structures directly
            // For now, we'll focus on the /proc approach
        }
    }

    return success;
}

std::vector<ProcessInfoExtended> ProcessDetailCollector::CollectAllProcessDetails() {
    std::vector<ProcessInfoExtended> result;

    if (!processWalker || !processWalker->Initialize()) {
        return result;
    }

    auto processes = processWalker->EnumerateProcesses();
    for (const auto& proc : processes) {
        ProcessInfoExtended extended;
        if (CollectProcessDetails(proc.pid, extended)) {
            result.push_back(extended);
        }
    }

    return result;
}

bool ProcessDetailCollector::CollectMemoryMaps(uint64_t pid, std::vector<MemoryMapping>& maps) {
    if (method == METHOD_GUEST_AGENT || method == METHOD_HYBRID) {
        return CollectMemoryMapsFromProc(pid, maps);
    } else {
        return CollectMemoryMapsFromKernel(pid, maps);
    }
}

bool ProcessDetailCollector::CollectFileDescriptors(uint64_t pid, std::vector<FileDescriptor>& fds) {
    if (method == METHOD_GUEST_AGENT || method == METHOD_HYBRID) {
        return CollectFileDescriptorsFromProc(pid, fds);
    } else {
        return CollectFileDescriptorsFromKernel(pid, fds);
    }
}

bool ProcessDetailCollector::CollectMemoryMapsFromProc(uint64_t pid, std::vector<MemoryMapping>& maps) {
    if (!guestExec) return false;

    std::string cmd = "cat /proc/" + std::to_string(pid) + "/maps 2>/dev/null";
    std::string content = guestExec(cmd);

    if (content.empty()) return false;

    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        MemoryMapping map;
        if (ParseMapsLine(line, map)) {
            maps.push_back(map);
        }
    }

    return !maps.empty();
}

bool ProcessDetailCollector::ParseMapsLine(const std::string& line, MemoryMapping& map) {
    // Parse lines like:
    // c5934f990000-c5934f9ae000 r-xp 00000000 fd:02 271644  /usr/lib/systemd/systemd
    // ffff8000-ffff8001 rwxp 00000000 00:00 0  [vectors]

    std::istringstream ss(line);
    std::string addr_range, perms, device;

    // Read address range
    if (!(ss >> addr_range)) return false;

    size_t dash = addr_range.find('-');
    if (dash == std::string::npos) return false;

    try {
        map.start_addr = std::stoull(addr_range.substr(0, dash), nullptr, 16);
        map.end_addr = std::stoull(addr_range.substr(dash + 1), nullptr, 16);
    } catch (...) {
        return false;
    }

    // Read permissions
    if (!(ss >> map.permissions)) return false;

    // Read offset
    std::string offset_str;
    if (!(ss >> offset_str)) return false;
    try {
        map.offset = std::stoull(offset_str, nullptr, 16);
    } catch (...) {
        return false;
    }

    // Read device
    if (!(ss >> map.device)) return false;

    // Read inode
    if (!(ss >> map.inode)) return false;

    // Read pathname (may be empty or contain spaces)
    std::string remaining;
    std::getline(ss, remaining);

    // Trim leading whitespace
    size_t first_non_space = remaining.find_first_not_of(" \t");
    if (first_non_space != std::string::npos) {
        map.pathname = remaining.substr(first_non_space);
    }

    return true;
}

bool ProcessDetailCollector::CollectFileDescriptorsFromProc(uint64_t pid, std::vector<FileDescriptor>& fds) {
    if (!guestExec) return false;

    // List all file descriptors
    std::string cmd = "ls -la /proc/" + std::to_string(pid) + "/fd 2>/dev/null";
    std::string content = guestExec(cmd);

    if (content.empty()) return false;

    std::istringstream stream(content);
    std::string line;

    // Parse ls output to get FD numbers and their targets
    // Format: lrwx------ 1 user group 64 date time fd -> target
    std::regex fd_regex(R"((\d+)\s+->\s+(.+))");

    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, fd_regex)) {
            FileDescriptor fd;
            try {
                fd.fd = std::stoi(match[1]);
                fd.target = match[2];

                // Determine type from target
                if (fd.target.find("socket:[") != std::string::npos) {
                    fd.type = "socket";
                    // Extract inode from socket:[12345]
                    size_t start = fd.target.find('[') + 1;
                    size_t end = fd.target.find(']');
                    if (start != std::string::npos && end != std::string::npos) {
                        fd.inode = std::stoull(fd.target.substr(start, end - start));
                    }
                } else if (fd.target.find("pipe:[") != std::string::npos) {
                    fd.type = "pipe";
                    size_t start = fd.target.find('[') + 1;
                    size_t end = fd.target.find(']');
                    if (start != std::string::npos && end != std::string::npos) {
                        fd.inode = std::stoull(fd.target.substr(start, end - start));
                    }
                } else if (fd.target.find("anon_inode:") != std::string::npos) {
                    fd.type = "anon_inode";
                } else if (fd.target[0] == '/') {
                    fd.type = "file";
                } else {
                    fd.type = "unknown";
                }

                fds.push_back(fd);
            } catch (...) {
                // Skip invalid entries
            }
        }
    }

    return !fds.empty();
}

bool ProcessDetailCollector::CollectStatusFromProc(uint64_t pid, ProcessInfoExtended& info) {
    if (!guestExec) return false;

    std::string cmd = "cat /proc/" + std::to_string(pid) + "/status 2>/dev/null";
    std::string content = guestExec(cmd);

    if (content.empty()) return false;

    return ParseStatusFile(content, info);
}

bool ProcessDetailCollector::ParseStatusFile(const std::string& content, ProcessInfoExtended& info) {
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        // Trim whitespace from value
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        if (key == "State") {
            info.state = value;
        } else if (key == "Uid") {
            std::istringstream iss(value);
            iss >> info.uid;
        } else if (key == "Gid") {
            std::istringstream iss(value);
            iss >> info.gid;
        } else if (key == "VmPeak") {
            std::istringstream iss(value);
            iss >> info.vm_peak;
        } else if (key == "VmSize") {
            std::istringstream iss(value);
            iss >> info.vm_size;
        } else if (key == "VmRSS") {
            std::istringstream iss(value);
            iss >> info.vm_rss;
        } else if (key == "VmData") {
            std::istringstream iss(value);
            iss >> info.vm_data;
        } else if (key == "VmStk") {
            std::istringstream iss(value);
            iss >> info.vm_stack;
        } else if (key == "VmExe") {
            std::istringstream iss(value);
            iss >> info.vm_exe;
        } else if (key == "VmLib") {
            std::istringstream iss(value);
            iss >> info.vm_lib;
        } else if (key == "Threads") {
            std::istringstream iss(value);
            iss >> info.threads;
        } else if (key == "FDSize") {
            std::istringstream iss(value);
            iss >> info.fd_count;
        }
    }

    return true;
}

bool ProcessDetailCollector::CollectCommandLine(uint64_t pid, std::vector<std::string>& cmdline) {
    if (!guestExec) return false;

    // Read cmdline (null-separated arguments)
    std::string cmd = "cat /proc/" + std::to_string(pid) + "/cmdline 2>/dev/null | tr '\\0' ' '";
    std::string content = guestExec(cmd);

    if (content.empty()) return false;

    // Split by spaces (we converted nulls to spaces)
    std::istringstream stream(content);
    std::string arg;
    while (stream >> arg) {
        cmdline.push_back(arg);
    }

    return !cmdline.empty();
}

bool ProcessDetailCollector::CollectEnvironment(uint64_t pid, std::map<std::string, std::string>& env) {
    if (!guestExec) return false;

    // Read environment (null-separated KEY=VALUE pairs)
    std::string cmd = "cat /proc/" + std::to_string(pid) + "/environ 2>/dev/null | tr '\\0' '\\n'";
    std::string content = guestExec(cmd);

    if (content.empty()) return false;

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            env[key] = value;
        }
    }

    return !env.empty();
}

bool ProcessDetailCollector::CollectThreads(uint64_t pid, std::vector<ThreadInfo>& threads) {
    if (!guestExec) return false;

    // List threads
    std::string cmd = "ls /proc/" + std::to_string(pid) + "/task 2>/dev/null";
    std::string content = guestExec(cmd);

    if (content.empty()) return false;

    std::istringstream stream(content);
    std::string tid_str;
    while (stream >> tid_str) {
        try {
            ThreadInfo thread;
            thread.tid = std::stoull(tid_str);

            // Get thread name
            std::string comm_cmd = "cat /proc/" + std::to_string(pid) + "/task/" + tid_str + "/comm 2>/dev/null";
            thread.name = guestExec(comm_cmd);
            thread.name.erase(thread.name.find_last_not_of("\n\r\t ") + 1);

            threads.push_back(thread);
        } catch (...) {
            // Skip invalid TIDs
        }
    }

    return !threads.empty();
}

bool ProcessDetailCollector::CollectNetworkConnections(uint64_t pid, std::vector<NetworkConnection>& conns) {
    // This requires matching socket inodes from FDs with /proc/net/tcp, /proc/net/udp, etc.
    // For now, return empty
    return false;
}

// Kernel structure parsing methods (stubs for now)
bool ProcessDetailCollector::CollectMemoryMapsFromKernel(uint64_t task_addr, std::vector<MemoryMapping>& maps) {
    // Would parse mm_struct->mmap linked list
    // Each vm_area_struct contains start, end, flags, file pointer, etc.
    return false;
}

bool ProcessDetailCollector::CollectFileDescriptorsFromKernel(uint64_t task_addr, std::vector<FileDescriptor>& fds) {
    // Would parse task_struct->files->fdtable
    return false;
}

// ProcessInfoExtended helper implementations
size_t ProcessInfoExtended::GetTotalMappedMemory() const {
    size_t total = 0;
    for (const auto& map : memory_maps) {
        total += map.GetSize();
    }
    return total;
}

size_t ProcessInfoExtended::GetExecutableMemory() const {
    size_t total = 0;
    for (const auto& map : memory_maps) {
        if (map.IsExecutable()) {
            total += map.GetSize();
        }
    }
    return total;
}

size_t ProcessInfoExtended::GetHeapSize() const {
    for (const auto& map : memory_maps) {
        if (map.pathname == "[heap]") {
            return map.GetSize();
        }
    }
    return 0;
}

size_t ProcessInfoExtended::GetStackSize() const {
    for (const auto& map : memory_maps) {
        if (map.pathname == "[stack]") {
            return map.GetSize();
        }
    }
    return 0;
}

std::vector<MemoryMapping> ProcessInfoExtended::GetLibraryMappings() const {
    std::vector<MemoryMapping> libs;
    for (const auto& map : memory_maps) {
        if (map.pathname.find(".so") != std::string::npos) {
            libs.push_back(map);
        }
    }
    return libs;
}

std::vector<FileDescriptor> ProcessInfoExtended::GetSocketDescriptors() const {
    std::vector<FileDescriptor> sockets;
    for (const auto& fd : file_descriptors) {
        if (fd.IsSocket()) {
            sockets.push_back(fd);
        }
    }
    return sockets;
}

std::vector<FileDescriptor> ProcessInfoExtended::GetFileDescriptors() const {
    std::vector<FileDescriptor> files;
    for (const auto& fd : file_descriptors) {
        if (fd.IsFile()) {
            files.push_back(fd);
        }
    }
    return files;
}

bool ProcessInfoExtended::HasOpenFile(const std::string& path) const {
    for (const auto& fd : file_descriptors) {
        if (fd.target == path) {
            return true;
        }
    }
    return false;
}

bool ProcessInfoExtended::HasNetworkConnection(const std::string& addr) const {
    for (const auto& conn : network_connections) {
        if (conn.remote_addr == addr || conn.local_addr == addr) {
            return true;
        }
    }
    return false;
}

}