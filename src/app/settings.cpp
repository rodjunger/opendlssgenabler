#include "app/settings.h"

#include "core/text.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace odg::app {
namespace {

// Reads an integer and rejects it when it falls outside [low, high].
int ReadRange(const config::Ini& ini, const char* section, const char* key, int fallback, int low,
              int high) {
    const int value = ini.GetInt(section, key, fallback);
    if (value >= low && value <= high)
        return value;
    ini.Reject(section, key,
               std::to_string(value) + " is outside " + std::to_string(low) + ".." +
                   std::to_string(high));
    return fallback;
}

// Accepts 0 as "not set" or a value in [low, high].
int ReadOptionalRange(const config::Ini& ini, const char* section, const char* key, int low,
                      int high) {
    const int value = ini.GetInt(section, key, 0);
    if (value == 0 || (value >= low && value <= high))
        return value;
    ini.Reject(section, key,
               std::to_string(value) + " is neither 0 nor within " + std::to_string(low) + ".." +
                   std::to_string(high));
    return 0;
}

// A comma-separated module list. "none" clears it, which makes the spoof
// unconditional; an empty value leaves the default in place.
std::vector<std::wstring> SplitModules(const std::string& value) {
    std::vector<std::wstring> modules;
    std::string item;
    for (char c : value + ",") {
        if (c != ',') {
            if (!std::isspace(static_cast<unsigned char>(c)))
                item += c;
            continue;
        }
        if (!item.empty() && item != "none")
            modules.push_back(text::FromUtf8(item));
        item.clear();
    }
    return modules;
}

} // namespace

Settings Settings::FromIni(const config::Ini& ini) {
    Settings s;
    s.enabled = ini.GetBool("General", "Enabled", s.enabled);

    s.spoof_arch_to_game = ini.GetBool("Compatibility", "SpoofArchToGame", s.spoof_arch_to_game);
    const std::string callers = ini.GetString("Compatibility", "SpoofCallers");
    if (!callers.empty())
        s.spoof_callers = SplitModules(callers);
    s.patch_flip_metering =
        ini.GetBool("Compatibility", "PatchFlipMetering", s.patch_flip_metering);
    s.flip_metering_value = ReadRange(ini, "Compatibility", "FlipMeteringValue", -1, -1, 1);
    s.patch_frame_clamp = ini.GetBool("Compatibility", "PatchFrameClamp", s.patch_frame_clamp);
    s.stub_scg_priority = ini.GetBool("Compatibility", "StubScgPriority", s.stub_scg_priority);
    s.force_multiplier = ReadOptionalRange(ini, "FrameGeneration", "ForceMultiplier", 2, 6);
    s.multi_frame = ini.GetBool("FrameGeneration", "MultiFrame", s.multi_frame);

    s.retarget_kernels = ini.GetBool("Kernels", "Retarget", s.retarget_kernels);
    s.target_sm = ReadOptionalRange(ini, "Kernels", "TargetSM", 50, 200);
    s.vulkan_hooks = ini.GetBool("Kernels", "VulkanHooks", s.vulkan_hooks);

    std::string mode = ini.GetString("Runtime", "Mode", "Off");
    std::transform(mode.begin(), mode.end(), mode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mode != "off" && mode != "bundled")
        ini.Reject("Runtime", "Mode", "'" + mode + "' is neither Off nor Bundled");
    s.redirect_runtime = mode == "bundled";
    const std::string runtime_file = ini.GetString("Runtime", "RuntimeFile");
    if (!runtime_file.empty())
        s.runtime_file = text::FromUtf8(runtime_file);

    s.streamline_diagnostics =
        ini.GetBool("Debug", "StreamlineDiagnostics", s.streamline_diagnostics);
    s.dump_kernels = ini.GetBool("Debug", "DumpKernels", s.dump_kernels);

    s.log_level = log::LevelFromSetting(ReadRange(ini, "Logging", "Level", 1, 0, 3));
    const std::string directory = ini.GetString("Logging", "Directory");
    if (!directory.empty())
        s.log_directory = text::FromUtf8(directory);
    return s;
}

} // namespace odg::app
