#include "kernels/vulkan_hooks.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/pe.h"
#include "kernels/substitute.h"
#include "streamline/streamline.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace odg::kernels {
namespace {

// VkCuModuleCreateInfoNVX, from VK_NVX_binary_import. The layout is part of the
// Vulkan ABI: structure type, extension chain, then the image and its length.
struct CuModuleCreateInfo {
    uint32_t type;
    uint32_t reserved;
    const void* next;
    size_t data_size;
    const void* data;
};

// VkCuFunctionCreateInfoNVX: structure type, extension chain, the module, then
// the entry point's name.
struct CuFunctionCreateInfo {
    uint32_t type;
    uint32_t reserved;
    const void* next;
    uint64_t module;
    const char* name;
};

// VK_ERROR_INITIALIZATION_FAILED, the answer for an image this code refuses.
constexpr int32_t kVkErrorInitializationFailed = -3;
// Returned only when a hook has no original to call.
constexpr int32_t kVkErrorUnknown = -13;
constexpr uint32_t kLoggedFunctionFailures = 16;

using PfnVoidFunction = void(__stdcall*)();
using PfnGetProcAddr = PfnVoidFunction(__stdcall*)(void* instance_or_device, const char* name);
using PfnCreateCuModule = int32_t(__stdcall*)(void* device, const CuModuleCreateInfo* info,
                                              const void* allocator, uint64_t* out_module);
using PfnCreateCuFunction = int32_t(__stdcall*)(void* device, const CuFunctionCreateInfo* info,
                                                const void* allocator, uint64_t* out_function);

std::atomic<PfnGetProcAddr> g_get_device_proc_addr{nullptr};
std::atomic<PfnGetProcAddr> g_get_instance_proc_addr{nullptr};
std::atomic<PfnCreateCuModule> g_create_cu_module{nullptr};
std::atomic<PfnCreateCuFunction> g_create_cu_function{nullptr};
std::atomic<uint32_t> g_function_failures{0};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_intercept_reported{false};

constexpr wchar_t kLoaderName[] = L"vulkan-1.dll";

int32_t __stdcall HookedCreateCuModule(void* device, const CuModuleCreateInfo* info,
                                       const void* allocator, uint64_t* out_module) {
    PfnCreateCuModule original = g_create_cu_module.load(std::memory_order_acquire);
    if (!original)
        return kVkErrorUnknown;
    if (!info)
        return original(device, info, allocator, out_module);

    const Request request = Request::From(kRouteVulkan, ODG_RETURN_ADDRESS());
    Substitution substitution;
    switch (Decide(info->data, info->data_size, request, substitution)) {
    case Decision::Unchanged:
        return original(device, info, allocator, out_module);
    case Decision::Refused:
        // Unlike the D3D12 path, the Vulkan driver accepts an image built for
        // a newer architecture and only fails when it runs, by hanging the GPU.
        return kVkErrorInitializationFailed;
    case Decision::Substituted:
        break;
    }
    return CreateSubstituted(info->data, info->data_size, request, substitution,
                             [&](const std::vector<uint8_t>& image) {
                                 CuModuleCreateInfo substituted = *info;
                                 substituted.data = image.data();
                                 substituted.data_size = image.size();
                                 return original(device, &substituted, allocator, out_module);
                             });
}

// A function the runtime cannot find is the difference between a pipeline that
// stalls and one that runs, and the runtime does not name it.
int32_t __stdcall HookedCreateCuFunction(void* device, const CuFunctionCreateInfo* info,
                                         const void* allocator, uint64_t* out_function) {
    PfnCreateCuFunction original = g_create_cu_function.load(std::memory_order_acquire);
    if (!original)
        return kVkErrorUnknown;
    const int32_t status = original(device, info, allocator, out_function);
    if (status != 0 &&
        g_function_failures.fetch_add(1, std::memory_order_relaxed) < kLoggedFunctionFailures)
        log::Event(log::Level::Warning, "vulkan_function_failed",
                   {log::Field::Str("kernel", info && info->name ? info->name : ""),
                    log::Field::Int("status", status)});
    return status;
}

// Reflex on Vulkan (VK_NV_low_latency2). With low-latency mode on, the driver
// expects a vkLatencySleepNV call every frame. A game that never makes one is
// paced by the driver inside vkQueuePresentKHR instead, one present at a time.
// With frame generation on, the presenter makes a present for every displayed
// frame, generated ones included, so that pacing holds the output to half the
// refresh rate while the GPU idles. No Man's Sky turns Reflex on and never
// sleeps. While frame generation is on and no vkLatencySleepNV has been seen,
// low-latency mode is passed to the driver as off. Otherwise the game's request
// is passed through; Streamline sends it again on every new swapchain, which
// is how the game's setting comes back after frame generation is switched off.

// VkLatencySleepModeInfoNV, from VK_NV_low_latency2 in the Vulkan specification.
struct LatencySleepModeInfo {
    uint32_t type;
    const void* next;
    uint32_t low_latency_mode;
    uint32_t low_latency_boost;
    uint32_t minimum_interval_us;
};
using PfnSetLatencySleepMode = int32_t(__stdcall*)(void* device, uint64_t swapchain,
                                                   const LatencySleepModeInfo* info);
using PfnLatencySleep = int32_t(__stdcall*)(void* device, uint64_t swapchain, const void* info);

std::atomic<PfnSetLatencySleepMode> g_set_latency_sleep_mode{nullptr};
std::atomic<PfnLatencySleep> g_latency_sleep{nullptr};
std::atomic<bool> g_latency_sleep_seen{false};
std::atomic<bool> g_present_pacing_fix{true};
std::atomic<bool> g_low_latency_off{false};

int32_t __stdcall HookedLatencySleep(void* device, uint64_t swapchain, const void* info) {
    const PfnLatencySleep original = g_latency_sleep.load(std::memory_order_acquire);
    if (!original)
        return kVkErrorUnknown;
    g_latency_sleep_seen.store(true, std::memory_order_release);
    return original(device, swapchain, info);
}

int32_t __stdcall HookedSetLatencySleepMode(void* device, uint64_t swapchain,
                                            const LatencySleepModeInfo* info) {
    const PfnSetLatencySleepMode original = g_set_latency_sleep_mode.load(std::memory_order_acquire);
    if (!original)
        return kVkErrorUnknown;
    if (!info)
        return original(device, swapchain, info);

    const bool frame_generation = streamline::FrameGenerationOn();
    const bool game_sleeps = g_latency_sleep_seen.load(std::memory_order_acquire);
    const bool off = info->low_latency_mode && Active() &&
                     g_present_pacing_fix.load(std::memory_order_acquire) && !game_sleeps &&
                     frame_generation;
    if (g_low_latency_off.exchange(off, std::memory_order_relaxed) != off)
        log::Event(log::Level::Info, "reflex_present_pacing",
                   {log::Field::Bool("low_latency_off", off),
                    log::Field::Bool("frame_generation", frame_generation),
                    log::Field::Bool("game_sleeps", game_sleeps)});
    if (!off)
        return original(device, swapchain, info);
    LatencySleepModeInfo sent = *info;
    sent.low_latency_mode = 0;
    sent.low_latency_boost = 0;
    return original(device, swapchain, &sent);
}

PfnVoidFunction Intercept(const char* name, PfnVoidFunction resolved) {
    if (!resolved || !name)
        return resolved;
    if (std::strcmp(name, "vkSetLatencySleepModeNV") == 0) {
        g_set_latency_sleep_mode.store(reinterpret_cast<PfnSetLatencySleepMode>(resolved),
                                       std::memory_order_release);
        return reinterpret_cast<PfnVoidFunction>(&HookedSetLatencySleepMode);
    }
    if (std::strcmp(name, "vkLatencySleepNV") == 0) {
        g_latency_sleep.store(reinterpret_cast<PfnLatencySleep>(resolved),
                              std::memory_order_release);
        return reinterpret_cast<PfnVoidFunction>(&HookedLatencySleep);
    }
    if (std::strcmp(name, "vkCreateCuModuleNVX") == 0) {
        g_create_cu_module.store(reinterpret_cast<PfnCreateCuModule>(resolved),
                                 std::memory_order_release);
        if (!g_intercept_reported.exchange(true))
            log::Event(log::Level::Info, "vulkan_cu_module_intercepted", {});
        return reinterpret_cast<PfnVoidFunction>(&HookedCreateCuModule);
    }
    if (std::strcmp(name, "vkCreateCuFunctionNVX") == 0) {
        g_create_cu_function.store(reinterpret_cast<PfnCreateCuFunction>(resolved),
                                   std::memory_order_release);
        return reinterpret_cast<PfnVoidFunction>(&HookedCreateCuFunction);
    }
    return resolved;
}

PfnVoidFunction __stdcall HookedGetDeviceProcAddr(void* device, const char* name) {
    PfnGetProcAddr original = g_get_device_proc_addr.load(std::memory_order_acquire);
    return original ? Intercept(name, original(device, name)) : nullptr;
}

PfnVoidFunction __stdcall HookedGetInstanceProcAddr(void* instance, const char* name) {
    PfnGetProcAddr original = g_get_instance_proc_addr.load(std::memory_order_acquire);
    return original ? Intercept(name, original(instance, name)) : nullptr;
}

// Hooks one of the loader's exported resolvers. The export may be a jump stub,
// which is followed to the function it leads to.
bool HookResolver(HMODULE loader, const char* name, void* detour,
                  std::atomic<PfnGetProcAddr>& slot) {
    void* exported = hooks::detail::Export(loader, name);
    return exported && hooks::Install(pe::ResolveJumpThunk(exported), detour, slot, name);
}

} // namespace

