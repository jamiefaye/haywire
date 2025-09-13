#include <iostream>
#include <iomanip>
#include <vector>
#include <set>
#include <cstring>
#include "memory_backend.h"

using namespace Haywire;

// Linux task_struct has tasks.next and tasks.prev pointers forming a circular list
// We're looking for this pattern in memory

struct ListCandidate {
    uint64_t addr;
    uint64_t next;
    uint64_t prev;
    int chain_length;
    bool is_circular;
    std::vector<uint64_t> nodes;
};

class ProcessListFinder {
public:
    ProcessListFinder(MemoryBackend* mem) : memory(mem) {}
    
    // Look for circular doubly-linked lists in kernel memory
    std::vector<ListCandidate> FindCircularLists() {
        std::vector<ListCandidate> candidates;
        
        std::cout << "Scanning for circular doubly-linked lists..." << std::endl;
        
        // Scan likely kernel memory region (1GB - 2GB physical)
        for (uint64_t addr = 0x40000000; addr < 0x80000000; addr += 0x1000) {
            // Read a page
            std::vector<uint8_t> page;
            if (!memory->Read(addr, 0x1000, page) || page.size() != 0x1000) {
                continue;
            }
            
            // Look for potential list nodes in this page
            for (size_t offset = 0; offset < 0x1000 - 16; offset += 8) {
                uint64_t potential_next, potential_prev;
                memcpy(&potential_next, page.data() + offset, 8);
                memcpy(&potential_prev, page.data() + offset + 8, 8);
                
                // Check if these look like kernel pointers
                if (LooksLikeKernelPointer(potential_next) && 
                    LooksLikeKernelPointer(potential_prev)) {
                    
                    // Try to follow the chain
                    ListCandidate candidate;
                    candidate.addr = addr + offset;
                    candidate.next = potential_next;
                    candidate.prev = potential_prev;
                    
                    if (ValidateLinkedList(candidate)) {
                        candidates.push_back(candidate);
                        std::cout << "Found circular list at 0x" << std::hex << candidate.addr
                                  << " with " << std::dec << candidate.chain_length << " nodes" << std::endl;
                    }
                }
            }
            
            // Progress indicator
            if ((addr & 0xFFFFFF) == 0) {
                std::cout << "Scanned up to 0x" << std::hex << addr << "\r" << std::flush;
            }
        }
        
        std::cout << std::endl;
        return candidates;
    }
    
    // Validate that this is actually a process list by checking for task_struct signatures
    bool ValidateAsProcessList(const ListCandidate& list) {
        std::cout << "Validating list at 0x" << std::hex << list.addr << " as process list..." << std::endl;
        
        int valid_processes = 0;
        
        for (uint64_t node_addr : list.nodes) {
            // Adjust for the fact that tasks list is embedded in task_struct
            // We need to go back from the list node to the start of task_struct
            // This offset varies by kernel version, but let's try common ones
            
            std::vector<int> common_offsets = {
                0x2F8,  // Common on 5.x kernels
                0x318,  // Alternative
                0x2E8,  // Older kernels
                0x308,  // Another variant
            };
            
            for (int offset : common_offsets) {
                uint64_t potential_task = node_addr - offset;
                
                // Try to read what might be a task_struct
                std::vector<uint8_t> task_data;
                if (!memory->Read(potential_task, 0x600, task_data) || task_data.size() != 0x600) {
                    continue;
                }
                
                // Look for signatures of a task_struct:
                // 1. PID should be reasonable (0-65535)
                // 2. comm field should be printable ASCII
                // 3. There should be valid pointers
                
                // Check for comm field (usually around offset 0x550)
                char comm[17] = {0};
                memcpy(comm, task_data.data() + 0x550, 16);
                
                if (IsPrintableString(comm)) {
                    // Check for PID (usually around offset 0x398)
                    uint32_t pid;
                    memcpy(&pid, task_data.data() + 0x398, 4);
                    
                    if (pid < 65536) {
                        std::cout << "  Found process: PID=" << pid << " comm=" << comm << std::endl;
                        valid_processes++;
                        
                        // Special check: PID 0 should be "swapper"
                        if (pid == 0 && strstr(comm, "swapper") != nullptr) {
                            std::cout << "  *** Found init_task! ***" << std::endl;
                            return true;  // Definitely the process list!
                        }
                    }
                }
            }
        }
        
        // If we found multiple valid processes, it's probably the process list
        return valid_processes >= 3;
    }
    
private:
    MemoryBackend* memory;
    
