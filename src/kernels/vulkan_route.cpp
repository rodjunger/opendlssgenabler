#include "kernels/vulkan_route.h"

#include "core/hooks.h"
#include "core/log.h"
#include "kernels/substitute.h"

#include <atomic>
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
constexpr uint32_t kLoggedFunctionFailures = 16;

using PfnCreateCuModule = int32_t(__stdcall*)(void* device, const CuModuleCreateInfo* info,
                                              const void* allocator, uint64_t* out_module);
using PfnCreateCuFunction = int32_t(__stdcall*)(void* device, const CuFunctionCreateInfo* info,
                                                const void* allocator, uint64_t* out_function);

std::atomic<uint32_t> g_function_failures{0};
std::atomic<bool> g_intercept_reported{false};

int32_t __stdcall HookedCreateCuModule(void* device, const CuModuleCreateInfo* info,
                                       const void* allocator, uint64_t* out_module) {
    const auto original =
        vulkan::Original<PfnCreateCuModule>(vulkan::Function::CreateCuModule, device);
    if (!original)
        return vulkan::kVkErrorUnknown;
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
    const auto original =
        vulkan::Original<PfnCreateCuFunction>(vulkan::Function::CreateCuFunction, device);
    if (!original)
        return vulkan::kVkErrorUnknown;
    const int32_t status = original(device, info, allocator, out_function);
    if (status != 0 &&
        g_function_failures.fetch_add(1, std::memory_order_relaxed) < kLoggedFunctionFailures)
        log::Event(log::Level::Warning, "vulkan_function_failed",
                   {log::Field::Str("kernel", info && info->name ? info->name : ""),
                    log::Field::Int("status", status)});
    return status;
}

} // namespace

vulkan::PfnVoidFunction VulkanWrapper(vulkan::Function function) {
    switch (function) {
    case vulkan::Function::CreateCuModule:
        if (!g_intercept_reported.exchange(true))
            log::Event(log::Level::Info, "vulkan_cu_module_intercepted", {});
        return reinterpret_cast<vulkan::PfnVoidFunction>(&HookedCreateCuModule);
    case vulkan::Function::CreateCuFunction:
        return reinterpret_cast<vulkan::PfnVoidFunction>(&HookedCreateCuFunction);
    default:
        return nullptr;
    }
}

} // namespace odg::kernels
