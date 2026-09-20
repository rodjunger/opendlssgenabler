#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// In-memory patches to the Streamline DLSS-G plugin (sl.dlss_g.dll). They are
// applied only on a GPU this engine enables, and only to the mapped image.
//
// Flip metering. Ada and newer pace generated frames in hardware; Ampere cannot.
// The plugin believes it is on Ada, so it would choose hardware metering and the
// generated frames would never be presented. It already has a software
// fallback, which it takes when it detects an older frame-generation runtime:
// right after logging "FG1 DLL has been detected" it stores its metering flag's
// off value. That store tells us where the flag is and what off means, both of
// which change between plugin builds. Every store of the opposite value to the
// flag is then changed to store the off value.
//
// Frame-count clamp. The plugin limits the generated-frame count with
// `mov edx, limit; cmp ecx, edx; cmovb edx, ecx`. Replacing the cmovb with a
// no-op lifts the limit the plugin advertises, but not the runtime's own, so it
// is off by default.
//
// Technique adapted from the MIT-licensed RTX40MFG-Unlock and RTX40MFG-minimal
// projects.
namespace odg::streamline {

// Everything the patches would change in a plugin, found without changing it.
struct PluginAnalysis {
    struct FlipMetering {
        int32_t flag_offset = 0; // the flag's offset within the plugin's context
        uint8_t off_value = 0;
        std::vector<const std::byte*> opposite_stores; // immediates to rewrite
    };
    struct FrameClamp {
        uint32_t limit = 0;              // the plugin's compiled maximum
        const std::byte* cmov = nullptr; // the instruction that applies it
    };
    std::optional<FlipMetering> flip_metering;
    std::optional<FrameClamp> frame_clamp;
    const char* flip_metering_problem = ""; // why flip_metering is empty
    size_t frame_clamp_matches = 0;
};

// Finds the patch sites in a loaded sl.dlss_g.dll. Changes nothing.
PluginAnalysis AnalyzePlugin(HMODULE plugin);

// Selects which patches apply. Either can be turned off to isolate a fault.
void SetPatchesEnabled(bool flip_metering, bool frame_clamp);

// Value stored for the off state. Negative uses the plugin's own; 0 or 1 forces
// it, in case a future build is read wrongly.
void SetFlipMeteringValue(int value);

// Applies the selected patches to a loaded sl.dlss_g.dll. Idempotent.
void PatchPlugin(HMODULE plugin);

} // namespace odg::streamline
