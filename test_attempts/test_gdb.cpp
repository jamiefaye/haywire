#include <iostream>
#include <iomanip>
#include "gdb_connection.h"

using namespace Haywire;

int main() {
    GDBConnection gdb;
    
    std::cout << "Connecting to GDB server at localhost:1234..." << std::endl;
    if (!gdb.Connect("localhost", 1234)) {
        std::cerr << "Failed to connect to GDB server" << std::endl;
        std::cerr << "Make sure QEMU is running with -gdb tcp::1234" << std::endl;
        return 1;
    }
    
    std::cout << "Connected to GDB!" << std::endl;
    
    // Try to read registers
    std::string registers;
    if (gdb.ReadRegisters(registers)) {
        std::cout << "Register data (first 128 bytes): " << std::endl;
        for (size_t i = 0; i < std::min(size_t(128), registers.size()); i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') 
                      << (int)(unsigned char)registers[i];
            if ((i + 1) % 16 == 0) std::cout << std::endl;
        }
        std::cout << std::dec << std::endl;
    } else {
        std::cerr << "Failed to read registers" << std::endl;
    }
    
    // Try to read memory at a known location
    std::cout << "\nTrying to read memory at 0x40000000..." << std::endl;
    std::vector<uint8_t> buffer;
    if (gdb.ReadMemory(0x40000000, 64, buffer)) {
        std::cout << "Memory read successful! First 64 bytes:" << std::endl;
        for (size_t i = 0; i < buffer.size(); i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') 
                      << (int)buffer[i];
            if ((i + 1) % 16 == 0) std::cout << std::endl;
            else std::cout << " ";
        }
        std::cout << std::dec << std::endl;
    } else {
        std::cerr << "Failed to read memory" << std::endl;
    }
    
    gdb.Disconnect();
    std::cout << "\nDisconnected from GDB" << std::endl;
    
    return 0;
}