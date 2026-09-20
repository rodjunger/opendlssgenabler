#pragma once

#include <cstdint>

namespace odg::kernels {

// NV_GPU_ARCHITECTURE_ID values from the public NVAPI headers.
enum NvApiArchitecture : uint32_t {
    kNvApiTuring = 0x160,    // TU10x, RTX 20
    kNvApiAmpere = 0x170,    // GA10x, RTX 30
    kNvApiAda = 0x190,       // AD10x, RTX 40
    kNvApiBlackwell = 0x1B0, // GB20x, RTX 50
};

// SM number of the GPU in this process, e.g. 86 for a GA10x card. Read from the
// CUDA driver's own compute-capability attributes, which is the same number
// ptxas targets. Returns 0 when no CUDA device answers.
uint32_t DetectDeviceSm();

// SM number for an NVAPI architecture and implementation id, used when the CUDA
// driver cannot be reached. Returns 0 for an architecture with no mapping.
uint32_t SmFromNvApiArch(uint32_t architecture, uint32_t implementation);

} // namespace odg::kernels
