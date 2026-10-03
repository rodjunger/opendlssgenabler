#pragma once

#include <cstdint>

// The Vulkan loader's resolvers, vkGetDeviceProcAddr and vkGetInstanceProcAddr.
// The extension functions this project wraps are exported by nobody; the
// resolvers hand them out, so that is where they are taken over.
namespace odg::vulkan {

using PfnVoidFunction = void(__stdcall*)();

// The functions this project may wrap.
enum class Function : uint8_t {
    CreateCuModule,      // vkCreateCuModuleNVX, VK_NVX_binary_import
    CreateCuFunction,    // vkCreateCuFunctionNVX, VK_NVX_binary_import
    SetLatencySleepMode, // vkSetLatencySleepModeNV, VK_NV_low_latency2
    LatencySleep,        // vkLatencySleepNV, VK_NV_low_latency2
};

// The wrapper to hand out in place of `function`, or null to hand out the
// driver's own.
using WrapperFor = PfnVoidFunction (*)(Function function);

// Hooks the resolvers once the Vulkan loader is loaded, and keeps it loaded
// from then on. Returns false while it is not loaded, so a module scan can
// simply call again.
bool InstallLoaderHooks(WrapperFor wrapper_for);

// The function a wrapper calls through for `device`. vkGetDeviceProcAddr can
// return a different function for each device, so each is kept per device.
// Null when nothing was resolved for that device.
PfnVoidFunction Original(Function function, void* device);

template <typename Pfn> Pfn Original(Function function, void* device) {
    return reinterpret_cast<Pfn>(Original(function, device));
}

// VK_ERROR_UNKNOWN, what a wrapper returns when it has nothing to call through to.
constexpr int32_t kVkErrorUnknown = -13;

} // namespace odg::vulkan
