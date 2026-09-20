#include "proxy/proxy.h"

#include "core/log.h"

#include <windows.h>

#include <string>

namespace odg::proxy {
namespace {
BindResult g_last_bind;
}

bool Bind(const wchar_t* real_dll, const Export* exports, size_t count) {
    g_last_bind = {real_dll, 0, count, true};

    wchar_t system_dir[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    std::wstring path(system_dir, length);
    path += L'\\';
    path += real_dll;

    HMODULE real = LoadLibraryW(path.c_str());
    if (!real)
        return false;

    size_t resolved = 0;
    for (size_t i = 0; i < count; ++i) {
        auto* fn = reinterpret_cast<void*>(GetProcAddress(real, exports[i].name));
        *exports[i].slot = fn;
        if (fn)
            ++resolved;
    }
    g_last_bind.resolved = resolved;
    return resolved == count;
}

const BindResult& LastBind() {
    return g_last_bind;
}

} // namespace odg::proxy
