#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace odg::kernels {

// NVAPI's cubin-creation entry point takes a versioned parameter block whose
// layout is not public and changes between NVAPI releases. Instead of fixing
// the field offsets, the block is searched for a pointer to a fatbin container
// and for the field holding the length that container states for itself. Both
// checks have to agree, which pins the layout without hardcoding it.
struct BlobFields {
    bool valid = false;
    size_t struct_size = 0;
    size_t data_offset = 0;
    size_t size_offset = 0;
    size_t name_offset = 0; // 0 when the block carries no entry-point name
    bool size_is_64bit = false;
};

BlobFields LocateBlob(const void* params);

const void* ReadBlob(const void* params, const BlobFields& fields, size_t& size);
std::string ReadName(const void* params, const BlobFields& fields);
void WriteBlob(void* params, const BlobFields& fields, const void* data, size_t size);

} // namespace odg::kernels
