#include "spoof/nvapi.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/paths.h"
#include "kernels/device.h"
#include "kernels/substitute.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace odg::spoof::nvapi {
namespace {

// Interface ids from the public NVAPI headers. NVAPI resolves its entry points
// through nvapi_QueryInterface rather than by export name, so an id is the only
// way to reach one.
enum : uint32_t {
    kInterfaceGetArchInfo = 0xD8265D24,
    kInterfaceSetRawScgPriority = 0x5DB3048A,
    kInterfaceCreateCubinShaderExV2 = 0x299F5FDC,
    kInterfaceGetDriverVersion = 0x2926AAAD, // NvAPI_SYS_GetDriverAndBranchVersion
    kInterfaceCreateCuModule = 0xAD1A677D,
    kInterfaceCreateCuFunction = 0xE2436E22,
};

// Streamline and NGX both gate frame generation by comparing the reported
// architecture against the first one NVIDIA supports for it, so reporting Ada
// is what opens the feature.
using kernels::kNvApiAda;
using kernels::kNvApiAmpere;
using kernels::kNvApiTuring;

// NV_GPU_ARCH_INFO_VER1 and _VER2: the struct size in the low word and the
// version in the high word. Anything else is a layout this code cannot read.
enum : uint32_t {
    kArchInfoVersion1 = 0x10010,
    kArchInfoVersion2 = 0x20010,
};

// NVAPI_ERROR, returned only when a hook has no original to call.
constexpr uint32_t kNvApiError = 0xFFFFFFFF;

// Enough architecture queries to see who asks, without a line per frame from a
// caller that polls.
constexpr uint32_t kLoggedQueries = 32;

struct ArchInfo {
    uint32_t version;
    uint32_t architecture;
    uint32_t implementation;
    uint32_t revision;
};

using PfnQueryInterface = void*(__cdecl*)(uint32_t interface_id);
using PfnGetArchInfo = uint32_t(__cdecl*)(void* gpu, ArchInfo* info);
// The version is the driver's release number times 100, so 57316 is 573.16.
using PfnGetDriverVersion = uint32_t(__cdecl*)(uint32_t* version, char branch[64]);
using PfnSetRawScgPriority = uint32_t(__cdecl*)(void* params);
using PfnCreateCubinShaderExV2 = uint32_t(__cdecl*)(void* params);
// NvAPI_D3D12_CreateCuModule(ID3D12Device*, const void* blob, NvU32 size,
// NVDX_ObjectHandle* module) and NvAPI_D3D12_CreateCuFunction(ID3D12Device*,
// NVDX_ObjectHandle module, const char* name, NVDX_ObjectHandle* function),
// from the NVAPI headers, where both are marked experimental and internal.
using PfnCreateCuModule = uint32_t(__cdecl*)(void* device, const void* blob, uint32_t size,
                                             void** out_module);
using PfnCreateCuFunction = uint32_t(__cdecl*)(void* device, void* module, const char* name,
                                               void** out_function);

std::atomic<PfnGetArchInfo> g_get_arch_info{nullptr};
std::atomic<PfnSetRawScgPriority> g_set_raw_scg_priority{nullptr};
std::atomic<PfnCreateCubinShaderExV2> g_create_cubin_shader{nullptr};
std::atomic<PfnCreateCuModule> g_create_cu_module{nullptr};
std::atomic<PfnCreateCuFunction> g_create_cu_function{nullptr};
std::atomic<uint32_t> g_cu_function_failures{0};
std::atomic<bool> g_cu_module_reported{false};

thread_local std::vector<uint8_t> t_module;

// Idle, installing, installed. A failed attempt returns to idle so the next
// module scan can try again.
enum : int { kIdle, kInstalling, kInstalled };
std::atomic<int> g_install_state{kIdle};

std::atomic<bool> g_spoof_enabled{true};
std::atomic<bool> g_stub_scg_priority{true};
std::atomic<uint32_t> g_real_architecture{0};
std::atomic<bool> g_spoof_reported{false};
std::atomic<uint32_t> g_arch_queries{0};

std::atomic<PfnQueryInterface> g_query_interface{nullptr};
std::atomic<bool> g_driver_reported{false};

std::vector<std::wstring> g_spoof_callers;
SRWLOCK g_callers_lock = SRWLOCK_INIT;

bool IsSupported(uint32_t architecture) {
    return architecture == kNvApiAmpere;
}

bool CallerIsSpoofed(const void* return_address) {
    AcquireSRWLockShared(&g_callers_lock);
    const bool everyone = g_spoof_callers.empty();
    ReleaseSRWLockShared(&g_callers_lock);
    if (everyone)
        return true;

    HMODULE module = nullptr;
    if (!return_address ||
        !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(return_address), &module))
        return false;
    // The component this module is, not the file name it happens to carry: NGX
    // downloads replacement plugins under a name that identifies nothing, and a
    // downloaded sl.common is the one whose answer every plugin later reads.
    const std::wstring name = paths::ComponentFileName(paths::ModulePath(module));

