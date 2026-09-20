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

} // namespace odg::kernels
