#pragma once

#include "common.h"
#include <vector>
#include <string>
#include <cstdint>

#ifdef HAS_CAPSTONE
#include <capstone/capstone.h>
#endif

namespace Haywire {

enum class InstructionCategory {
    DATA_MOVE,      // mov, mvn
    ARITHMETIC,     // add, sub, mul, div
    LOGIC,          // and, orr, eor
    LOAD,           // ldr, ldp
    STORE,          // str, stp
    BRANCH,         // b, bl, br, blr, ret
    COMPARE,        // cmp, cmn, tst
    SIMD_FP,        // fadd, fmul, vadd
    SYSTEM,         // msr, mrs, svc
    INVALID         // Failed to disassemble
};

struct InstructionInfo {
    uint64_t address;
    uint8_t bytes[4];       // ARM64 always 4 bytes
    std::string mnemonic;
    std::string operands;
    InstructionCategory category;
    bool valid;             // False if disassembly failed
};

class InstructionRenderer {
public:
    InstructionRenderer();
    ~InstructionRenderer();
    
    // Disassemble one instruction at address
    // Returns false if disassembly fails (garbage data)
    bool Disassemble(const uint8_t* data, uint64_t address, InstructionInfo& out);
    
    // Disassemble a range of memory
    // Invalid instructions are marked as such but still included
    std::vector<InstructionInfo> DisassembleRange(
        const uint8_t* data, size_t size, uint64_t baseAddress
    );
    
    // Classify instruction by mnemonic
    static InstructionCategory Classify(const char* mnemonic);
    
    // Get category color for rendering
    static uint32_t GetCategoryColor(InstructionCategory cat);
    
    // Get icon glyph for category (4×6 pixel bitmap)
    static const uint8_t* GetCategoryIcon(InstructionCategory cat);
    
private:
#ifdef HAS_CAPSTONE
    csh handle;
    bool initialized;
#endif
};

}
