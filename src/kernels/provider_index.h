#pragma once

#include <cstddef>
#include <cstdint>

#include <windows.h>

namespace odg::kernels {

// The runtime does not always hand over a container. It often selects an image
// itself and passes a bare cubin, which carries no PTX and so cannot be
// retargeted on its own.
//
// It does not have to be. NVIDIA's runtime already ships native cubins for
// older architectures beside the ones it selects, a matched pair per kernel,
// and it only passes the newer one because it has been told the GPU is newer.
// The index walks the runtime's image once and pairs them up, so the kernel the
// hardware can actually run is put back in place of the one it cannot.
void BuildProviderIndex(HMODULE provider);

// The runtime's own cubin for `arch`, matching the kernel `cubin` belongs to.
// Nothing is rewritten: these are NVIDIA's images, used as shipped. Returns
// nullptr when the runtime carries no image for this architecture.
const void* FindNativeCubin(const void* cubin, size_t size, uint32_t arch, size_t& out_size);

// Container a cubin was taken from, for a runtime that stores its images inside
// fatbins rather than beside them. Returns nullptr when it is not known.
const void* FindContainerForCubin(const void* cubin, size_t size, size_t& container_size);

} // namespace odg::kernels
