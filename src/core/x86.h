#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Decoding of single x86-64 instructions, on top of HDE64, the decoder MinHook
// bundles. Callers ask what an instruction does, not how it is encoded.
namespace odg::x86 {

struct Instruction {
    const std::byte* address = nullptr;
    size_t length = 0;
    const std::byte* Next() const { return address + length; }
};

// Decodes the instruction at `address`. Empty when the bytes are not a valid
// instruction.
std::optional<Instruction> Decode(const std::byte* address);

// `mov byte ptr [register + displacement], immediate`: a store of a constant to
// a byte field of an object held in a register. Empty for anything else,
// including stores through an index register or to the stack.
struct ByteStore {
    int32_t displacement = 0;
    uint8_t value = 0;
};
std::optional<ByteStore> AsByteStore(const Instruction& instruction);

// `mov byte ptr [register + disp32], r8`: a store of a byte register to such a
// field. Returns the displacement; empty for anything else.
std::optional<int32_t> AsRegisterByteStore(const Instruction& instruction);

// The register store `instruction` rewritten to store the constant `value` to
// the same field, as `mov byte ptr [register + disp32], imm8`. Empty unless the
// new encoding is exactly as long as the old one, so it can replace it in
// place. It fits when the old store needed a REX prefix only for its source
// register (`sil`, `dil`, `bpl`, `spl`, `r8b` and up), since the immediate form
// has no source register and drops that byte.
std::optional<std::vector<std::byte>> AsImmediateByteStore(const Instruction& instruction,
                                                           uint8_t value);

// The address a `[rip + disp32]` memory operand refers to, such as the string
// a `lea` loads. Empty for an instruction without one.
std::optional<const std::byte*> RipRelativeOperand(const Instruction& instruction);

// What a conditional instruction asks of the flags: Jcc, SETcc and CMOVcc are
// all classified the same way.
//
// This is how an architecture gate is told apart from an unrelated comparison.
// A gate gives a feature to every architecture at or above some id, so its
// result is read with an ordering test. Rewriting the id it compares against
// keeps that meaning. An equality test asks whether the architecture is one
// exact id, and rewriting the value it is compared with would change the
// question rather than answer it differently, so such a comparison is left
// alone.
enum class Condition {
    NotConditional,
    Ordering, // below, above, less, greater, and their negations
    Equality, // equal, not equal
    Other,    // sign, overflow, parity
};
Condition ConditionTested(const Instruction& instruction);

// The condition's name, for a log line or a report.
const char* ConditionName(Condition condition);

// The destination of an unconditional jump: `jmp rel32`, or `jmp [rip + rel32]`
// through a pointer. Empty for anything else, or when the pointer cannot be
// read.
std::optional<const std::byte*> JumpTarget(const Instruction& instruction);

} // namespace odg::x86
