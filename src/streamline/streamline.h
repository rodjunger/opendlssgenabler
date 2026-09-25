#pragma once

#include <windows.h>

#include <cstdint>

// Hooks the Streamline interposer so the DLSS-G options the game sets pass
// through this engine. It logs what the game requests and what DLSS-G reports,
// which is the primary way to see why generation does or does not run, and can
// optionally hold the generated-frame count to a fixed multiplier.
namespace odg::streamline {

// 0 follows the game; 2..6 forces that multiplier within the runtime's ceiling.
void SetForceMultiplier(uint32_t multiplier);

// Raise Streamline's own log level and route its messages into our log. It
// explains its feature decisions there and nowhere else.
void SetDiagnostics(bool enabled);

bool InstallInterposerHooks(HMODULE interposer);
// Whether the game has most recently asked for frame generation to be on.
bool FrameGenerationOn();

} // namespace odg::streamline
