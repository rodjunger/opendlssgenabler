#include "provider/version_policy.h"

#include <winver.h>

#include <array>
#include <string>
#include <vector>

namespace odg::provider {
namespace {

// Runtimes verified in a game: 310.2.1 in Halo Campaign Evolved, 310.3 in
// PRAGMATA and Jurassic World Evolution 3, 310.5.2 in Corsair Cove, all
// Direct3D 12, and 310.6 in DOOM The Dark Ages (Vulkan). Matched on
// major.minor.build.
struct Tested {
    uint16_t major, minor, build;
};
constexpr std::array<Tested, 4> kTested{
    {{310, 2, 1}, {310, 3, 0}, {310, 5, 2}, {310, 6, 0}}};

// The version-info APIs are resolved from the System32 copy of version.dll at
// runtime rather than imported. A static import would be a self-reference when
// this engine is itself loaded under the name version.dll.
using PfnGetSize = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
using PfnGetInfo = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
using PfnQuery = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

struct VersionApi {
    PfnGetSize get_size = nullptr;
    PfnGetInfo get_info = nullptr;
    PfnQuery query = nullptr;
    bool ok = false;
};

const VersionApi& Api() {
    static const VersionApi api = [] {
        VersionApi a;
        wchar_t system_dir[MAX_PATH];
        const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return a;
        std::wstring path(system_dir, length);
        path += L"\\version.dll";
        HMODULE module = LoadLibraryW(path.c_str());
        if (!module)
            return a;
        a.get_size = reinterpret_cast<PfnGetSize>(GetProcAddress(module, "GetFileVersionInfoSizeW"));
        a.get_info = reinterpret_cast<PfnGetInfo>(GetProcAddress(module, "GetFileVersionInfoW"));
        a.query = reinterpret_cast<PfnQuery>(GetProcAddress(module, "VerQueryValueW"));
        a.ok = a.get_size && a.get_info && a.query;
        return a;
    }();
    return api;
}

} // namespace

bool ReadVersion(const wchar_t* path, Version& out) {
    out = {};
    if (!path || !*path)
        return false;

    const VersionApi& api = Api();
    if (!api.ok)
        return false;

    DWORD ignored = 0;
    const DWORD size = api.get_size(path, &ignored);
    if (size == 0)
        return false;

    std::vector<uint8_t> buffer(size);
    if (!api.get_info(path, 0, size, buffer.data()))
        return false;

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixed_size = 0;
    if (!api.query(buffer.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixed_size) ||
        !fixed || fixed_size < sizeof(VS_FIXEDFILEINFO) || fixed->dwSignature != VS_FFI_SIGNATURE)
        return false;

    out.major = HIWORD(fixed->dwFileVersionMS);
    out.minor = LOWORD(fixed->dwFileVersionMS);
    out.build = HIWORD(fixed->dwFileVersionLS);
    out.revision = LOWORD(fixed->dwFileVersionLS);
    out.valid = true;
    return true;
}

std::string ToString(const Version& version) {
    if (!version.valid)
        return "unknown";
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
           std::to_string(version.build) + "." + std::to_string(version.revision);
}

bool IsTested(const Version& version) {
    if (!version.valid)
        return false;
    for (const Tested& tested : kTested) {
        if (version.major == tested.major && version.minor == tested.minor &&
            version.build == tested.build)
            return true;
    }
    return false;
}

bool IsDlssgProvider(HMODULE module) {
    if (!module)
        return false;
    // Every NGX feature runtime creates features; a Super Resolution runtime
    // additionally exposes the DirectSR entry, which a frame-generation runtime
    // does not.
    const bool creates = GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature") ||
                         GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature");
    const bool direct_sr = GetProcAddress(module, "NVSDK_NGX_DirectSR_Create");
    return creates && !direct_sr;
}

} // namespace odg::provider
