#include "instruction_renderer.h"
#include <cstring>
#include <algorithm>

namespace Haywire {

InstructionRenderer::InstructionRenderer() 
#ifdef HAS_CAPSTONE
    : initialized(false)
#endif
{
#ifdef HAS_CAPSTONE
    // Initialize Capstone for ARM64
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) == CS_ERR_OK) {
        cs_option(handle, CS_OPT_DETAIL, CS_OPT_OFF);  // Don't need detailed info
        initialized = true;
    }
#endif
}

InstructionRenderer::~InstructionRenderer() {
#ifdef HAS_CAPSTONE
    if (initialized) {
        cs_close(&handle);
    }
#endif
}

bool InstructionRenderer::Disassemble(const uint8_t* data, uint64_t address, 
                                       InstructionInfo& out) {
#ifdef HAS_CAPSTONE
    if (!initialized) {
        out.valid = false;
        return false;
    }
    
    cs_insn* insn = nullptr;
    size_t count = cs_disasm(handle, data, 4, address, 1, &insn);
    
    if (count > 0) {
        // Successfully disassembled
        out.address = insn[0].address;
        std::memcpy(out.bytes, insn[0].bytes, 4);
        out.mnemonic = insn[0].mnemonic;
        out.operands = insn[0].op_str;
        out.category = Classify(insn[0].mnemonic);
        out.valid = true;
        
        cs_free(insn, count);
        return true;
    } else {
        // Disassembly failed - treat as invalid/garbage
        out.address = address;
        std::memcpy(out.bytes, data, 4);
        out.mnemonic = "???";
        out.operands = "";
        out.category = InstructionCategory::INVALID;
        out.valid = false;
        
        return false;
    }
#else
    // No Capstone - return invalid
    out.address = address;
    std::memcpy(out.bytes, data, 4);
    out.mnemonic = "NO_CAPSTONE";
    out.operands = "";
    out.category = InstructionCategory::INVALID;
    out.valid = false;
    return false;
#endif
}

std::vector<InstructionInfo> InstructionRenderer::DisassembleRange(
    const uint8_t* data, size_t size, uint64_t baseAddress
) {
    std::vector<InstructionInfo> results;
    results.reserve(size / 4);
    
    for (size_t offset = 0; offset + 4 <= size; offset += 4) {
        InstructionInfo info;
        Disassemble(data + offset, baseAddress + offset, info);
        results.push_back(info);
    }
    
    return results;
}

InstructionCategory InstructionRenderer::Classify(const char* mnemonic) {
    if (!mnemonic || mnemonic[0] == '\0') {
        return InstructionCategory::INVALID;
    }
    
    // Data movement
    if (std::strncmp(mnemonic, "mov", 3) == 0 || 
        std::strncmp(mnemonic, "mvn", 3) == 0) {
        return InstructionCategory::DATA_MOVE;
    }
    
    // Loads
    if (std::strncmp(mnemonic, "ldr", 3) == 0 || 
        std::strncmp(mnemonic, "ldp", 3) == 0 ||
        std::strncmp(mnemonic, "ldur", 4) == 0 ||
        std::strncmp(mnemonic, "ldar", 4) == 0) {
        return InstructionCategory::LOAD;
    }
    
    // Stores
    if (std::strncmp(mnemonic, "str", 3) == 0 || 
        std::strncmp(mnemonic, "stp", 3) == 0 ||
        std::strncmp(mnemonic, "stur", 4) == 0 ||
        std::strncmp(mnemonic, "stlr", 4) == 0) {
        return InstructionCategory::STORE;
    }
    
    // Arithmetic
    if (std::strncmp(mnemonic, "add", 3) == 0 || 
        std::strncmp(mnemonic, "sub", 3) == 0 ||
        std::strncmp(mnemonic, "mul", 3) == 0 ||
        std::strncmp(mnemonic, "div", 3) == 0 ||
        std::strncmp(mnemonic, "madd", 4) == 0 ||
        std::strncmp(mnemonic, "msub", 4) == 0) {
        return InstructionCategory::ARITHMETIC;
    }
    
    // Logic
    if (std::strncmp(mnemonic, "and", 3) == 0 || 
        std::strncmp(mnemonic, "orr", 3) == 0 ||
        std::strncmp(mnemonic, "eor", 3) == 0 ||
        std::strncmp(mnemonic, "bic", 3) == 0) {
        return InstructionCategory::LOGIC;
    }
    
    // Branches
    if (mnemonic[0] == 'b' && 
        (mnemonic[1] == '\0' || mnemonic[1] == '.' || mnemonic[1] == 'l' || 
         mnemonic[1] == 'r')) {
        return InstructionCategory::BRANCH;
    }
    if (std::strncmp(mnemonic, "ret", 3) == 0 ||
        std::strncmp(mnemonic, "cbz", 3) == 0 ||
        std::strncmp(mnemonic, "cbnz", 4) == 0 ||
        std::strncmp(mnemonic, "tbz", 3) == 0 ||
        std::strncmp(mnemonic, "tbnz", 4) == 0) {
        return InstructionCategory::BRANCH;
    }
    
    // Compares
    if (std::strncmp(mnemonic, "cmp", 3) == 0 || 
        std::strncmp(mnemonic, "cmn", 3) == 0 ||
        std::strncmp(mnemonic, "tst", 3) == 0) {
        return InstructionCategory::COMPARE;
    }
    
    // SIMD/FP (starts with 'f' or 'v')
    if (mnemonic[0] == 'f' || mnemonic[0] == 'v') {
        return InstructionCategory::SIMD_FP;
    }
    
    // System
    if (std::strncmp(mnemonic, "msr", 3) == 0 || 
        std::strncmp(mnemonic, "mrs", 3) == 0 ||
        std::strncmp(mnemonic, "svc", 3) == 0 ||
        std::strncmp(mnemonic, "nop", 3) == 0) {
        return InstructionCategory::SYSTEM;
    }
    
    // Everything else
    return InstructionCategory::DATA_MOVE;  // Default fallback
}

