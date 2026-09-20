#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Reports a supported architecture to the components that gate DLSS-G, without
// changing the hardware. NvAPI_GPU_GetArchInfo is hooked inline, and an Ampere
// answer is rewritten to Ada for the callers listed in SetSpoofCallers only.
// Every other caller, the game included, sees the real architecture.
namespace odg::spoof::nvapi {

// Hooks the NVAPI entry points on a loaded nvapi64.dll. Idempotent, and safe to
// retry: a failed attempt leaves nothing installed.
bool Install(HMODULE nvapi);

// Whether GetArchInfo results are rewritten. The hook stays installed either
// way so a disabled run still records what was asked.
void SetSpoofEnabled(bool enabled);

// Modules told the GPU is Ada. An empty list tells every caller, which is a
// diagnostic and not a setting: a game that asks the architecture switches on
// rendering paths that do not exist on the hardware underneath.
void SetSpoofCallers(const std::vector<std::wstring>& modules);

// NvAPI_D3D12_SetRawScgPriority is an Ada-and-newer call that removes the
// device when it fails on older hardware. While spoofing it is answered with
// success and otherwise ignored.
void SetStubScgPriority(bool enabled);

// What the GPU actually is, as NVAPI reports it before any rewrite. Unknown
// until the first successful query.
enum class Architecture { Unknown, Supported, NotNeeded };

// Supported: an architecture this engine enables, which today means Ampere.
// NotNeeded: anything else, including GPUs that already run DLSS-G natively and
// Turing, whose kernels cannot be supplied yet (see docs/ARCHITECTURE.md).
Architecture RealArchitecture();

} // namespace odg::spoof::nvapi
