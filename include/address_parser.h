#pragma once

#include <string>
#include <cstdint>
#include <map>

namespace Haywire {

// Address space types
enum class AddressSpace {
    NONE,      // No explicit space specified
    SHARED,    // s: Shared memory file offset (memory-backend-file)
    PHYSICAL,  // p: Guest physical address
    VIRTUAL,   // v: Virtual address (process VA)
    CRUNCHED   // c: Crunched/flattened address space
};

// Lightweight struct to pair an address with its type
// This avoids creating lots of small objects while keeping type info
struct TypedAddress {
    uint64_t value;
    AddressSpace space;
    
    TypedAddress() : value(0), space(AddressSpace::NONE) {}
    TypedAddress(uint64_t v, AddressSpace s) : value(v), space(s) {}
    
    // Convenience constructors for common cases
    static TypedAddress Shared(uint64_t v) { return TypedAddress(v, AddressSpace::SHARED); }
    static TypedAddress Physical(uint64_t v) { return TypedAddress(v, AddressSpace::PHYSICAL); }
    static TypedAddress Virtual(uint64_t v) { return TypedAddress(v, AddressSpace::VIRTUAL); }
    static TypedAddress Crunched(uint64_t v) { return TypedAddress(v, AddressSpace::CRUNCHED); }
    
    // Check if this is a valid address (has a space assigned)
    bool isValid() const { return space != AddressSpace::NONE; }
};

// Result of parsing an address expression
struct ParsedAddress {
    uint64_t address;          // The parsed address value
    AddressSpace space;        // Which address space it's in
    bool isValid;              // Whether parsing succeeded
    std::string warning;       // Warning message if any
    double confidence;         // 0.0 to 1.0 confidence in interpretation
    
    // For PID-qualified virtual addresses
    bool hasPid;
    int pid;
    
    ParsedAddress() : address(0), space(AddressSpace::NONE), 
                     isValid(false), confidence(0.0),
                     hasPid(false), pid(-1) {}
};

class AddressParser {
public:
    AddressParser();
    ~AddressParser();
    
    // Main parsing function
    ParsedAddress Parse(const std::string& input);
    
    // Parse with context (current address space, PID, etc)
    ParsedAddress Parse(const std::string& input, AddressSpace currentSpace, int currentPid = -1);
    
    // Set/get variables for expressions
    void SetVariable(const std::string& name, uint64_t value);
    uint64_t GetVariable(const std::string& name) const;
    bool HasVariable(const std::string& name) const;
    
    // Built-in variables
    void SetBuiltins(uint64_t ramBase, uint64_t stackPtr, uint64_t programCounter);
    
    // Convert address space to string prefix
    static std::string GetSpacePrefix(AddressSpace space);
    static AddressSpace ParseSpacePrefix(const std::string& prefix);
    
    // Format an address with appropriate prefix
    static std::string Format(uint64_t address, AddressSpace space);
    
private:
    // Parse different number formats
    bool ParseHexNumber(const std::string& str, uint64_t& value);
    bool ParseDecimalNumber(const std::string& str, uint64_t& value);
    bool ParseNumber(const std::string& str, uint64_t& value, bool defaultHex = true);
    
    // Parse address with optional space prefix
    ParsedAddress ParsePrefixedAddress(const std::string& input);
    
    // Parse arithmetic expressions (simple + and - for now)
    ParsedAddress ParseExpression(const std::string& input, AddressSpace currentSpace);
    
    // Helper to strip whitespace
    std::string Trim(const std::string& str);
    
    // Variables for expressions
    std::map<std::string, uint64_t> variables;
    
    // Built-in variable values
    uint64_t builtinRamBase;
    uint64_t builtinStackPtr; 
    uint64_t builtinProgramCounter;
};

// Address displayer for showing calculations
class AddressDisplayer {
public:
    struct DisplayInfo {
        std::string formula;        // The calculation formula
        std::string simplified;     // Simplified result
        std::string allSpaces;      // Same address in all spaces
        
        // Components of the address calculation
        uint64_t base;
        int64_t offset;
        int row, col;
        int bytesPerPixel;
        int stride;
    };
    
    AddressDisplayer();
    ~AddressDisplayer();
    
    // Generate display for current viewport position
    DisplayInfo GetDisplay(uint64_t address, AddressSpace space,
                           int x, int y, int stride, int bytesPerPixel);
    
    // Get the address in all spaces
    std::string GetAllSpaces(uint64_t address, AddressSpace currentSpace);
    
    // Set translation helpers
    void SetMemoryMapper(class MemoryMapper* mapper) { memoryMapper = mapper; }
    void SetQemuConnection(class QemuConnection* qemu) { qemuConnection = qemu; }
    
private:
    class MemoryMapper* memoryMapper;
    class QemuConnection* qemuConnection;
    
    // Translate between address spaces
    uint64_t TranslateAddress(uint64_t addr, AddressSpace from, AddressSpace to);
};

}  // namespace Haywire