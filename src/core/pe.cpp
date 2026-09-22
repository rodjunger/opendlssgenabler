#include "core/pe.h"

#include "core/x86.h"

#include <libhat/process.hpp>

#include <cstring>

namespace odg::pe {

std::vector<std::span<const std::byte>> ReadableSections(HMODULE module) {
    std::vector<std::span<const std::byte>> sections;
    const auto image = hat::process::module_at(module);
    if (!image)
        return sections;
    image->for_each_section([&](std::string_view, std::span<std::byte> data, hat::protection access) {
        if ((access & hat::protection::Read) == hat::protection::Read)
            sections.emplace_back(data);
        return true;
    });
    return sections;
}

HMODULE ModuleOf(const void* address) {
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        static_cast<LPCWSTR>(address), &module))
        return nullptr;
    return module;
}

bool PatchCode(const void* code, const void* bytes, size_t size) {
    void* address = const_cast<void*>(code);
    DWORD previous = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous))
        return false;
    std::memcpy(address, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

namespace {

bool RangeReadable(const void* address, size_t size) {
    const auto* cursor = static_cast<const uint8_t*>(address);
    const auto* end = cursor + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(cursor, &info, sizeof(info)) != sizeof(info))
            return false;
        if (info.State != MEM_COMMIT)
            return false;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
        if ((info.Protect & readable) == 0 || (info.Protect & PAGE_GUARD))
            return false;
        cursor = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

} // namespace

bool SafeCopy(void* destination, const void* source, size_t size) {
    if (!destination || !source || size == 0)
        return false;
    // Checked rather than caught: the MinGW toolchains have no structured
    // exception handling for C++. The range can in principle change between
    // the check and the copy, which nothing this reads does in practice.
    if (!RangeReadable(source, size))
        return false;
    std::memcpy(destination, source, size);
    return true;
}

void* ResolveJumpThunk(void* address) {
    const auto thunk = x86::Decode(static_cast<const std::byte*>(address));
    const auto target = thunk ? x86::JumpTarget(*thunk) : std::nullopt;
    const HMODULE module = ModuleOf(address);
    if (!target || !module || ModuleOf(*target) != module)
        return address;
    return const_cast<std::byte*>(*target);
}

} // namespace odg::pe