uint32_t InstructionRenderer::GetCategoryColor(InstructionCategory cat) {
    switch (cat) {
        case InstructionCategory::DATA_MOVE:  return 0xFF3C78DC;  // Blue
        case InstructionCategory::ARITHMETIC: return 0xFF32C8DC;  // Yellow
        case InstructionCategory::LOGIC:      return 0xFF50C8C8;  // Cyan
        case InstructionCategory::LOAD:       return 0xFF32C878;  // Green
        case InstructionCategory::STORE:      return 0xFF3C96FF;  // Orange
        case InstructionCategory::BRANCH:     return 0xFF3232DC;  // Red
        case InstructionCategory::COMPARE:    return 0xFFC832DC;  // Magenta
        case InstructionCategory::SIMD_FP:    return 0xFFA050DC;  // Purple
        case InstructionCategory::SYSTEM:     return 0xFF808080;  // Gray
        case InstructionCategory::INVALID:    return 0xFF404040;  // Dark gray
        default:                              return 0xFF808080;
    }
}

// 4×6 pixel icon glyphs
static const uint8_t ICON_MOVE[6] = {
    0b0000, 0b0010, 0b1111, 0b1111, 0b0010, 0b0000  // →
};

static const uint8_t ICON_ADD[6] = {
    0b0000, 0b0100, 0b1110, 0b0100, 0b0000, 0b0000  // +
};

static const uint8_t ICON_LOGIC[6] = {
    0b0000, 0b1001, 0b0110, 0b0110, 0b1001, 0b0000  // &
};

static const uint8_t ICON_LOAD[6] = {
    0b0100, 0b0100, 0b0100, 0b1110, 0b0100, 0b0000  // ↓
};

static const uint8_t ICON_STORE[6] = {
    0b0000, 0b0100, 0b1110, 0b0100, 0b0100, 0b0100  // ↑
};

static const uint8_t ICON_BRANCH[6] = {
    0b0001, 0b0011, 0b1111, 0b0010, 0b0100, 0b1000  // ⤴
};

static const uint8_t ICON_COMPARE[6] = {
    0b0110, 0b0001, 0b0010, 0b0000, 0b0010, 0b0000  // ?
};

static const uint8_t ICON_SIMD[6] = {
    0b0000, 0b1010, 0b0101, 0b1010, 0b0101, 0b0000  // ~
};

static const uint8_t ICON_SYSTEM[6] = {
    0b1110, 0b1010, 0b1110, 0b1010, 0b1110, 0b0000  // ⚙
};

static const uint8_t ICON_INVALID[6] = {
    0b1111, 0b1001, 0b1001, 0b1001, 0b1111, 0b0000  // □ (box)
};

const uint8_t* InstructionRenderer::GetCategoryIcon(InstructionCategory cat) {
    switch (cat) {
        case InstructionCategory::DATA_MOVE:  return ICON_MOVE;
        case InstructionCategory::ARITHMETIC: return ICON_ADD;
        case InstructionCategory::LOGIC:      return ICON_LOGIC;
        case InstructionCategory::LOAD:       return ICON_LOAD;
        case InstructionCategory::STORE:      return ICON_STORE;
        case InstructionCategory::BRANCH:     return ICON_BRANCH;
        case InstructionCategory::COMPARE:    return ICON_COMPARE;
        case InstructionCategory::SIMD_FP:    return ICON_SIMD;
        case InstructionCategory::SYSTEM:     return ICON_SYSTEM;
        case InstructionCategory::INVALID:    return ICON_INVALID;
        default:                              return ICON_INVALID;
    }
}

}
