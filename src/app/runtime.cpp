#include "app/runtime.h"

#include "app/loader.h"
#include "app/settings.h"
#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "kernels/substitute.h"
#include "provider/multi_frame.h"
#include "proxy/proxy.h"
#include "spoof/nvapi.h"
#include "streamline/plugin_patch.h"
#include "streamline/streamline.h"
#include "version.h"

#include <atomic>
#include <string>
#include <vector>

namespace odg::app {
namespace {

constexpr wchar_t kConfigName[] = L"opendlssg.ini";

std::atomic<bool> g_initialized{false};

// Streamline refuses frame generation outright when hardware-accelerated GPU
// scheduling is off, and says so only in its own log, which a report rarely
// includes. The mode lives in the graphics driver key: 2 is on, 1 is off.
//
// The value exists only once the setting has been changed from the Windows
// default, so most machines have none and nothing is written for them. That
// suits the purpose: a machine with frame generation missing because scheduling
// was turned off is a machine where somebody turned it off.
void ReportHardwareScheduling() {
    DWORD mode = 0;
    DWORD size = sizeof(mode);
    DWORD type = REG_DWORD;
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE,
                                        L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                                        L"HwSchMode", RRF_RT_REG_DWORD, &type, &mode, &size);
    if (status != ERROR_SUCCESS || (mode != 1 && mode != 2))
        return;
    const bool enabled = mode == 2;
    log::Event(enabled ? log::Level::Info : log::Level::Warning, "hardware_scheduling",
               {log::Field::Bool("enabled", enabled),
                log::Field::Str("note", enabled ? ""
                                                : "Streamline refuses frame generation without "
                                                  "it: Windows Settings, System, Display, "
                                                  "Graphics, Default graphics settings")});
}

std::wstring ResolveRelative(const std::wstring& base, const std::wstring& path) {
    if (path.size() >= 2 && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\')))
        return path; // already absolute
    std::wstring resolved = base;
    if (!resolved.empty() && resolved.back() != L'\\')
        resolved += L'\\';
    resolved += path;
    return resolved;
}


// The callers told Ada. A redirected runtime is the frame-generation runtime
// under the file name it was configured as, and creates no kernels unless it is
// told Ada too, so its name joins the one it stands in for.
std::vector<std::wstring> SpoofCallers(const Settings& settings) {
    std::vector<std::wstring> callers = settings.spoof_callers;
    if (settings.redirect_runtime)
        callers.push_back(settings.runtime_file);
    return callers;
}

// Streamline reads these before it initializes, which is the only point early
// enough to raise its log level from outside the host.
void ArmStreamlineDiagnostics(const std::wstring& directory) {
    SetEnvironmentVariableW(L"SL_LOG_LEVEL", L"2");
    SetEnvironmentVariableW(L"SL_LOG_PATH", directory.c_str());
    log::Event(log::Level::Info, "streamline_diagnostics_armed",
               {log::Field::Path("path", directory.c_str())});
}

void ConfigureKernels(const Settings& settings, const std::wstring& log_directory) {
    kernels::Options options;
    options.enabled = settings.retarget_kernels;
    options.target_sm = static_cast<uint32_t>(settings.target_sm);
    options.multi_frame = settings.multi_frame;
    options.dump = settings.dump_kernels;
    options.dump_directory = log_directory + L"\\kernels";
    kernels::Configure(options);
}

void ConfigureRuntimeRedirect(const Settings& settings, const std::wstring& self_dir) {
    const std::wstring path = ResolveRelative(self_dir, settings.runtime_file);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log::Event(log::Level::Error, "runtime_redirect_missing",
                   {log::Field::Path("path", path.c_str())});
        return;
    }
    loader::SetRuntimeRedirect(path.c_str());
    log::Event(log::Level::Info, "runtime_redirect_configured",
               {log::Field::Path("path", path.c_str()), log::Field::Str("note", "experimental")});
}

// Hands each subsystem what it needs before any of them can run: the loader is
// started only once this returns.
void ApplySettings(const Settings& settings, const std::wstring& self_dir) {
    const std::wstring log_directory = ResolveRelative(self_dir, settings.log_directory);

    spoof::nvapi::SetSpoofEnabled(settings.spoof_arch_to_game);
    spoof::nvapi::SetSpoofCallers(SpoofCallers(settings));
    spoof::nvapi::SetStubScgPriority(settings.stub_scg_priority);

    streamline::SetPatchesEnabled(settings.patch_flip_metering, settings.patch_frame_clamp);
    streamline::SetFlipMeteringValue(settings.flip_metering_value);
    streamline::SetForceMultiplier(static_cast<uint32_t>(settings.force_multiplier));
    streamline::SetDiagnostics(settings.streamline_diagnostics);
    provider::SetMultiFrameEnabled(settings.multi_frame);

    ConfigureKernels(settings, log_directory);
    loader::SetVulkanHooksEnabled(settings.vulkan_hooks);
    if (settings.streamline_diagnostics)
        ArmStreamlineDiagnostics(log_directory);
    ReportHardwareScheduling();
    if (settings.redirect_runtime)
        ConfigureRuntimeRedirect(settings, self_dir);
}

} // namespace