    AcquireSRWLockShared(&g_callers_lock);
    bool listed = false;
    for (const std::wstring& candidate : g_spoof_callers)
        listed = listed || paths::FileNameEqualsInsensitive(name, candidate.c_str());
    ReleaseSRWLockShared(&g_callers_lock);
    return listed;
}

// Every report needs the driver version, and no other line carries it. Read
// rather than hooked: nothing here changes it.
//
// Asked on the first architecture query rather than while installing the hooks.
// Every NVAPI entry point except NvAPI_Initialize refuses until the process has
// initialized NVAPI, and this engine maps nvapi64.dll itself, long before the
// game gets there. A caller asking for the architecture has initialized it.
void ReportDriver() {
    PfnQueryInterface query = g_query_interface.load(std::memory_order_acquire);
    if (!query || g_driver_reported.exchange(true, std::memory_order_acq_rel))
        return;
    const auto get_version =
        reinterpret_cast<PfnGetDriverVersion>(query(kInterfaceGetDriverVersion));
    uint32_t version = 0;
    // NvAPI writes an NvAPI_ShortString, 64 bytes. The extra byte is never
    // written to, so the string is terminated however full it comes back.
    char branch[65] = {};
    if (!get_version || get_version(&version, branch) != 0 || version == 0) {
        // Promised by the documentation as part of every report, so its absence
        // is worth a line of its own.
        log::Event(log::Level::Warning, "driver_version_unavailable", {});
        return;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%02u", version / 100, version % 100);
    log::Header("driver", {log::Field::Str("version", text), log::Field::Str("branch", branch)});
}

// Decides what one GetArchInfo caller is told, rewriting `info` when it is on
// the spoof list. Returns the architecture the caller ends up with.
uint32_t Answer(uint32_t status, ArchInfo* info, const void* return_address) {
    if (status != 0 || !info ||
        (info->version != kArchInfoVersion1 && info->version != kArchInfoVersion2))
        return info ? info->architecture : 0;

    const uint32_t real = info->architecture;
    ReportDriver();
    if (g_real_architecture.exchange(real, std::memory_order_acq_rel) != real)
        log::Header("gpu_architecture",
                    {log::Field::Hex("arch", real),
                    log::Field::Hex("implementation", info->implementation),
                    log::Field::Bool("supported", IsSupported(real)),
                    log::Field::Str("note", real == kNvApiTuring
                                                ? "Turing is not supported yet; left unchanged"
                                                : "")});
    if (!IsSupported(real))
        return real;

    kernels::Activate(real, info->implementation);
    if (!CallerIsSpoofed(return_address) || !g_spoof_enabled.load(std::memory_order_acquire))
        return real;

    info->architecture = kNvApiAda;
    if (!g_spoof_reported.exchange(true))
        log::Header("arch_spoof_applied",
                    {log::Field::Hex("real_arch", real), log::Field::Hex("reported_arch", kNvApiAda),
                    log::Field::Str("first_caller", paths::ModuleNameForAddress(return_address))});
    return info->architecture;
}

uint32_t __cdecl HookedGetArchInfo(void* gpu, ArchInfo* info) {
    PfnGetArchInfo original = g_get_arch_info.load(std::memory_order_acquire);
    if (!original)
        return kNvApiError;

    const void* const return_address = ODG_RETURN_ADDRESS();
    const uint32_t status = original(gpu, info);
    const uint32_t real = info ? info->architecture : 0;
    const uint32_t reported = Answer(status, info, return_address);
    if (g_arch_queries.fetch_add(1, std::memory_order_relaxed) < kLoggedQueries)
        log::Event(log::Level::Trace, "arch_query",
                   {log::Field::Str("caller", paths::ModuleNameForAddress(return_address)),
                    log::Field::Uint("status", status), log::Field::Hex("real_arch", real),
                    log::Field::Hex("reported_arch", reported)});
    return status;
}

uint32_t __cdecl HookedSetRawScgPriority(void* params) {
    if (g_spoof_enabled.load(std::memory_order_acquire) &&
        g_stub_scg_priority.load(std::memory_order_acquire) &&
        IsSupported(g_real_architecture.load(std::memory_order_acquire)))
        return 0;
    PfnSetRawScgPriority original = g_set_raw_scg_priority.load(std::memory_order_acquire);
    return original ? original(params) : kNvApiError;
}

// The parameter block is versioned and its layout is not public, so it is
// treated as opaque: kernels::Apply finds the container inside it and swaps in
// one this GPU can run.
uint32_t __cdecl HookedCreateCubinShaderExV2(void* params) {
    PfnCreateCubinShaderExV2 original = g_create_cubin_shader.load(std::memory_order_acquire);
    if (!original)
        return kNvApiError;

    switch (kernels::Apply(params, ODG_RETURN_ADDRESS())) {
    case kernels::Decision::Unchanged:
        return original(params);
    case kernels::Decision::Refused:
        // The image cannot run on this GPU and there is nothing to replace it
        // with. The driver would refuse it too; refusing here keeps the
        // runtime on the same failure path without handing the driver an
        // image it cannot execute.
        return kNvApiError;
    case kernels::Decision::Substituted:
        break;
    }
    uint32_t status = original(params);
    if (status != 0 && kernels::ApplyFallback(params, status))
        status = original(params);
    kernels::Revert(params);
    kernels::ReportDriverResult(status, "d3d12");
    return status;
}

// From runtime 310.7 the Direct3D 12 route changed: instead of handing the
// driver one cubin per kernel, the runtime loads the whole fatbin once and
// resolves entry points from it, the same shape as Vulkan's
// VK_NVX_binary_import. A runtime taking this path never reaches
// CreateCubinComputeShaderExV2, so without this hook its kernels are never
// substituted, the driver refuses a module holding no image this GPU can run,
// and NGX fails the feature with a platform error.
uint32_t __cdecl HookedCreateCuModule(void* device, const void* blob, uint32_t size,
                                      void** out_module) {
    PfnCreateCuModule original = g_create_cu_module.load(std::memory_order_acquire);
    if (!original)
        return kNvApiError;
    if (!blob || !size)
        return original(device, blob, size, out_module);

    if (!g_cu_module_reported.exchange(true))
        log::Event(log::Level::Info, "cu_module_intercepted",
                   {log::Field::Str("route", "d3d12")});

    kernels::Request request;
    request.route = "d3d12";
    request.caller = paths::ModuleNameForAddress(ODG_RETURN_ADDRESS());
    switch (kernels::Decide(blob, size, request, t_module)) {
    case kernels::Decision::Unchanged:
        return original(device, blob, size, out_module);
    case kernels::Decision::Refused:
        // Nothing in the module can run here and there is no replacement. The
        // driver would refuse it too, so refusing keeps the runtime on the same
        // failure path rather than handing it code the GPU cannot execute.
        return kNvApiError;
    case kernels::Decision::Substituted:
        break;
    }

    uint32_t status =
        original(device, t_module.data(), static_cast<uint32_t>(t_module.size()), out_module);
    if (status != 0 && kernels::Fallback(blob, size, status, request, t_module))
        status =
            original(device, t_module.data(), static_cast<uint32_t>(t_module.size()), out_module);
    kernels::ReportDriverResult(status, "d3d12");
    return status;
}

// A kernel the runtime cannot resolve in the substituted module is the
// difference between frame generation running and failing, and the runtime does
// not say which one it wanted.
uint32_t __cdecl HookedCreateCuFunction(void* device, void* module, const char* name,
                                        void** out_function) {
    PfnCreateCuFunction original = g_create_cu_function.load(std::memory_order_acquire);
    if (!original)
        return kNvApiError;
    const uint32_t status = original(device, module, name, out_function);
    if (status != 0 &&
        g_cu_function_failures.fetch_add(1, std::memory_order_relaxed) < kLoggedQueries)
        log::Event(log::Level::Error, "cu_function_missing",
                   {log::Field::Str("route", "d3d12"),
                    log::Field::Str("kernel", name ? name : ""),
                    log::Field::Uint("status", status)});
    return status;
}

// Resolves an NVAPI entry point through the real dispatcher and hooks the
// function itself. Substituting the pointer nvapi_QueryInterface hands out only
// reaches callers that ask afterwards, and Streamline reads the architecture
// within the first hundred milliseconds of the process.
template <typename Pfn>
bool HookInterface(PfnQueryInterface query, uint32_t interface_id, void* detour,
                   std::atomic<Pfn>& slot, const char* name) {
    void* target = query(interface_id);
    if (!target) {
        log::Event(log::Level::Info, "nvapi_interface_absent", {log::Field::Str("name", name)});
        return false;
    }
    return hooks::Install(target, detour, slot, name);
}

} // namespace

