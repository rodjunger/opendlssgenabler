#include "app/loader.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/paths.h"
#include "kernels/provider_index.h"
#include "kernels/substitute.h"
#include "kernels/vulkan_hooks.h"
#include "provider/multi_frame.h"
#include "provider/version_policy.h"
#include "spoof/nvapi.h"
#include "streamline/plugin_patch.h"
#include "streamline/streamline.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace odg::app::loader {
namespace {

// How often the worker rescans when nothing wakes it. Modules pulled in as
// static imports never pass through LoadLibraryExW, so they are only found by
// this rescan.
constexpr DWORD kRescanMilliseconds = 1000;

// NVAPI installation is retried because the first attempt can race the module's
// own initialization. A failure that persists is permanent, and retrying it
// forever would only fill the log.
constexpr int kNvapiAttempts = 5;

// Library loads that map a file as data rather than code. Nothing the engine
// watches for arrives this way.
constexpr DWORD kDataOnlyLoad =
    LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;

// NVIDIA's frame-generation runtime, as games and NGX load it.
constexpr wchar_t kRuntimeName[] = L"nvngx_dlssg.dll";

using PfnLoadLibraryExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);

std::atomic<PfnLoadLibraryExW> g_load_library_ex_w{nullptr};
std::atomic<bool> g_vulkan_hooks{false};
// Every DLSS-G runtime this process has mapped, in the order they arrived.
// A driver profile with the DLSS override enabled makes NGX load a runtime of
// its own beside the one the game ships, and the kernels come from whichever it
// picked, so both are prepared and both are indexed.
std::mutex g_runtimes_mutex;
std::vector<HMODULE> g_runtimes;
std::atomic<int> g_nvapi_attempts{0};
HANDLE g_wake = nullptr;

std::wstring g_redirect_path;
std::atomic<bool> g_redirect_reported{false};
thread_local bool t_in_redirect = false;

// NGX stores the runtime it downloads as <architecture>_<application id>.bin in
// its model directory, so a load is recognised by the component the path names,
// not by the file name it carries.
bool NamesRuntime(LPCWSTR file_name) {
    // ComponentFileName canonicalises only the NGX store's names; every other
    // path keeps the case its caller wrote, and Windows file names do not care.
    return file_name && paths::FileNameEqualsInsensitive(paths::ComponentFileName(file_name),
                                                        kRuntimeName);
}

// Records a runtime and returns false when it was already known, so each is
// prepared once. NGX unloads a runtime once it has read what it wanted from it,
// and this handle is kept and re-examined on every rescan, so it is pinned
// first. Pinning before the lock is taken, rather than under it, keeps this
// engine's lock off the path to the loader lock: PinModule takes the loader
// lock, and this function itself runs under it, inside the LoadLibraryExW hook.
bool Known(HMODULE module) {
    std::lock_guard lock(g_runtimes_mutex);
    return std::find(g_runtimes.begin(), g_runtimes.end(), module) != g_runtimes.end();
}

bool RecordRuntime(HMODULE module) {
    if (!module || Known(module))
        return false;
    // Pinned outside the lock, because PinModule takes the loader lock and this
    // function itself runs under it, inside the LoadLibraryExW hook. Holding
    // this engine's lock across that acquisition would invert the two.
    if (!paths::PinModule(module)) {
        // The handle was stale, or the loader refused. Either way the image
        // cannot be relied on to stay mapped, so it is left alone rather than
        // read and indexed. A module that has already gone is not enumerated
        // again, so this is reported at most once for it.
        log::Event(log::Level::Warning, "provider_pin_failed",
                   {log::Field::Str("note", "runtime not indexed; it may already be unloaded")});
        return false;
    }
    std::lock_guard lock(g_runtimes_mutex);
    if (std::find(g_runtimes.begin(), g_runtimes.end(), module) != g_runtimes.end())
        return false;
    g_runtimes.push_back(module);
    return true;
}

std::vector<HMODULE> KnownRuntimes() {
    std::lock_guard lock(g_runtimes_mutex);
    return g_runtimes;
}

