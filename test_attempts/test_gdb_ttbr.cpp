#include <iostream>
#include <iomanip>
#include <sstream>
#include "gdb_connection.h"

using namespace Haywire;

// Helper to parse hex string to uint64_t
uint64_t parseHex(const std::string& hex) {
    uint64_t value = 0;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> value;
    return value;
}

int main() {
    GDBConnection gdb;
    
    std::cout << "Connecting to GDB server to read TTBR..." << std::endl;
    if (!gdb.Connect("localhost", 1234)) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    
    std::cout << "Connected!" << std::endl;
    
    // On ARM64, system registers aren't directly accessible via standard GDB protocol
    // But we can try some approaches:
    
    // 1. Try to read the general purpose registers
    std::cout << "\n1. Reading general registers..." << std::endl;
    std::string regs;
    if (gdb.ReadRegisters(regs)) {
        std::cout << "Got " << regs.size() << " bytes of register data" << std::endl;
        
        // ARM64 has 31 general purpose registers (X0-X30) + SP, PC, CPSR
        // Each is 8 bytes, so we expect at least 34*8 = 272 bytes
        if (regs.size() >= 272) {
            // Display first few registers
            for (int i = 0; i < 5; i++) {
                uint64_t reg = 0;
                for (int j = 0; j < 8; j++) {
                    reg |= ((uint64_t)(unsigned char)regs[i*8 + j]) << (j*8);
                }
                std::cout << "  X" << i << " = 0x" << std::hex << reg << std::dec << std::endl;
            }
            
            // SP is at position 31
            uint64_t sp = 0;
            for (int j = 0; j < 8; j++) {
                sp |= ((uint64_t)(unsigned char)regs[31*8 + j]) << (j*8);
            }
            std::cout << "  SP = 0x" << std::hex << sp << std::dec << std::endl;
            
            // PC is at position 32
            uint64_t pc = 0;
            for (int j = 0; j < 8; j++) {
                pc |= ((uint64_t)(unsigned char)regs[32*8 + j]) << (j*8);
            }
            std::cout << "  PC = 0x" << std::hex << pc << std::dec << std::endl;
        }
    }
    
    // 2. Try to read memory at common page table locations
    std::cout << "\n2. Scanning for page tables in physical memory..." << std::endl;
    
    // Common locations for ARM64 page tables
    std::vector<uint64_t> common_ttbr_locations = {
        0x40000000,  // 1GB
        0x41000000,  
        0x42000000,
        0x43000000,
        0x44000000,
        0x48000000,
        0x50000000,
        0x60000000,
        0x70000000,
        0x80000000,  // 2GB
    };
    
    for (uint64_t addr : common_ttbr_locations) {
        std::vector<uint8_t> buffer;
        if (gdb.ReadMemory(addr, 64, buffer) && buffer.size() == 64) {
            // Check if this looks like a page table
            // Page tables have entries that are 8 bytes each
            // Valid entries have bit 0 set (valid bit)
            
            int valid_entries = 0;
            for (size_t i = 0; i < 8; i++) {
                uint64_t entry = 0;
                for (int j = 0; j < 8; j++) {
                    entry |= ((uint64_t)buffer[i*8 + j]) << (j*8);
                }
                if (entry & 1) {  // Valid bit
                    valid_entries++;
                }
            }
            
            if (valid_entries > 0) {
                std::cout << "  Possible page table at 0x" << std::hex << addr 
                          << " (" << std::dec << valid_entries << " valid entries)" << std::endl;
                
                // Show first entry
                uint64_t first_entry = 0;
                for (int j = 0; j < 8; j++) {
                    first_entry |= ((uint64_t)buffer[j]) << (j*8);
                }
                std::cout << "    First entry: 0x" << std::hex << first_entry << std::dec << std::endl;
            }
        }
    }
    
    // 3. Try to use monitor commands (QEMU specific)
    std::cout << "\n3. Trying QEMU monitor commands..." << std::endl;
    
    // Note: This would need to be implemented in GDBConnection
    // The GDB protocol supports "qRcmd" for remote commands
    // We'd send: $qRcmd,696e666f207265676973746572732073797374656d#XX
    // Which is "info registers system" in hex
    
    gdb.Disconnect();
    std::cout << "\nDisconnected" << std::endl;
    
    // Summary
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "GDB can read memory and general registers, but not system registers directly." << std::endl;
    std::cout << "To get TTBR, we need to either:" << std::endl;
    std::cout << "1. Find page tables by scanning memory patterns" << std::endl;
    std::cout << "2. Use QEMU monitor commands via GDB" << std::endl;
    std::cout << "3. Use the guest agent with sudo access" << std::endl;
    
    return 0;
}