#include "kernels/vulkan_hooks.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/pe.h"
#include "kernels/substitute.h"

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

thread_local std::vector<uint8_t> t_buffer;

constexpr wchar_t kLoaderName[] = L"vulkan-1.dll";

int32_t __stdcall HookedCreateCuModule(void* device, const CuModuleCreateInfo* info,
                                       const void* allocator, uint64_t* out_module) {
    PfnCreateCuModule original = g_create_cu_module.load(std::memory_order_acquire);
    if (!original)
        return kVkErrorUnknown;
    if (!info)
        return original(device, info, allocator, out_module);

    Request request;
    request.route = kRouteVulkan;
    const void* caller = ODG_RETURN_ADDRESS();
    request.module = paths::ModuleForAddress(caller);
    request.caller = paths::ModuleNameForAddress(caller);
    switch (Decide(info->data, info->data_size, request, t_buffer)) {
    case Decision::Unchanged:
        return original(device, info, allocator, out_module);
    case Decision::Refused:
        // Unlike the D3D12 path, the Vulkan driver accepts an image built for
        // a newer architecture and only fails when it runs, by hanging the GPU.
        return kVkErrorInitializationFailed;
    case Decision::Substituted:
        break;
    }
    CuModuleCreateInfo substituted = *info;
    substituted.data = t_buffer.data();
    substituted.data_size = t_buffer.size();
    int32_t status = original(device, &substituted, allocator, out_module);
    if (status != 0 &&
        Fallback(info->data, info->data_size, static_cast<uint32_t>(status), request, t_buffer)) {
        substituted.data = t_buffer.data();
        substituted.data_size = t_buffer.size();
        status = original(device, &substituted, allocator, out_module);
    }
    ReportDriverResult(static_cast<uint32_t>(status), kRouteVulkan);
    return status;
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

PfnVoidFunction Intercept(const char* name, PfnVoidFunction resolved) {
    if (!resolved || !name)
        return resolved;
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
        log::Event(log::Level::Warning, "vulkan_hooks_unavailable",
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