// Experimental: loads the configured runtime in place of whichever one is being
// asked for, the game's or the one NGX downloaded for a driver profile with the
// DLSS override enabled. The thread guard stops that load from recursing back
// here.
HMODULE MaybeRedirect(LPCWSTR file_name) {
    if (g_redirect_path.empty() || t_in_redirect || !NamesRuntime(file_name))
        return nullptr;

    t_in_redirect = true;
    PfnLoadLibraryExW load = g_load_library_ex_w.load(std::memory_order_acquire);
    HMODULE module = load ? load(g_redirect_path.c_str(), nullptr, 0) : nullptr;
    t_in_redirect = false;

    if (!g_redirect_reported.exchange(true))
        log::Event(module ? log::Level::Info : log::Level::Error, "runtime_redirect",
                   {log::Field::Path("to", g_redirect_path.c_str()),
                    log::Field::Bool("ok", module != nullptr)});
    return module;
}

void ReportProvider(HMODULE module) {
    const std::wstring path = paths::ModulePath(module);
    provider::Version version;
    provider::ReadVersion(path.c_str(), version);
    log::Header("provider_found",
               {log::Field::Path("path", path.c_str()),
                log::Field::Str("version", provider::ToString(version)),
                log::Field::Bool("tested", provider::IsTested(version))});
    if (!provider::IsTested(version))
        log::Event(log::Level::Warning, "provider_untested",
                   {log::Field::Str("version", provider::ToString(version)),
                    log::Field::Str("note", "not verified yet; please report whether it works")});
}

// Called on every module scan. Indexing happens before the runtime creates any
// kernel, and resolving the target here keeps the cost of initializing CUDA off
// the game's render thread.
//
// Every mapped runtime is indexed, not only the first: which of them creates
// the kernels is NGX's choice, made later, and an index built from one build
// holds no image that belongs in another.
void InspectProvider() {
    // Recorded, and so pinned, before anything reads the image: both the gate
    // scan and the index read the module's sections directly, and a runtime NGX
    // has replaced can be unloaded between this enumeration and those reads.
    // A runtime loaded under a file name of its own, which a redirect does, is
    // already on the list from the load hook.
    for (HMODULE module : paths::LoadedComponents(kRuntimeName))
        RecordRuntime(module);

    for (HMODULE module : KnownRuntimes()) {
        if (!provider::IsDlssgProvider(module) || kernels::ProviderIndexed(module))
            continue;
        ReportProvider(module);
        // Normally already done inside the load; this covers a runtime that
        // arrived some other way.
        provider::UnlockMultiFrame(module, false);
        kernels::BuildProviderIndex(module);
    }

    // Resolving the target initializes CUDA, which belongs on this thread and
    // not on the render thread that creates the first kernel. It answers
    // nothing until NVAPI has identified the GPU, so it is asked on every scan
    // until it does; after that it is an atomic load.
    kernels::TargetSm();
}

