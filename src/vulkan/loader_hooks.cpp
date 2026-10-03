#include "vulkan/loader_hooks.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/pe.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>
#include <windows.h>

namespace odg::vulkan {
namespace {

constexpr wchar_t kLoaderName[] = L"vulkan-1.dll";

struct NamedFunction {
    const char* name;
    Function function;
};
constexpr NamedFunction kFunctions[] = {
    {"vkCreateCuModuleNVX", Function::CreateCuModule},
    {"vkCreateCuFunctionNVX", Function::CreateCuFunction},
    {"vkSetLatencySleepModeNV", Function::SetLatencySleepMode},
    {"vkLatencySleepNV", Function::LatencySleep},
};

// What a resolver returned for a function. `device` is null for a resolution
// through vkGetInstanceProcAddr, which returns the loader's trampoline: it
// dispatches through whatever device it is called with, so it serves them all.
struct Resolution {
    Function function;
    void* device;
    PfnVoidFunction original;
};
SRWLOCK g_resolutions_lock = SRWLOCK_INIT;
std::vector<Resolution> g_resolutions;

using PfnGetProcAddr = PfnVoidFunction(__stdcall*)(void* instance_or_device, const char* name);
std::atomic<PfnGetProcAddr> g_get_device_proc_addr{nullptr};
std::atomic<PfnGetProcAddr> g_get_instance_proc_addr{nullptr};
std::atomic<WrapperFor> g_wrapper_for{nullptr};
std::atomic<bool> g_installed{false};

void Record(Function function, void* device, PfnVoidFunction original) {
    AcquireSRWLockExclusive(&g_resolutions_lock);
    auto known =
        std::find_if(g_resolutions.begin(), g_resolutions.end(), [&](const Resolution& entry) {
            return entry.function == function && entry.device == device;
        });
    if (known != g_resolutions.end())
        known->original = original;
    else
        g_resolutions.push_back({function, device, original});
    ReleaseSRWLockExclusive(&g_resolutions_lock);
}

// `device` is null for a resolution through vkGetInstanceProcAddr.
PfnVoidFunction Intercept(const char* name, PfnVoidFunction resolved, void* device) {
    const WrapperFor wrapper_for = g_wrapper_for.load(std::memory_order_acquire);
    if (!resolved || !name || !wrapper_for)
        return resolved;
    for (const NamedFunction& named : kFunctions) {
        if (std::strcmp(name, named.name) != 0)
            continue;
        const PfnVoidFunction wrapper = wrapper_for(named.function);
        if (!wrapper)
            return resolved;
        Record(named.function, device, resolved);
        return wrapper;
    }
    return resolved;
}

PfnVoidFunction __stdcall HookedGetDeviceProcAddr(void* device, const char* name) {
    PfnGetProcAddr original = g_get_device_proc_addr.load(std::memory_order_acquire);
    return original ? Intercept(name, original(device, name), device) : nullptr;
}

PfnVoidFunction __stdcall HookedGetInstanceProcAddr(void* instance, const char* name) {
    PfnGetProcAddr original = g_get_instance_proc_addr.load(std::memory_order_acquire);
    return original ? Intercept(name, original(instance, name), nullptr) : nullptr;
}

// Hooks one of the loader's exported resolvers. The export may be a jump stub,
// which is followed to the function it leads to.
bool HookResolver(HMODULE loader, const char* name, void* detour,
                  std::atomic<PfnGetProcAddr>& slot) {
    void* exported = hooks::detail::Export(loader, name);
    return exported && hooks::Install(pe::ResolveJumpThunk(exported), detour, slot, name);
}

} // namespace

PfnVoidFunction Original(Function function, void* device) {
    PfnVoidFunction for_device = nullptr;
    PfnVoidFunction for_any_device = nullptr;
    AcquireSRWLockShared(&g_resolutions_lock);
    for (const Resolution& resolution : g_resolutions) {
        if (resolution.function != function)
            continue;
        if (resolution.device == device)
            for_device = resolution.original;
        else if (!resolution.device)
            for_any_device = resolution.original;
    }
    ReleaseSRWLockShared(&g_resolutions_lock);
    return for_device ? for_device : for_any_device;
}

bool InstallLoaderHooks(WrapperFor wrapper_for) {
    if (g_installed.load(std::memory_order_acquire))
        return true;
    // Direct3D 12 games often load the Vulkan loader only to probe for Vulkan
    // and unload it again. The reference taken here keeps it mapped for as long
    // as the hooks exist, which is the rest of the process. A loader that is
    // already gone is not hooked, and the next module scan tries again.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(0, kLoaderName, &module))
        return false;
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        FreeLibrary(module);
        return true;
    }
    g_wrapper_for.store(wrapper_for, std::memory_order_release);

    // Both resolvers can answer for a device function: vkGetInstanceProcAddr
    // returns a trampoline that dispatches through the device. A resolution
    // through the one left unhooked would hand the driver an image this GPU
    // cannot run, so both are hooked.
    const bool device =
        HookResolver(module, "vkGetDeviceProcAddr",
                     reinterpret_cast<void*>(&HookedGetDeviceProcAddr), g_get_device_proc_addr);
    const bool instance =
        HookResolver(module, "vkGetInstanceProcAddr",
                     reinterpret_cast<void*>(&HookedGetInstanceProcAddr), g_get_instance_proc_addr);
    if (!device || !instance) {
        // Attempted once: a hook that fails to install on a loaded, pinned
        // loader will not start working. An unhooked resolver can hand out the
        // extension functions unwrapped, so this is a failure even when the
        // other one works.
        log::Event(log::Level::Error, "vulkan_hooks_unavailable",
                   {log::Field::Bool("device_resolver", device),
                    log::Field::Bool("instance_resolver", instance),
                    log::Field::Str("note", "the Vulkan loader could not be fully hooked. "
                                            "Direct3D 12 games are unaffected; a Vulkan game's "
                                            "frame generation may have no kernels it can run.")});
        // A hook that failed to install is removed by hooks::Install, so with
        // neither installed nothing points into the loader any more.
        if (!device && !instance) {
            FreeLibrary(module);
            return false;
        }
    }
    log::Event(log::Level::Info, "vulkan_hooks_installed",
               {log::Field::Str("module", paths::ModuleFileName(module).c_str())});
    return true;
}

} // namespace odg::vulkan
