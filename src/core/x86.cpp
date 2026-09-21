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

namespace {

// The low nibble of a Jcc, SETcc or CMOVcc opcode, as the Intel manual's `tttn`.
constexpr uint8_t kConditionBelow = 0x2, kConditionAboveOrEqual = 0x3;
constexpr uint8_t kConditionEqual = 0x4, kConditionNotEqual = 0x5;
constexpr uint8_t kConditionBelowOrEqual = 0x6, kConditionAbove = 0x7;
constexpr uint8_t kConditionLess = 0xC, kConditionGreaterOrEqual = 0xD;
constexpr uint8_t kConditionLessOrEqual = 0xE, kConditionGreater = 0xF;

Condition FromConditionCode(uint8_t code) {
    switch (code) {
    case kConditionBelow:
    case kConditionAboveOrEqual:
    case kConditionBelowOrEqual:
    case kConditionAbove:
    case kConditionLess:
    case kConditionGreaterOrEqual:
    case kConditionLessOrEqual:
    case kConditionGreater: return Condition::Ordering;
    case kConditionEqual:
    case kConditionNotEqual: return Condition::Equality;
    default: return Condition::Other;
    }
}

// Opcodes that write the flags, which ends a comparison's reach.
bool WritesFlags(const hde64s& decoded) {
    if (decoded.opcode == 0x0F)
        return false;
    // cmp, test, add, sub, and, or, xor in their common forms, and the
    // immediate groups 0x80 to 0x83.
    switch (decoded.opcode & 0xFC) {
    case 0x00: case 0x08: case 0x10: case 0x18:
    case 0x20: case 0x28: case 0x30: case 0x38: return true;
    default: break;
    }
    switch (decoded.opcode) {
    case 0x80: case 0x81: case 0x83: case 0x84: case 0x85:
    case 0x3C: case 0x3D: case 0xA8: case 0xA9: return true;
    default: return false;
    }
}

} // namespace

Condition ConditionTested(const Instruction& instruction) {
    hde64s decoded{};
    if (!Decoded(instruction.address, decoded))
        return Condition::NotConditional;
    // Jcc rel8.
    if (decoded.opcode >= 0x70 && decoded.opcode <= 0x7F)
        return FromConditionCode(decoded.opcode & 0x0F);
    if (decoded.opcode != 0x0F)
        return Condition::NotConditional;
    // Jcc rel32, SETcc and CMOVcc share the low nibble as the condition.
    const uint8_t second = decoded.opcode2;
    if ((second >= 0x80 && second <= 0x8F) || (second >= 0x90 && second <= 0x9F) ||
        (second >= 0x40 && second <= 0x4F))
        return FromConditionCode(second & 0x0F);
    return Condition::NotConditional;
}

std::optional<Instruction> FirstFlagConsumer(const std::byte* start, int limit) {
    const std::byte* cursor = start;
    for (int i = 0; i < limit; ++i) {
        const auto instruction = Decode(cursor);
        if (!instruction)
            return std::nullopt;
        if (ConditionTested(*instruction) != Condition::NotConditional)
            return instruction;
        hde64s decoded{};
        if (!Decoded(cursor, decoded) || WritesFlags(decoded))
            return std::nullopt;
        cursor = instruction->Next();
    }
    return std::nullopt;
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
