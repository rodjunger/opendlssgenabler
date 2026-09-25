#pragma once

namespace odg::kernels {

// A Vulkan game reaches the driver through VK_NVX_binary_import rather than
// NVAPI, so the kernel container arrives at vkCreateCuModuleNVX instead of the
// D3D12 cubin entry point. The extension function is resolved at runtime, so it
// is intercepted where it is handed out.
//
// Hooks the Vulkan loader once it is loaded, and keeps it loaded from then on.
// Returns false while it is not loaded, so a module scan can simply call again.
bool InstallVulkanHooks();

// While frame generation is on and the game never calls vkLatencySleepNV, pass
// Reflex low-latency mode to the driver as off, so it does not pace every
// generated frame as a whole one. Follows the PatchFlipMetering setting.
void SetPresentPacingFix(bool enabled);

} // namespace odg::kernels
