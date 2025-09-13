#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <sstream>
#include <iomanip>
#include "guest_agent.h"

using namespace Haywire;

struct KcoreSegment {
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint32_t flags;
};

class KcoreParser {
private:
    GuestAgent agent;
    std::vector<KcoreSegment> segments;
    
public:
    bool Connect() {
        return agent.Connect("/tmp/qga.sock");
    }
    
    void Disconnect() {
        agent.Disconnect();
    }
    
    bool ParseKcoreHeaders() {
        std::string output;
        
        // Read ELF header
        std::cout << "Reading /proc/kcore ELF header..." << std::endl;
        if (!agent.ExecuteCommand("dd if=/proc/kcore bs=64 count=1 2>/dev/null | od -t x1 -An", output)) {
            return false;
        }
        
        // Parse program headers with readelf
        std::cout << "\nParsing program headers..." << std::endl;
        if (!agent.ExecuteCommand("readelf -l /proc/kcore 2>/dev/null | grep LOAD", output)) {
            std::cerr << "Failed to read program headers" << std::endl;
            return false;
        }
        
        // Parse the LOAD segments
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("LOAD") != std::string::npos) {
                // Example line:
                // LOAD           0x0000000000001000 0xffff800080000000 0x0000000040000000
                //                0x0000003fc0000000 0x0000003fc0000000  RWE    0x1000
                std::cout << "Segment: " << line << std::endl;
                
                // Try to extract virtual address after LOAD
                size_t pos = line.find("0x");
                if (pos != std::string::npos) {
                    std::string vaddr_str = line.substr(pos, 18);
                    
                    // Find next 0x for paddr  
                    pos = line.find("0x", pos + 1);
                    if (pos != std::string::npos) {
                        std::string paddr_str = line.substr(pos, 18);
                        
                        KcoreSegment seg;
                        seg.vaddr = std::stoull(vaddr_str, nullptr, 16);
                        seg.paddr = std::stoull(paddr_str, nullptr, 16);
                        segments.push_back(seg);
                        
                        std::cout << "  VA: 0x" << std::hex << seg.vaddr 
                                  << " -> PA: 0x" << seg.paddr << std::dec << std::endl;
                    }
                }
            }
        }
        
        return !segments.empty();
    }
    
    bool ReadKernelMemory(uint64_t kernel_va, size_t size) {
        // Find which segment contains this VA
        for (const auto& seg : segments) {
            if (kernel_va >= seg.vaddr && kernel_va < seg.vaddr + 0x1000000000ULL) {
                uint64_t offset = kernel_va - seg.vaddr + seg.paddr;
                
                std::cout << "Reading kernel VA 0x" << std::hex << kernel_va 
                          << " from kcore offset 0x" << offset << std::dec << std::endl;
                
                // Read using dd with skip
                std::stringstream cmd;
                cmd << "dd if=/proc/kcore bs=1 skip=" << offset 
                    << " count=" << size << " 2>/dev/null | od -t x1 -An | head -4";
                
                std::string output;
                if (agent.ExecuteCommand(cmd.str(), output)) {
                    std::cout << "Data at kernel VA 0x" << std::hex << kernel_va 
                              << ":\n" << output << std::endl;
                    return true;
                }
            }
        }
        
        std::cerr << "Kernel VA 0x" << std::hex << kernel_va 
                  << " not found in kcore segments" << std::dec << std::endl;
        return false;
    }
    
    bool FindInitTask() {
        // Try known init_task addresses for Ubuntu 6.14 kernel
        std::vector<uint64_t> init_task_addrs = {
            0xffff800083709840,  // From check_guest_symbols
            0xffff800082e00000,  // Common alternative
            0xffff800082a00000   // Another common location
        };
        
        std::cout << "\nSearching for init_task..." << std::endl;
        
        for (uint64_t addr : init_task_addrs) {
            std::cout << "\nTrying init_task at 0x" << std::hex << addr << std::dec << "..." << std::endl;
            
            // Try to read first 64 bytes of potential task_struct
            if (ReadKernelMemory(addr, 64)) {
                // Look for patterns that indicate a valid task_struct
                // The state field (at offset 0x10) should be 0 for init
                // The PID (around offset 0x590) should be 0 for init
                std::cout << "Possible init_task found at 0x" << std::hex << addr << std::dec << std::endl;
            }
        }
        
        return true;
    }
    
    bool TestDirectAccess() {
        // Test if we can read kernel memory directly through kcore
        std::cout << "\nTesting direct kernel memory access through kcore..." << std::endl;
        
        // Try to read kernel version string (usually in rodata)
        std::string output;
        if (agent.ExecuteCommand("strings /proc/kcore 2>/dev/null | grep 'Linux version' | head -1", output)) {
            if (!output.empty()) {
                std::cout << "✓ Successfully read kernel data: " << output;
                return true;
            }
        }
        
        std::cout << "Unable to read kernel data directly" << std::endl;
        return false;
    }
};

int main() {
    KcoreParser parser;
    
    if (!parser.Connect()) {
        std::cerr << "Failed to connect to guest agent" << std::endl;
        return 1;
    }
    
    std::cout << "=== Parsing /proc/kcore for kernel memory access ===" << std::endl;
    
    if (parser.ParseKcoreHeaders()) {
        std::cout << "\n✓ Successfully parsed kcore headers" << std::endl;
        
        // Test direct access
        parser.TestDirectAccess();
        
        // Try to find init_task
        parser.FindInitTask();
        
        std::cout << "\n=== Summary ===" << std::endl;
        std::cout << "kcore provides a view of kernel virtual memory as an ELF file." << std::endl;
        std::cout << "Each LOAD segment maps a kernel VA range to a file offset." << std::endl;
        std::cout << "We can read kernel memory by calculating the right offset." << std::endl;
        std::cout << "\nHowever, we still need:" << std::endl;
        std::cout << "1. The correct init_task address for this kernel" << std::endl;
        std::cout << "2. The exact struct offsets for this kernel version" << std::endl;
        std::cout << "3. Or: Modified QEMU for reliable VA->PA translation" << std::endl;
    }
    
    parser.Disconnect();
    return 0;
}