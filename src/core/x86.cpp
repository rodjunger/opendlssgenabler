#include "core/x86.h"

#include "core/pe.h"

#include <hde/hde64.h>

#include <cstring>

namespace odg::x86 {
namespace {

// ModRM `mod` field values, and the `rm` value that means an operand relative
// to the next instruction when `mod` is 00.
constexpr uint8_t kModIndirect = 0;
constexpr uint8_t kModDisplacement8 = 1;
constexpr uint8_t kModDisplacement32 = 2;
constexpr uint8_t kRmRipRelative = 5;

// Primary opcodes, and the ModRM `reg` values that select an operation within
// an opcode group.
constexpr uint8_t kOpcodeMovByteImmediate = 0xC6; // group 11: mov r/m8, imm8
constexpr uint8_t kGroupMov = 0;
constexpr uint8_t kOpcodeJmpRel32 = 0xE9;
constexpr uint8_t kOpcodeGroup5 = 0xFF; // group 5: inc, dec, call, jmp...
constexpr uint8_t kGroupJmpIndirect = 4;

bool Decoded(const std::byte* address, hde64s& decoded) {
    if (!address)
        return false;
    hde64_disasm(address, &decoded);
    return decoded.len != 0 && !(decoded.flags & F_ERROR);
}

int32_t Displacement(const hde64s& decoded) {
    if (decoded.flags & F_DISP32)
        return static_cast<int32_t>(decoded.disp.disp32);
    if (decoded.flags & F_DISP8)
        return static_cast<int8_t>(decoded.disp.disp8);
    return 0;
}

} // namespace

std::optional<Instruction> Decode(const std::byte* address) {
    hde64s decoded{};
    if (!Decoded(address, decoded))
        return std::nullopt;
    return Instruction{address, decoded.len};
}

std::optional<ByteStore> AsByteStore(const Instruction& instruction) {
    hde64s decoded{};
    if (!Decoded(instruction.address, decoded))
        return std::nullopt;
    const bool store = decoded.opcode == kOpcodeMovByteImmediate &&
                       (decoded.flags & F_MODRM) && decoded.modrm_reg == kGroupMov &&
                       (decoded.modrm_mod == kModDisplacement8 ||
                        decoded.modrm_mod == kModDisplacement32) &&
                       !(decoded.flags & F_SIB) && (decoded.flags & F_IMM8);
    if (!store)
        return std::nullopt;
    return ByteStore{Displacement(decoded), decoded.imm.imm8};
}

std::optional<const std::byte*> RipRelativeOperand(const Instruction& instruction) {
    hde64s decoded{};
    if (!Decoded(instruction.address, decoded) || !(decoded.flags & F_MODRM) ||
        decoded.modrm_mod != kModIndirect || decoded.modrm_rm != kRmRipRelative)
        return std::nullopt;
    return instruction.Next() + Displacement(decoded);
}

std::optional<const std::byte*> JumpTarget(const Instruction& instruction) {
    hde64s decoded{};
    if (!Decoded(instruction.address, decoded))
        return std::nullopt;

    if (decoded.opcode == kOpcodeJmpRel32 && (decoded.flags & F_IMM32))
        return instruction.Next() + static_cast<int32_t>(decoded.imm.imm32);

    const bool through_pointer = decoded.opcode == kOpcodeGroup5 && (decoded.flags & F_MODRM) &&
                                 decoded.modrm_reg == kGroupJmpIndirect &&
                                 decoded.modrm_mod == kModIndirect &&
                                 decoded.modrm_rm == kRmRipRelative;
    if (!through_pointer)
        return std::nullopt;
    const std::byte* slot = instruction.Next() + Displacement(decoded);
    const std::byte* target = nullptr;
    if (!pe::SafeCopy(&target, slot, sizeof(target)) || !target)
        return std::nullopt;
    return target;
}

} // namespace odg::x86
