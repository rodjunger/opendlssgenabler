#pragma once

#include "core/config.h"
#include "core/log.h"

#include <string>
#include <vector>

namespace odg::app {

// Everything opendlssg.ini can set. Defaults are what a user should run; every
// other value exists to isolate a fault. assets/opendlssg.ini documents each key.
struct Settings {
    bool enabled = true;

    // Report Ada to the components that gate frame generation. Off keeps the
    // real architecture and leaves the hooks in place for diagnosis.
    bool spoof_arch_to_game = true;

    // Modules told the GPU is Ada. sl.common decides whether frame generation
    // is offered at all, the NGX core answers the availability query the plugin
    // makes at startup, and NVIDIA's frame-generation runtime needs it to reach
    // its kernels. Anything not listed, including the game itself and the
    // upscaling runtimes, sees the real hardware.
    std::vector<std::wstring> spoof_callers = {L"sl.common.dll", L"_nvngx.dll",
                                              L"nvngx_dlssg.dll"};

    // Changes made to code rather than to a returned value. Each is separately
    // switchable because each could independently take the device down.
    bool patch_flip_metering = true;
    // Negative derives the value from the plugin's own code; 0 or 1 forces it.
    int flip_metering_value = -1;
    // Off by default: the plugin a game ships already allows what that game
    // supports.
    bool patch_frame_clamp = false;
    bool stub_scg_priority = true;

    // 0 follows the game's requested multiplier; 2..6 forces that multiplier.
    int force_multiplier = 0;
    // Let the runtime generate more than one frame (3x and above), as it does on
    // Blackwell, where the game supports it.
    bool multi_frame = true;

    // Supply kernels the GPU can run. Without this, frame generation is offered
    // and then has nothing to execute.
    bool retarget_kernels = true;
    // 0 asks the CUDA driver which architecture this is.
    int target_sm = 0;
    // Vulkan titles reach the driver through VK_NVX_binary_import instead of
    // NVAPI, so without this their frame generation has no kernels either.
    bool vulkan_hooks = true;

    // Experimental: load a different nvngx_dlssg.dll in place of the game's.
    bool redirect_runtime = false;
    std::wstring runtime_file = L"opendlssg_nvngx_dlssg.dll";

    // Capture Streamline's own diagnostics, which is where it states why it
    // accepts or refuses frame generation.
    bool streamline_diagnostics = false;
    bool dump_kernels = false;

    log::Level log_level = log::Level::Warning; // the INI's default, Level=1
    std::wstring log_directory = L"opendlssg\\logs";

    // Values that were present but rejected fall back to the default and are
    // reported through `ini.Rejected()` once logging is open.
    static Settings FromIni(const config::Ini& ini);
};

} // namespace odg::app
