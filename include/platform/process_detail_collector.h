#pragma once

#include "platform/process_info_extended.h"
#include "platform/process_walker.h"
#include <memory>
#include <string>
#include <functional>

namespace Haywire {

class MemoryBackend;

// Collector for detailed process information
// Can work either through guest agent (in-VM) or memory introspection
class ProcessDetailCollector {
public:
    enum CollectionMethod {
        METHOD_GUEST_AGENT,     // Use guest agent to read /proc
        METHOD_MEMORY_SCAN,     // Parse kernel structures from memory
        METHOD_HYBRID           // Use both methods
    };

    ProcessDetailCollector(MemoryBackend* backend, CollectionMethod method = METHOD_MEMORY_SCAN);
    ~ProcessDetailCollector();

    // Collect extended information for a single process
    bool CollectProcessDetails(uint64_t pid, ProcessInfoExtended& info);

    // Collect extended information for all processes
    std::vector<ProcessInfoExtended> CollectAllProcessDetails();

    // Set guest agent connection (for METHOD_GUEST_AGENT)
    void SetGuestAgent(std::function<std::string(const std::string&)> execCommand);

    // Individual collectors (can be called separately)
    bool CollectMemoryMaps(uint64_t pid, std::vector<MemoryMapping>& maps);
    bool CollectFileDescriptors(uint64_t pid, std::vector<FileDescriptor>& fds);
    bool CollectProcessStatus(uint64_t pid, ProcessInfoExtended& info);
    bool CollectNetworkConnections(uint64_t pid, std::vector<NetworkConnection>& conns);
    bool CollectThreads(uint64_t pid, std::vector<ThreadInfo>& threads);
    bool CollectCommandLine(uint64_t pid, std::vector<std::string>& cmdline);
    bool CollectEnvironment(uint64_t pid, std::map<std::string, std::string>& env);

private:
    MemoryBackend* memory;
    CollectionMethod method;
    std::unique_ptr<ProcessWalker> processWalker;
    std::function<std::string(const std::string&)> guestExec;

    // Memory scanning methods (parse kernel structures)
    bool CollectMemoryMapsFromKernel(uint64_t task_addr, std::vector<MemoryMapping>& maps);
    bool CollectFileDescriptorsFromKernel(uint64_t task_addr, std::vector<FileDescriptor>& fds);

    // Guest agent methods (read from /proc)
    bool CollectMemoryMapsFromProc(uint64_t pid, std::vector<MemoryMapping>& maps);
    bool CollectFileDescriptorsFromProc(uint64_t pid, std::vector<FileDescriptor>& fds);
    bool CollectStatusFromProc(uint64_t pid, ProcessInfoExtended& info);

    // Parsing helpers
    bool ParseMapsLine(const std::string& line, MemoryMapping& map);
    bool ParseStatusFile(const std::string& content, ProcessInfoExtended& info);
    bool ParseFdLink(const std::string& link, FileDescriptor& fd);

    // Network parsing
    bool ParseTcpConnections(std::vector<NetworkConnection>& conns);
    bool ParseUdpConnections(std::vector<NetworkConnection>& conns);
    bool ParseUnixConnections(std::vector<NetworkConnection>& conns);
};

}