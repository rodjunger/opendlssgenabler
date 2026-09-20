#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Cubins: CUDA machine code for one architecture, stored as an ELF64 image.
namespace odg::kernels {

// SM number a cubin targets, e.g. 89 for sm_89. A runtime that has already
// chosen an image hands one of these over instead of a container, and it
// carries no PTX, so there is nothing to retarget. Returns 0 when the blob is
// not a cubin.
uint32_t CubinArch(const void* blob, size_t size);

// Length of the cubin at `blob`, taken from its own headers, or 0 when it is not
// a cubin that fits in `available` bytes. Callers are often handed a pointer
// with a rounded-up or unknown length, which this makes exact.
size_t CubinSize(const void* blob, size_t available);

struct CubinSection {
    std::string_view name;
    std::span<const uint8_t> data;
};

// Every section of a cubin, named. Empty when the headers do not validate;
// sections whose data or name fall outside the image are left out.
std::vector<CubinSection> CubinSections(const void* blob, size_t available);

} // namespace odg::kernels
