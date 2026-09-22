#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

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
//
// More than one DLSS-G runtime can be mapped at once: NGX loads a copy it
// downloaded in place of the one a game ships whenever a driver profile enables
// the DLSS override. Each runtime is therefore indexed on its own and answers
// only for the kernels it created. An image from one build is not a replacement
// for a kernel of another, and the driver refuses it as an invalid image.
// The index points into the image, so the caller pins the module first, and
// builds each index from one thread: the loader's worker.
struct IndexSummary {
    size_t containers = 0;
    size_t cubins = 0;                    // outside any container
    size_t kernels_with_alternatives = 0; // with a cubin for more than one architecture
    size_t ambiguous_images = 0;          // in two containers, so answering nothing
};
// Empty when the runtime was already indexed.
std::optional<IndexSummary> BuildProviderIndex(HMODULE provider);

// Whether this runtime has been indexed. A runtime that has not been cannot be
// answered from another one's images.
bool ProviderIndexed(HMODULE provider);

// The one indexed runtime, when exactly one is indexed, and null otherwise.
// A call whose own module cannot be determined, which happens when another tool
// has hooked the same entry point and calls through its own trampoline, can
// still be answered while there is only one runtime it could have come from.
HMODULE SoleIndexedProvider();

// `provider`'s own cubin for `arch`, matching the kernel `cubin` belongs to.
// Nothing is rewritten: these are NVIDIA's images, used as shipped. Returns
// nullptr when the runtime carries no image for this architecture, or when it
// is not the runtime that was indexed.
const void* FindNativeCubin(HMODULE provider, const void* cubin, size_t size, uint32_t arch,
                            size_t& out_size);

// Container a cubin was taken from, for a runtime that stores its images inside
// fatbins rather than beside them. Returns nullptr when it is not known.
const void* FindContainerForCubin(HMODULE provider, const void* cubin, size_t size,
                                  size_t& container_size);

} // namespace odg::kernels
