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
constexpr uint8_t kOpcodeMovByteRegister = 0x88;  // mov r/m8, r8
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

// Jcc, SETcc and CMOVcc all encode their condition in the low nibble of the
// opcode, as the Intel manual's `tttn`. Only the orderings and the two equality
// codes are named; the rest test sign, overflow or parity.
constexpr uint8_t kConditionBelow = 0x2, kConditionAboveOrEqual = 0x3;
constexpr uint8_t kConditionEqual = 0x4, kConditionNotEqual = 0x5;
constexpr uint8_t kConditionBelowOrEqual = 0x6, kConditionAbove = 0x7;
constexpr uint8_t kConditionLess = 0xC, kConditionGreaterOrEqual = 0xD;
constexpr uint8_t kConditionLessOrEqual = 0xE, kConditionGreater = 0xF;

// The opcodes that carry a condition. `jcc rel8` is one byte; the others are
// two, behind the 0x0F escape.
constexpr uint8_t kJccRel8First = 0x70, kJccRel8Last = 0x7F;
constexpr uint8_t kTwoByteEscape = 0x0F;
constexpr uint8_t kCmovccFirst = 0x40, kCmovccLast = 0x4F;
constexpr uint8_t kJccRel32First = 0x80, kJccRel32Last = 0x8F;
constexpr uint8_t kSetccFirst = 0x90, kSetccLast = 0x9F;

constexpr uint8_t kConditionMask = 0x0F;

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

bool Within(uint8_t value, uint8_t first, uint8_t last) {
    return value >= first && value <= last;
}

} // namespace

Condition ConditionTested(const Instruction& instruction) {
    hde64s decoded{};
    if (!Decoded(instruction.address, decoded))
        return Condition::NotConditional;
    if (Within(decoded.opcode, kJccRel8First, kJccRel8Last))
        return FromConditionCode(decoded.opcode & kConditionMask);
    if (decoded.opcode != kTwoByteEscape)
        return Condition::NotConditional;
    if (Within(decoded.opcode2, kCmovccFirst, kCmovccLast) ||
        Within(decoded.opcode2, kJccRel32First, kJccRel32Last) ||
        Within(decoded.opcode2, kSetccFirst, kSetccLast))
        return FromConditionCode(decoded.opcode2 & kConditionMask);
    return Condition::NotConditional;
}

const char* ConditionName(Condition condition) {
    switch (condition) {
    case Condition::Ordering: return "ordering";
    case Condition::Equality: return "equality";
    case Condition::Other: return "other";
    default: return "none";
    }
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

namespace {

// REX prefix: 0100WRXB. X and B extend the index and base registers of the
// memory operand; R extends the ModRM `reg` field, the source register here.
constexpr uint8_t kRexBase = 0x40;
constexpr uint8_t kRexX = 0x02;
constexpr uint8_t kRexB = 0x01;
// ModRM: mod(2) reg(3) rm(3). Clearing `reg` selects `mov` in group 11.
constexpr uint8_t kModRmRegMask = 0x38;
constexpr size_t kDisplacement32Size = 4;

// A register byte store to `[base + disp32]` with no SIB byte and no prefix but
// REX, which is the only shape AsImmediateByteStore re-encodes.
bool IsPlainRegisterByteStore(const hde64s& decoded) {
    return decoded.opcode == kOpcodeMovByteRegister && (decoded.flags & F_MODRM) &&
           decoded.modrm_mod == kModDisplacement32 && !(decoded.flags & F_SIB) &&
           !(decoded.flags & (F_PREFIX_ANY & ~F_PREFIX_REX));
}

} // namespace

std::optional<int32_t> AsRegisterByteStore(const Instruction& instruction) {
    hde64s decoded{};
    if (!Decoded(instruction.address, decoded) || !IsPlainRegisterByteStore(decoded))
        return std::nullopt;
    return Displacement(decoded);
}

std::optional<std::vector<std::byte>> AsImmediateByteStore(const Instruction& instruction,
                                                           uint8_t value) {
    hde64s decoded{};
    if (!Decoded(instruction.address, decoded) || !IsPlainRegisterByteStore(decoded))
        return std::nullopt;
    const uint8_t memory_rex = (decoded.rex_x ? kRexX : 0) | (decoded.rex_b ? kRexB : 0);
    std::vector<std::byte> encoded;
    if (memory_rex)
        encoded.push_back(std::byte{static_cast<uint8_t>(kRexBase | memory_rex)});
    encoded.push_back(std::byte{kOpcodeMovByteImmediate});
    encoded.push_back(std::byte{static_cast<uint8_t>(decoded.modrm & ~kModRmRegMask)});
    for (size_t i = 0; i < kDisplacement32Size; ++i)
        encoded.push_back(std::byte{static_cast<uint8_t>(decoded.disp.disp32 >> (8 * i))});
    encoded.push_back(std::byte{value});
    if (encoded.size() != instruction.length)
        return std::nullopt;
    return encoded;
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
