#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

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

// The address a `[rip + disp32]` memory operand refers to, such as the string
// a `lea` loads. Empty for an instruction without one.
std::optional<const std::byte*> RipRelativeOperand(const Instruction& instruction);

// The destination of an unconditional jump: `jmp rel32`, or `jmp [rip + rel32]`
// through a pointer. Empty for anything else, or when the pointer cannot be
// read.
std::optional<const std::byte*> JumpTarget(const Instruction& instruction);

} // namespace odg::x86