// Runs on the worker thread and once from Start, never under the loader lock:
// installing an inline hook briefly suspends the other threads, which under the
// loader lock can deadlock the thread performing a load.
void InspectLoadedModules() {
    if (HMODULE nvapi = GetModuleHandleW(L"nvapi64.dll")) {
        if (g_nvapi_attempts.load(std::memory_order_relaxed) < kNvapiAttempts &&
            !spoof::nvapi::Install(nvapi))
            g_nvapi_attempts.fetch_add(1, std::memory_order_relaxed);
    }

    // A Vulkan title hands its kernels to the driver through
    // VK_NVX_binary_import. The extension function is resolved through
    // vkGetDeviceProcAddr, and every resolution funnels through the loader.
    if (g_vulkan_hooks.load(std::memory_order_acquire))
        kernels::InstallVulkanHooks();

    if (HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll"))
        streamline::InstallInterposerHooks(interposer);

    // The plugin patches change how an older GPU paces frames. A GPU that has
    // hardware flip metering must keep it, so they wait until NVAPI has said
    // what the GPU really is, and are only applied when it needs them.
    //
    // A game's own plugin and one NGX downloaded can both be mapped, and
    // Streamline picks the newer, so every copy is patched.
    if (spoof::nvapi::RealArchitecture() == spoof::nvapi::Architecture::Supported) {
        for (HMODULE plugin : paths::LoadedComponents(L"sl.dlss_g.dll"))
            streamline::PatchPlugin(plugin);
    }

    InspectProvider();
}

// Each NVIDIA component named once, when it first appears, so a report says
// which of them were in play.
void ReportNvidiaModules() {
    static const wchar_t* const kNames[] = {
        L"nvapi64.dll",   L"nvcuda.dll",        L"vulkan-1.dll",  L"_nvngx.dll",
        L"nvngx_dlss.dll", L"nvngx_dlssd.dll",  L"nvngx_dlssg.dll", L"sl.interposer.dll",
        L"sl.common.dll", L"sl.dlss_g.dll",
    };
    static std::atomic<uint32_t> reported{0};
    uint32_t seen = 0;
    for (size_t i = 0; i < std::size(kNames); ++i) {
        if (GetModuleHandleW(kNames[i]))
            seen |= 1u << i;
    }
    const uint32_t fresh = seen & ~reported.fetch_or(seen);
    for (size_t i = 0; i < std::size(kNames); ++i) {
        if (fresh & (1u << i)) {
            HMODULE module = GetModuleHandleW(kNames[i]);
            log::Event(log::Level::Info, "module_present",
                       {log::Field::Str("name", kNames[i]),
                        log::Field::Path("path", paths::ModulePath(module).c_str())});
        }
    }
}

// The runtime publishes its capabilities as soon as NGX asks, which can happen
// before the worker wakes. Rewriting a few bytes suspends no other thread, so
// unlike installing a hook it is safe here, inside the load.
void PrepareRuntime(HMODULE module) {
    if (!provider::IsDlssgProvider(module) || !RecordRuntime(module))
        return;
    provider::UnlockMultiFrame(module, true);
}

// Otherwise does the minimum the loader lock allows: wakes the worker. On
// Windows 10 and later every LoadLibrary variant funnels into LoadLibraryExW, so
// this one hook sees them all.
HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR file_name, HANDLE file, DWORD flags) {
    const bool code = (flags & kDataOnlyLoad) == 0;
    if (code) {
        if (HMODULE redirected = MaybeRedirect(file_name)) {
            PrepareRuntime(redirected);
            return redirected;
        }
    }
    PfnLoadLibraryExW original = g_load_library_ex_w.load(std::memory_order_acquire);
    HMODULE module = original ? original(file_name, file, flags) : nullptr;
    if (module && code) {
        if (NamesRuntime(file_name))
            PrepareRuntime(module);
        if (g_wake)
            SetEvent(g_wake);
    }
    return module;
}

DWORD WINAPI WorkerThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_wake, kRescanMilliseconds);
        InspectLoadedModules();
        ReportNvidiaModules();
        kernels::ReportSummary();
    }
}

} // namespace

void SetVulkanHooksEnabled(bool enabled) {
    g_vulkan_hooks.store(enabled, std::memory_order_release);
}

void SetRuntimeRedirect(const wchar_t* absolute_runtime_path) {
    if (absolute_runtime_path)
        g_redirect_path = absolute_runtime_path;
}

void Start() {
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hooks::InstallExport(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryExW",
                         reinterpret_cast<void*>(&HookedLoadLibraryExW), g_load_library_ex_w);

    // Streamline records the adapter architecture once, early in its own
    // startup, and every later decision about frame generation compares
    // against that record. nvapi64.dll usually arrives as a static import of
    // sl.common.dll, so it never passes through LoadLibraryExW under its own
    // name and a rescan would find it too late. Mapping it here, on this
    // thread, makes the ordering certain: whoever loads it afterwards receives
    // the module already hooked.
    if (!GetModuleHandleW(L"nvapi64.dll"))
        LoadLibraryW(L"nvapi64.dll");
    InspectLoadedModules();

    if (HANDLE thread = CreateThread(nullptr, 0, &WorkerThread, nullptr, 0, nullptr))
        CloseHandle(thread);
    log::Event(log::Level::Info, "loader_started", {});
}

} // namespace odg::app::loader
