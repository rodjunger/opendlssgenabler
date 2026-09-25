#pragma once

#include "vulkan/loader_hooks.h"

// Reflex on Vulkan (VK_NV_low_latency2). With low-latency mode on, the driver
// expects a vkLatencySleepNV call every frame. A game that never makes one is
// paced by the driver inside vkQueuePresentKHR instead, one present at a time.
// With frame generation on, the presenter makes a present for every displayed
// frame, generated ones included, so that pacing holds the output to half the
// refresh rate while the GPU idles. No Man's Sky turns Reflex on and never
// sleeps.
//
// While frame generation is on and no vkLatencySleepNV has been seen,
// low-latency mode is passed to the driver as off. Otherwise the game's request
// is passed through; Streamline sends it again on every new swapchain, which is
// how the game's setting comes back after frame generation is switched off.
namespace odg::vulkan {

// Whether a request for low-latency mode must reach the driver as off. Each
// check that fails leaves the request as the game made it.
bool MustTurnLowLatencyOff(bool requested, bool frame_generation, bool game_sleeps);

// Follows the PatchFlipMetering setting.
void SetPresentPacingFix(bool enabled);

// The wrapper for a VK_NV_low_latency2 function, or null for any other
// function, or when the fix does not apply: another GPU, or the setting off.
PfnVoidFunction ReflexWrapper(Function function);

} // namespace odg::vulkan