void SetPresentPacingFix(bool enabled) {
    g_present_pacing_fix.store(enabled, std::memory_order_release);
}

bool InstallVulkanHooks() {
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

    // The extension functions are exported by nobody; the loader's resolvers
    // hand them out, so that is where they are taken over. Both resolvers can
    // answer for a device function: vkGetInstanceProcAddr returns a trampoline
    // that dispatches through the device. A resolution through the one left
    // unhooked would hand the driver an image this GPU cannot run, so both are
    // hooked, and each only compares the name it was asked for.
    const bool device = HookResolver(module, "vkGetDeviceProcAddr",
                                     reinterpret_cast<void*>(&HookedGetDeviceProcAddr),
                                     g_get_device_proc_addr);
    const bool instance = HookResolver(module, "vkGetInstanceProcAddr",
                                       reinterpret_cast<void*>(&HookedGetInstanceProcAddr),
                                       g_get_instance_proc_addr);
    if (!device || !instance) {
        // Attempted once: a hook that fails to install on a loaded, pinned
        // loader will not start working.
        // An unhooked resolver can hand out the extension function unwrapped,
        // so this is a failure even when the other one works.
        log::Event(log::Level::Error, "vulkan_hooks_unavailable",
                   {log::Field::Bool("device_resolver", device),
                    log::Field::Bool("instance_resolver", instance),
                    log::Field::Str("note", "the Vulkan loader could not be fully hooked. "
                                            "Direct3D 12 games are unaffected; a Vulkan game's "
                                            "frame generation may have no kernels it can run.")});
        if (!device && !instance) {
            FreeLibrary(module);
            return false;
        }
    }
    log::Event(log::Level::Info, "vulkan_hooks_installed",
               {log::Field::Str("module", paths::ModuleFileName(module).c_str())});
    return true;
}

} // namespace odg::kernels
