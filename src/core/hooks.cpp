#include "core/hooks.h"

#include "core/log.h"

#include <MinHook.h>

#include <mutex>

namespace odg::hooks {
namespace {

std::once_flag g_init_once;
bool g_initialized = false;

} // namespace

bool Init() {
    std::call_once(g_init_once, [] {
        const MH_STATUS status = MH_Initialize();
        g_initialized = status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
        if (!g_initialized)
            log::Event(log::Level::Error, "minhook_init_failed",
                       {log::Field::Str("status", MH_StatusToString(status))});
    });
    return g_initialized;
}

namespace detail {

bool Create(void* target, void* detour, void** original, const char* name) {
    if (!target || !detour || !Init())
        return false;
    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK)
        return true;
    log::Event(log::Level::Error, "hook_failed",
               {log::Field::Str("name", name), log::Field::Str("stage", "create"),
                log::Field::Str("status", MH_StatusToString(status))});
    return false;
}

bool Enable(void* target, const char* name) {
    const MH_STATUS status = MH_EnableHook(target);
    if (status == MH_OK) {
        log::Event(log::Level::Info, "hook_installed", {log::Field::Str("name", name)});
        return true;
    }
    MH_RemoveHook(target);
    log::Event(log::Level::Error, "hook_failed",
               {log::Field::Str("name", name), log::Field::Str("stage", "enable"),
                log::Field::Str("status", MH_StatusToString(status))});
    return false;
}

void* Export(HMODULE module, const char* name) {
    void* target = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
    if (!target)
        log::Event(log::Level::Info, "hook_export_missing", {log::Field::Str("name", name)});
    return target;
}

} // namespace detail
} // namespace odg::hooks
