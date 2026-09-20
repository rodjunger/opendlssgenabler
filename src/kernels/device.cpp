#include "kernels/device.h"

#include "core/log.h"

#include <windows.h>

namespace odg::kernels {
namespace {

// CUDA driver API. Resolved dynamically so the proxy never imports nvcuda.dll:
// the runtime loads it long after we do, and a static import would pull it in
// under the loader lock.
using PfnInit = int(__stdcall*)(unsigned int);
using PfnDeviceGet = int(__stdcall*)(int*, int);
using PfnDeviceGetAttribute = int(__stdcall*)(int*, int, int);

enum : int {
    kAttributeComputeCapabilityMajor = 75,
    kAttributeComputeCapabilityMinor = 76,
};

} // namespace

uint32_t DetectDeviceSm() {
    HMODULE cuda = GetModuleHandleW(L"nvcuda.dll");
    if (!cuda)
        cuda = LoadLibraryW(L"nvcuda.dll");
    if (!cuda)
        return 0;

    auto init = reinterpret_cast<PfnInit>(
        reinterpret_cast<void*>(GetProcAddress(cuda, "cuInit")));
    auto device_get = reinterpret_cast<PfnDeviceGet>(
        reinterpret_cast<void*>(GetProcAddress(cuda, "cuDeviceGet")));
    auto get_attribute = reinterpret_cast<PfnDeviceGetAttribute>(
        reinterpret_cast<void*>(GetProcAddress(cuda, "cuDeviceGetAttribute")));
    if (!init || !device_get || !get_attribute || init(0) != 0)
        return 0;

    int device = 0;
    if (device_get(&device, 0) != 0)
        return 0;
    int major = 0, minor = 0;
    if (get_attribute(&major, kAttributeComputeCapabilityMajor, device) != 0 ||
        get_attribute(&minor, kAttributeComputeCapabilityMinor, device) != 0)
        return 0;
    if (major <= 0)
        return 0;
    return static_cast<uint32_t>(major) * 10 + static_cast<uint32_t>(minor);
}

uint32_t SmFromNvApiArch(uint32_t architecture, uint32_t implementation) {
    switch (architecture) {
    case kNvApiTuring: // TU10x is sm_75; TU11x has no tensor cores but keeps it.
        return 75;
    case kNvApiAmpere: // Implementation 0 is GA100 (sm_80); the GA10x parts are sm_86.
        return implementation == 0 ? 80 : 86;
    case kNvApiAda:
        return 89;
    default:
        return 0;
    }
}

} // namespace odg::kernels