void Initialize(HMODULE self) {
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true))
        return;

    const std::wstring self_dir = paths::ParentDirectory(paths::ModulePath(self));

    config::Ini ini;
    const std::wstring config_path = ResolveRelative(self_dir, kConfigName);
    const bool config_found = ini.Load(config_path);
    const Settings settings = Settings::FromIni(ini);

    log::Open(ResolveRelative(self_dir, settings.log_directory), settings.log_level, L"loader");
    log::Header("attach", {log::Field::Str("version", ODG_VERSION),
                           log::Field::Str("proxy", paths::ModuleFileName(self).c_str()),
                           log::Field::Path("host", paths::ModulePath(nullptr).c_str()),
                           log::Field::Path("config", config_path.c_str()),
                           log::Field::Bool("config_found", config_found)});
    for (const std::string& rejected : ini.Rejected())
        log::Event(log::Level::Warning, "config_value_rejected",
                   {log::Field::Str("setting", rejected),
                    log::Field::Str("note", "the default is used instead")});
    log::Header("configuration",
                {log::Field::Bool("enabled", settings.enabled),
                log::Field::Bool("spoof_arch_to_game", settings.spoof_arch_to_game),
                log::Field::Uint("spoof_callers", settings.spoof_callers.size()),
                log::Field::Bool("patch_flip_metering", settings.patch_flip_metering),
                log::Field::Int("flip_metering_value", settings.flip_metering_value),
                log::Field::Bool("patch_frame_clamp", settings.patch_frame_clamp),
                log::Field::Bool("stub_scg_priority", settings.stub_scg_priority),
                log::Field::Int("force_multiplier", settings.force_multiplier),
                log::Field::Bool("multi_frame", settings.multi_frame),
                log::Field::Bool("retarget_kernels", settings.retarget_kernels),
                log::Field::Int("target_sm", settings.target_sm),
                log::Field::Bool("vulkan_hooks", settings.vulkan_hooks),
                log::Field::Bool("redirect_runtime", settings.redirect_runtime)});

    const proxy::BindResult& bind = proxy::LastBind();
    if (bind.attempted) {
        const bool complete = bind.resolved == bind.total;
        log::Event(complete ? log::Level::Info : log::Level::Error, "proxy_bound",
                   {log::Field::Str("dll", bind.dll), log::Field::Uint("resolved", bind.resolved),
                    log::Field::Uint("total", bind.total)});
    }

    if (!settings.enabled) {
        log::Header("disabled");
        return;
    }

    ApplySettings(settings, self_dir);
    loader::Start();
}

void Shutdown() {
    if (!g_initialized.load(std::memory_order_acquire))
        return;
    log::Event(log::Level::Info, "detach", {});
    log::Close();
}

} // namespace odg::app
