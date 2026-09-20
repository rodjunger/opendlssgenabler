#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Helpers for inspecting and patching a module already mapped in this process.
// All addresses are live virtual addresses inside the image.
namespace odg::pe {

// The sections of a loaded module that are mapped readable, code included.
// Empty when `module` is not a loaded image.
std::vector<std::span<const std::byte>> ReadableSections(HMODULE module);

// The loaded module containing `address`, or null for memory outside any image,
// such as code another tool allocated.
HMODULE ModuleOf(const void* address);

// Overwrites code after switching the page to writable and flushing the
// instruction cache. Restores the previous protection on success. Code is mapped
// read-only, so it is addressed as const like everywhere else it is read.
bool PatchCode(const void* address, const void* bytes, size_t size);

// memcpy that swallows an access violation rather than crashing the host.
bool SafeCopy(void* destination, const void* source, size_t size);

// Follows a jump thunk (`jmp rel32` or `jmp [rip + rel32]`) to the function it
// calls, when that function is in the same module. Some modules export such
// thunks packed a few bytes apart, and an inline hook there would run into the
// next export, so the real function is hooked instead. A jump that leaves the
// module is another tool's hook, such as an overlay's; following it would hook
// that tool's code, so `address` is returned and the hook chains after it.
void* ResolveJumpThunk(void* address);

} // namespace odg::pe
