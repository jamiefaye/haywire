// Stub implementation of InstructionRenderer for WebAssembly
// The ARM64 instruction disassembly feature isn't needed for x86_64 memory dumps

#include "instruction_renderer.h"

namespace Haywire {

InstructionRenderer::InstructionRenderer() {
    // Stub - no initialization needed
}

InstructionRenderer::~InstructionRenderer() {
    // Stub
}

bool InstructionRenderer::Disassemble(const uint8_t* code, uint64_t address, InstructionInfo& info) {
    // Stub - return invalid instruction
    info.category = InstructionCategory::Invalid;
    info.text[0] = '\0';
    return false;
}

uint32_t InstructionRenderer::GetCategoryColor(InstructionCategory cat) {
    // Stub - return gray for all
    return 0xFF808080;
}

const uint8_t* InstructionRenderer::GetCategoryIcon(InstructionCategory cat) {
    // Stub - return empty 4x6 icon
    static const uint8_t emptyIcon[24] = {0};
    return emptyIcon;
}

} // namespace Haywire
