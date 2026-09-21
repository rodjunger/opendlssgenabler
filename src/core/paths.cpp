#include "core/paths.h"

#include "core/text.h"

#include <psapi.h>

#include <cstring>
#include <iterator>
#include <string_view>

namespace odg::paths {

std::wstring ModulePath(HMODULE module) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, path.data(),
                                                static_cast<DWORD>(path.size()));
        if (length == 0)
            return {};
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring ModuleFileName(HMODULE module) {
    const std::wstring path = ModulePath(module);
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring ParentDirectory(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

bool FileNameEqualsInsensitive(const std::wstring& path, const wchar_t* name) {
    const size_t separator = path.find_last_of(L"\\/");
    const wchar_t* leaf = separator == std::wstring::npos ? path.c_str()
                                                          : path.c_str() + separator + 1;
    return name && _wcsicmp(leaf, name) == 0;
}

namespace {

// The component directories NGX downloads into, and the module each one
// replaces. Only the components this engine acts on are listed; an `override`
// directory holds the copy NVIDIA's own application selected.
// Case-insensitive search, ASCII only, which is all these path segments are.
// Written out rather than lowercasing a copy: this runs for every loaded module
// on every scan, and almost none of them are in the store.
size_t FindInsensitive(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty() || haystack.size() < needle.size())
        return std::wstring_view::npos;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (_wcsnicmp(haystack.data() + i, needle.data(), needle.size()) == 0)
            return i;
    }
    return std::wstring_view::npos;
}

struct OtaComponent {
    const wchar_t* directory;
    const wchar_t* module;
};
constexpr OtaComponent kOtaComponents[] = {
    {L"sl_common_0", L"sl.common.dll"},        {L"sl_common_override_0", L"sl.common.dll"},
    {L"sl_dlss_g_0", L"sl.dlss_g.dll"},        {L"sl_dlss_g_override_0", L"sl.dlss_g.dll"},
    {L"dlssg", L"nvngx_dlssg.dll"},
};

constexpr wchar_t kOtaMarker[] = L"\\NGX\\models\\";
constexpr size_t kOtaMarkerLength = std::size(kOtaMarker) - 1;

// The path segment naming the component, when the path is in the NGX model
// store. Every loaded module is asked this once a second, so it compares in
// place: only the few paths that are in the store are looked at any further.
std::wstring_view OtaComponentDirectory(std::wstring_view path) {
    const size_t marker = FindInsensitive(path, kOtaMarker);
    if (marker == std::wstring_view::npos)
        return {};
    const size_t start = marker + kOtaMarkerLength;
    const size_t end = path.find(L'\\', start);
    return end == std::wstring_view::npos ? std::wstring_view{}
                                          : path.substr(start, end - start);
}

} // namespace

std::wstring ComponentFileName(const std::wstring& path) {
    const std::wstring_view directory = OtaComponentDirectory(path);
    for (const OtaComponent& component : kOtaComponents) {
        if (directory.size() == wcslen(component.directory) &&
            _wcsnicmp(directory.data(), component.directory, directory.size()) == 0)
            return component.module;
    }
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::vector<HMODULE> LoadedComponents(const wchar_t* module_name) {
    std::vector<HMODULE> found;
    if (!module_name)
        return found;

    std::vector<HMODULE> modules(256);
    DWORD needed = 0;
    for (;;) {
        const DWORD size = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
        if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), size, &needed))
            return found;
        if (needed <= size) {
            modules.resize(needed / sizeof(HMODULE));
            break;
        }
        modules.resize(needed / sizeof(HMODULE));
    }

    for (HMODULE module : modules) {
        if (_wcsicmp(ComponentFileName(ModulePath(module)).c_str(), module_name) == 0)
            found.push_back(module);
    }
    return found;
}

bool PinModule(HMODULE module) {
    HMODULE pinned = nullptr;
    return module &&
           GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(module), &pinned) != 0;
}

HMODULE ModuleForAddress(const void* address) {
    HMODULE module = nullptr;
    if (!address ||
        !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &module))
        return nullptr;
    return module;
}

std::string ModuleNameForAddress(const void* address) {
    HMODULE module = ModuleForAddress(address);
    return module ? text::ToUtf8(ModuleFileName(module)) : std::string{};
}

} // namespace odg::paths
