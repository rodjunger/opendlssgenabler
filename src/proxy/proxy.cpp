#include "proxy/proxy.h"

#include "core/paths.h"

#include <windows.h>

namespace odg::proxy {
namespace {
BindResult g_last_bind;
}

bool Bind(const wchar_t* real_dll, const Export* exports, size_t count) {
    g_last_bind = {real_dll, 0, count, true};

    HMODULE real = paths::LoadSystemLibrary(real_dll);
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
