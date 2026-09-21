#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

// Multi-frame generation (3x and above) in NVIDIA's DLSS-G runtime.
//
// The runtime generates more than one frame only on Blackwell. It decides by
// comparing the architecture it was told against Blackwell's NVAPI id,
// `cmp r32, 0x1B0`: where it publishes DLSSG.MultiFrameCountMax, and where it
// validates the count a game asks for. Moving those comparisons to the Ada id,
// which is what this engine reports to the runtime, makes it treat the GPU as
// multi-frame capable, as RTX40MFG-Unlock and mfg-unlock do on RTX 40 cards.
// The rule for telling a gate from any other comparison against that id is
// mfg-unlock's, whose analysis of 310.6 through 310.8 found that the form the
// comparison takes is a compiler decision and its reader is not.
//
// A comparison is a gate only when its result is read as an ordering: the
// runtime gives the feature to every architecture at or above Blackwell, so it
// branches on `jl`, `setae`, `cmovl` and the like. Which of those a build uses
// is a compiler decision rather than a version one. A comparison read for
// equality is asking whether the GPU is one exact architecture, and moving the
// id it is compared against would change that question instead of answering it
// differently, so it is left alone.
//
// The same id also gates other Blackwell capabilities, such as
// DLSSG.ReflexWarp.Available in 310.3. A comparison whose result is published
// as any DLSSG parameter other than the multi-frame count is left as it is.
// Multi-frame also needs the kernels built for it, which
// kernels::Options::multi_frame selects.
namespace odg::provider {

struct Gate {
    const std::byte* immediate = nullptr; // the imm32 compared against
    std::string publishes;                // DLSSG parameter it feeds, if found
    const char* condition = "";           // how its result is read
    bool unlock = false;                  // whether it is rewritten
};

// Every comparison against Blackwell's id, classified. Changes nothing.
std::vector<Gate> FindMultiFrameGates(HMODULE provider);

void SetMultiFrameEnabled(bool enabled);
bool MultiFrameEnabled();

// Rewrites the gates once per process. `at_load` records whether this ran
// inside the load, before any of the runtime's code could run.
void UnlockMultiFrame(HMODULE provider, bool at_load);

} // namespace odg::provider
