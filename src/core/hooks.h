#pragma once

#include <windows.h>

#include <atomic>

// The return address of the current function, used to tell which module made a
// call into a hook.
#if defined(_MSC_VER)
#include <intrin.h>
#define ODG_RETURN_ADDRESS() _ReturnAddress()
#else
#define ODG_RETURN_ADDRESS() __builtin_return_address(0)
#endif

// Inline hooks, on top of MinHook.
//
// Every hook is installed in the same three steps: create it, publish the
// trampoline to the slot the detour reads, then enable it. Enabling first leaves
// a window in which another thread reaches a detour that has nothing to call
// through to. On LoadLibraryExW that fails a DLL load the game asked for; on
// NvAPI_GPU_GetArchInfo it hands Streamline an error. Both are rare, random and
// fatal, so there is no API here that installs a hook any other way.
namespace odg::hooks {

bool Init();

namespace detail {
bool Create(void* target, void* detour, void** original, const char* name);
bool Enable(void* target, const char* name);
void* Export(HMODULE module, const char* name);
} // namespace detail

// Hooks `target` with `detour`. The original function is available in `slot`
// before the detour can run, and stays null if installation fails.
template <typename Pfn>
bool Install(void* target, void* detour, std::atomic<Pfn>& slot, const char* name) {
    void* original = nullptr;
    if (!detail::Create(target, detour, &original, name))
        return false;
    slot.store(reinterpret_cast<Pfn>(original), std::memory_order_release);
    if (detail::Enable(target, name))
        return true;
    slot.store(nullptr, std::memory_order_release);
    return false;
}

// Hooks an exported function of a loaded module.
template <typename Pfn>
bool InstallExport(HMODULE module, const char* name, void* detour, std::atomic<Pfn>& slot) {
    void* target = detail::Export(module, name);
    return target && Install(target, detour, slot, name);
}

} // namespace odg::hooks
