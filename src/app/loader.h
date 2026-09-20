#pragma once

// Watches the process for the NVIDIA modules the engine attaches to. A worker
// thread scans loaded modules whenever a library loads and once a second
// besides, because modules pulled in as static imports never pass through the
// LoadLibrary hook under their own name.
namespace odg::app::loader {

// Experimental: loads this absolute path in place of the game's
// nvngx_dlssg.dll. Must be set before Start.
void SetRuntimeRedirect(const wchar_t* absolute_runtime_path);

// Covers Vulkan titles, which hand their kernels to the driver through
// VK_NVX_binary_import instead of NVAPI.
void SetVulkanHooksEnabled(bool enabled);

void Start();

} // namespace odg::app::loader