    bool LooksLikeKernelPointer(uint64_t ptr) {
        // ARM64 kernel virtual addresses typically start with 0xffff
        // Physical addresses for kernel are typically 0x40000000 - 0x100000000
        
        // Check for kernel virtual address
        if ((ptr & 0xFFFF000000000000) == 0xFFFF000000000000) {
            return true;
        }
        
        // Check for physical address in kernel range
        if (ptr >= 0x40000000 && ptr < 0x100000000) {
            return true;
        }
        
        return false;
    }
    
    bool ValidateLinkedList(ListCandidate& candidate) {
        std::set<uint64_t> visited;
        uint64_t current = candidate.addr;
        
        // Follow the 'next' pointers
        for (int i = 0; i < 1000; i++) {  // Max 1000 processes
            if (visited.count(current)) {
                // We've looped back!
                candidate.is_circular = (current == candidate.addr);
                candidate.chain_length = visited.size();
                
                // Copy visited nodes
                candidate.nodes.assign(visited.begin(), visited.end());
                
                // A process list typically has 50-500 entries
                if (candidate.is_circular && candidate.chain_length >= 10 && 
                    candidate.chain_length <= 500) {
                    
                    // Also verify backward links
                    return VerifyBackwardLinks(candidate);
                }
                return false;
            }
            
            visited.insert(current);
            
            // Read next pointer
            std::vector<uint8_t> ptr_data;
            if (!memory->Read(current, 8, ptr_data) || ptr_data.size() != 8) {
                return false;
            }
            
            uint64_t next;
            memcpy(&next, ptr_data.data(), 8);
            
            if (!LooksLikeKernelPointer(next)) {
                return false;
            }
            
            current = next;
        }
        
        return false;
    }
    
    bool VerifyBackwardLinks(const ListCandidate& candidate) {
        // Verify that following 'prev' pointers also forms the same circular list
        uint64_t current = candidate.addr;
        
        for (size_t i = 0; i < candidate.nodes.size(); i++) {
            // Read prev pointer (8 bytes after next)
            std::vector<uint8_t> ptr_data;
            if (!memory->Read(current + 8, 8, ptr_data) || ptr_data.size() != 8) {
                return false;
            }
            
            uint64_t prev;
            memcpy(&prev, ptr_data.data(), 8);
            
            // Read the 'next' pointer of the previous node
            std::vector<uint8_t> prev_next_data;
            if (!memory->Read(prev, 8, prev_next_data) || prev_next_data.size() != 8) {
                return false;
            }
            
            uint64_t prev_next;
            memcpy(&prev_next, prev_next_data.data(), 8);
            
            // The previous node's next should point back to us
            if (prev_next != current) {
                return false;
            }
            
            current = prev;
        }
        
        return true;
    }
    
    bool IsPrintableString(const char* str) {
        for (int i = 0; i < 16 && str[i]; i++) {
            if (str[i] < 32 || str[i] > 126) {
                return false;
            }
        }
        return str[0] != 0;  // Not empty
    }
};

int main() {
    // Connect to memory backend
    MemoryBackend mem;
    if (!mem.AutoDetect()) {
        std::cerr << "Failed to detect memory backend" << std::endl;
        return 1;
    }
    
    std::cout << "Connected to memory backend" << std::endl;
    
    ProcessListFinder finder(&mem);
    
    // Find all circular doubly-linked lists
    auto candidates = finder.FindCircularLists();
    
    std::cout << "\nFound " << candidates.size() << " circular linked lists" << std::endl;
    
    // Check each one to see if it's the process list
    for (const auto& candidate : candidates) {
        if (finder.ValidateAsProcessList(candidate)) {
            std::cout << "\n*** FOUND PROCESS LIST! ***" << std::endl;
            std::cout << "List head at: 0x" << std::hex << candidate.addr << std::endl;
            std::cout << "Number of processes: " << std::dec << candidate.chain_length << std::endl;
            
            // This is probably init_task.tasks!
            // The actual init_task struct starts earlier
            break;
        }
    }
    
    return 0;
}