void SetSpoofEnabled(bool enabled) {
    g_spoof_enabled.store(enabled, std::memory_order_release);
}

void SetStubScgPriority(bool enabled) {
    g_stub_scg_priority.store(enabled, std::memory_order_release);
}

void SetSpoofCallers(const std::vector<std::wstring>& modules) {
    AcquireSRWLockExclusive(&g_callers_lock);
    g_spoof_callers = modules;
    ReleaseSRWLockExclusive(&g_callers_lock);
}

Architecture RealArchitecture() {
    const uint32_t real = g_real_architecture.load(std::memory_order_acquire);
    if (!real)
        return Architecture::Unknown;
    return IsSupported(real) ? Architecture::Supported : Architecture::NotNeeded;
}

bool Install(HMODULE nvapi) {
    if (!nvapi)
        return false;
    int expected = kIdle;
    if (!g_install_state.compare_exchange_strong(expected, kInstalling))
        return expected == kInstalled;

    const auto query = reinterpret_cast<PfnQueryInterface>(
        reinterpret_cast<void*>(GetProcAddress(nvapi, "nvapi_QueryInterface")));
    // Without the architecture hook nothing downstream ever offers frame
    // generation, so a failure here is retried on the next module scan.
    if (!query || !HookInterface(query, kInterfaceGetArchInfo,
                                 reinterpret_cast<void*>(&HookedGetArchInfo), g_get_arch_info,
                                 "NvAPI_GPU_GetArchInfo")) {
        g_install_state.store(kIdle, std::memory_order_release);
        return false;
    }
    HookInterface(query, kInterfaceSetRawScgPriority,
                  reinterpret_cast<void*>(&HookedSetRawScgPriority), g_set_raw_scg_priority,
                  "NvAPI_D3D12_SetRawScgPriority");
    HookInterface(query, kInterfaceCreateCubinShaderExV2,
                  reinterpret_cast<void*>(&HookedCreateCubinShaderExV2), g_create_cubin_shader,
                  "NvAPI_D3D12_CreateCubinComputeShaderExV2");
    // The route a runtime from 310.7 onwards takes instead of the one above.
    // Older drivers do not publish these, which is not a failure.
    HookInterface(query, kInterfaceCreateCuModule,
                  reinterpret_cast<void*>(&HookedCreateCuModule), g_create_cu_module,
                  "NvAPI_D3D12_CreateCuModule");
    HookInterface(query, kInterfaceCreateCuFunction,
                  reinterpret_cast<void*>(&HookedCreateCuFunction), g_create_cu_function,
                  "NvAPI_D3D12_CreateCuFunction");
    g_query_interface.store(query, std::memory_order_release);
    g_install_state.store(kInstalled, std::memory_order_release);
    return true;
}

} // namespace odg::spoof::nvapi
