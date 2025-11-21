/**
 * Test VirtualBox COM API on Windows
 *
 * This program tests if we can access guest physical memory via VirtualBox's
 * IMachineDebugger::ReadPhysicalMemory() method on Windows.
 *
 * Compile with:
 *   cl /EHsc test_vbox_com.cpp /I"C:\Program Files\Oracle\VirtualBox\sdk\bindings\mscom\include"
 *
 * Or use CMake (see test_vbox_com_CMakeLists.txt)
 */

#include <windows.h>
#include <comdef.h>
#include <iostream>
#include <vector>
#include <iomanip>

// VirtualBox COM includes
#import "C:\\Program Files\\Oracle\\VirtualBox\\VBoxC.dll" no_namespace

void PrintHexDump(const BYTE* data, size_t size, uint64_t baseAddr) {
    for (size_t i = 0; i < size && i < 64; i += 16) {
        std::cout << "  " << std::hex << std::setw(8) << std::setfill('0')
                  << (baseAddr + i) << ":  ";

        // Hex bytes
        for (size_t j = 0; j < 16 && (i + j) < size; j++) {
            std::cout << std::setw(2) << std::setfill('0')
                      << (int)data[i + j] << " ";
        }

        // ASCII
        std::cout << " ";
        for (size_t j = 0; j < 16 && (i + j) < size; j++) {
            char c = data[i + j];
            std::cout << (c >= 32 && c < 127 ? c : '.');
        }
        std::cout << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: test_vbox_com <vm_name> [address] [size]" << std::endl;
        std::cerr << "Example: test_vbox_com Ubuntu-x86_64-Haywire 0x0 4096" << std::endl;
        return 1;
    }

    const char* vmName = argv[1];
    uint64_t testAddr = (argc > 2) ? _strtoui64(argv[2], nullptr, 0) : 0x0;  // x86_64 default
    uint32_t testSize = (argc > 3) ? strtoul(argv[3], nullptr, 0) : 4096;

    std::cout << "========================================" << std::endl;
    std::cout << "VirtualBox COM API Memory Test" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "VM Name: " << vmName << std::endl;
    std::cout << "Address: 0x" << std::hex << testAddr << std::endl;
    std::cout << "Size: " << std::dec << testSize << " bytes" << std::endl;
    std::cout << std::endl;

    HRESULT hr;

    // Initialize COM
    hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        std::cerr << "ERROR: Failed to initialize COM: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    std::cout << "✓ COM initialized" << std::endl;

    try {
        // Create VirtualBox instance
        IVirtualBoxPtr vbox;
        hr = vbox.CreateInstance(__uuidof(VirtualBox));
        if (FAILED(hr)) {
            std::cerr << "ERROR: Failed to create VirtualBox instance: 0x"
                      << std::hex << hr << std::endl;
            CoUninitialize();
            return 1;
        }

        std::cout << "✓ VirtualBox instance created" << std::endl;

        // Get VirtualBox version
        BSTR versionBstr;
        vbox->get_Version(&versionBstr);
        _bstr_t version(versionBstr, false);
        std::cout << "  Version: " << (const char*)version << std::endl;
        std::cout << std::endl;

        // Find VM
        std::cout << "Finding VM '" << vmName << "'..." << std::endl;
        _bstr_t vmNameBstr(vmName);
        IMachinePtr machine;
        machine = vbox->FindMachine(vmNameBstr);
        if (!machine) {
            hr = E_FAIL;
            std::cerr << "ERROR: VM not found: 0x" << std::hex << hr << std::endl;
            std::cerr << std::endl;
            std::cerr << "Available VMs:" << std::endl;

            // List available VMs
            SAFEARRAY* machinesArray;
            vbox->get_Machines(&machinesArray);
            IMachine** machines = (IMachine**)machinesArray->pvData;
            LONG count = machinesArray->rgsabound[0].cElements;

            for (LONG i = 0; i < count; i++) {
                BSTR name;
                machines[i]->get_Name(&name);
                _bstr_t nameStr(name, false);
                std::cerr << "  - " << (const char*)nameStr << std::endl;
            }

            SafeArrayDestroy(machinesArray);
            CoUninitialize();
            return 1;
        }

        std::cout << "✓ VM found" << std::endl;

        // Check VM state
        MachineState state;
        machine->get_State(&state);
        std::cout << "  State: " << (int)state
                  << (state == MachineState_Running ? " (Running)" : " (Not running)")
                  << std::endl;

        if (state != MachineState_Running) {
            std::cerr << std::endl;
            std::cerr << "ERROR: VM is not running!" << std::endl;
            std::cerr << "Start it with: VBoxManage startvm \"" << vmName << "\" --type headless" << std::endl;
            CoUninitialize();
            return 1;
        }
        std::cout << std::endl;

        // Open session
        std::cout << "Opening session..." << std::endl;
        ISessionPtr session;
        hr = session.CreateInstance(__uuidof(Session));
        if (FAILED(hr)) {
            std::cerr << "ERROR: Failed to create session: 0x" << std::hex << hr << std::endl;
            CoUninitialize();
            return 1;
        }

        hr = machine->LockMachine(session, LockType_Shared);
        if (FAILED(hr)) {
            std::cerr << "ERROR: Failed to lock machine: 0x" << std::hex << hr << std::endl;
            CoUninitialize();
            return 1;
        }

        std::cout << "✓ Session opened (Shared lock)" << std::endl;

        // Get console
        IConsolePtr console;
        hr = session->get_Console(&console);
        if (FAILED(hr)) {
            std::cerr << "ERROR: Failed to get console: 0x" << std::hex << hr << std::endl;
            session->UnlockMachine();
            CoUninitialize();
            return 1;
        }

        std::cout << "✓ Console obtained" << std::endl;

        // Get machine debugger
        IMachineDebuggerPtr debugger;
        hr = console->get_Debugger(&debugger);
        if (FAILED(hr)) {
            std::cerr << "ERROR: Failed to get debugger: 0x" << std::hex << hr << std::endl;
            session->UnlockMachine();
            CoUninitialize();
            return 1;
        }

        std::cout << "✓ Debugger obtained" << std::endl;
        std::cout << std::endl;

        // Try to read physical memory
        std::cout << "========================================" << std::endl;
        std::cout << "Testing ReadPhysicalMemory..." << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "Address: 0x" << std::hex << testAddr << std::endl;
        std::cout << "Size: " << std::dec << testSize << " bytes" << std::endl;
        std::cout << std::endl;

        SAFEARRAY* dataArray = nullptr;
        try {
            dataArray = debugger->ReadPhysicalMemory(testAddr, testSize);
            hr = S_OK;
        } catch (_com_error& e) {
            hr = e.Error();
            dataArray = nullptr;
        }

        if (SUCCEEDED(hr) && dataArray) {
            std::cout << "✅ SUCCESS! ReadPhysicalMemory IS IMPLEMENTED!" << std::endl;
            std::cout << std::endl;

            // Extract data from SAFEARRAY
            BYTE* data = (BYTE*)dataArray->pvData;
            LONG dataSize = dataArray->rgsabound[0].cElements;

            std::cout << "Read " << dataSize << " bytes from 0x"
                      << std::hex << testAddr << std::endl;
            std::cout << std::endl;
            std::cout << "First 64 bytes:" << std::endl;
            PrintHexDump(data, dataSize, testAddr);

            SafeArrayDestroy(dataArray);

            std::cout << std::endl;
            std::cout << "🎉 VirtualBox memory bridge is FEASIBLE!" << std::endl;
            std::cout << "   We can use COM API without recompiling VirtualBox." << std::endl;

        } else {
            std::cerr << "❌ FAILED! ReadPhysicalMemory NOT implemented" << std::endl;
            std::cerr << std::endl;
            std::cerr << "Error code: 0x" << std::hex << hr << std::endl;

            if (hr == E_NOTIMPL || hr == 0x80004001) {
                std::cerr << std::endl;
                std::cerr << "The method exists but returns NotImplemented." << std::endl;
                std::cerr << "This is VirtualBox ticket #10222." << std::endl;
                std::cerr << std::endl;
                std::cerr << "Options:" << std::endl;
                std::cerr << "  1. Use dumpvmcore (snapshot-based)" << std::endl;
                std::cerr << "  2. Patch VirtualBox (secret range approach)" << std::endl;
                std::cerr << "  3. Continue using QEMU" << std::endl;
            }
        }

        // Cleanup
        session->UnlockMachine();
        CoUninitialize();

        return SUCCEEDED(hr) ? 0 : 1;

    } catch (_com_error& e) {
        std::cerr << "COM Error: " << (char*)e.Description() << std::endl;
        std::cerr << "Error code: 0x" << std::hex << e.Error() << std::endl;
        CoUninitialize();
        return 1;
    }
}
