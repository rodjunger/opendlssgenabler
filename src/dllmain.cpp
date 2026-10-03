#include "app/runtime.h"
#include "core/paths.h"
#include "proxy/proxy.h"

#include <windows.h>

namespace {

HMODULE g_self = nullptr;

// Initialization installs inline hooks, which briefly suspend the process's
// other threads. Doing that under the loader lock can deadlock, so it runs on a
// thread of its own.
DWORD WINAPI InitThread(LPVOID) {
    odg::app::Initialize(g_self);
    return 0;
}

} // namespace

// Only attach is handled. The module pins itself, so detach happens only at
// process exit, when Windows has already terminated every other thread,
// possibly while one held a lock cleanup would take. Doing nothing there is the
// only safe choice, and the log is flushed after every line anyway.
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    g_self = instance;
    DisableThreadLibraryCalls(instance);
    // The hooks and the worker thread run code in this module for the rest of
    // the process. A game that loads this DLL by name and later frees it would
    // unmap that code under them, so the module is pinned.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&DllMain), &pinned);

    // The forwarded exports are bound before the game can call any of them, so
    // this stays on the loader thread.
    const std::wstring name = odg::paths::ModuleFileName(instance);
    odg::proxy::BindActive(name.c_str());

    if (HANDLE thread = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr))
        CloseHandle(thread);
    return TRUE;
}
