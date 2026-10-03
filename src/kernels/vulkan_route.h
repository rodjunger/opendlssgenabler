#pragma once

#include "vulkan/loader_hooks.h"

namespace odg::kernels {

// A Vulkan game reaches the driver through VK_NVX_binary_import rather than
// NVAPI, so the kernel container arrives at vkCreateCuModuleNVX instead of the
// D3D12 cubin entry point. The wrapper for a VK_NVX_binary_import function, or
// null for any other function.
vulkan::PfnVoidFunction VulkanWrapper(vulkan::Function function);

} // namespace odg::kernels
