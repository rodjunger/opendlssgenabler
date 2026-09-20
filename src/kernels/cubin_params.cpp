#include "kernels/cubin_params.h"

#include "core/pe.h"
#include "kernels/fatbin.h"

#include <array>
#include <cctype>
#include <cstring>
#include <optional>
#include <utility>

namespace odg::kernels {
namespace {

// The block opens with its own size. A value outside this range means the read
// found something else, and the search falls back to a fixed window.
constexpr size_t kMinStructSize = 0x20;
constexpr size_t kMaxStructSize = 0x400;
constexpr size_t kFallbackWindow = 0x80;
// Enough of the container to read its header and validate the stated length.
constexpr size_t kProbeBytes = 32;
constexpr size_t kMaxNameBytes = 96;

// An entry-point name: a C identifier of a plausible length. Used to label the
// kernel in the log, never to decide what to substitute.
bool LooksLikeEntryName(const char* text, size_t size) {
    if (size < 3 || !(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_'))
        return false;
    for (size_t i = 0; i < size; ++i) {
        if (text[i] == '\0')
            return i >= 3;
        if (!std::isalnum(static_cast<unsigned char>(text[i])) && text[i] != '_')
            return false;
    }
    return false;
}

// A copy of the T at `address`, or empty when it cannot be read.
template <typename T>
std::optional<T> ReadAt(const void* address) {
    T value{};
    if (!pe::SafeCopy(&value, address, sizeof(value)))
        return std::nullopt;
    return value;
}

size_t StructSize(const void* params) {
    const auto declared = ReadAt<uint64_t>(params);
    if (!declared)
        return 0;
    if (*declared >= kMinStructSize && *declared <= kMaxStructSize)
        return static_cast<size_t>(*declared);
    return kFallbackWindow;
}

// The pointer field that points at a fatbin container. Returns its offset and
// the container's length, which is what the size field has to hold.
std::optional<std::pair<size_t, size_t>> FindDataField(const uint8_t* base, size_t window) {
    for (size_t offset = sizeof(uint64_t); offset + sizeof(void*) <= window;
         offset += sizeof(void*)) {
        const auto candidate = ReadAt<const void*>(base + offset);
        const auto probe = candidate && *candidate
                               ? ReadAt<std::array<uint8_t, kProbeBytes>>(*candidate)
                               : std::nullopt;
        if (const size_t total = probe ? ContainerSize(probe->data(), probe->size()) : 0)
            return std::pair(offset, total);
    }
    return std::nullopt;
}

// The pointer field that points at the kernel's entry-point name.
size_t FindNameField(const uint8_t* base, size_t window, size_t data_offset) {
    for (size_t offset = sizeof(uint64_t); offset + sizeof(void*) <= window;
         offset += sizeof(void*)) {
        if (offset == data_offset)
            continue;
        const auto candidate = ReadAt<const void*>(base + offset);
        const auto text = candidate && *candidate
                              ? ReadAt<std::array<char, kMaxNameBytes>>(*candidate)
                              : std::nullopt;
        if (text && LooksLikeEntryName(text->data(), text->size()))
            return offset;
    }
    return 0;
}

// The field holding the container's length, 32 or 64 bits wide. A 64-bit field
// is recognised by its high half being zero as well.
bool FindSizeField(const uint8_t* base, size_t window, size_t container_size, BlobFields& fields) {
    for (size_t offset = sizeof(uint64_t); offset + sizeof(uint32_t) <= window;
         offset += sizeof(uint32_t)) {
        if (ReadAt<uint32_t>(base + offset) != container_size)
            continue;
        fields.size_offset = offset;
        fields.size_is_64bit = offset + sizeof(uint64_t) <= window &&
                               ReadAt<uint64_t>(base + offset) == container_size;
        return true;
    }
    return false;
}

} // namespace

BlobFields LocateBlob(const void* params) {
    BlobFields fields;
    const size_t window = params ? StructSize(params) : 0;
    if (!window)
        return fields;
    fields.struct_size = window;
    const auto* base = static_cast<const uint8_t*>(params);

    const auto data = FindDataField(base, window);
    if (!data)
        return fields;
    const auto [data_offset, container_size] = *data;
    fields.data_offset = data_offset;
    fields.name_offset = FindNameField(base, window, data_offset);
    fields.valid = FindSizeField(base, window, container_size, fields);
    return fields;
}

const void* ReadBlob(const void* params, const BlobFields& fields, size_t& size) {
    size = 0;
    if (!fields.valid || !params)
        return nullptr;
    const auto* base = static_cast<const uint8_t*>(params);
    const void* data = nullptr;
    if (!pe::SafeCopy(&data, base + fields.data_offset, sizeof(data)))
        return nullptr;
    uint64_t stored = 0;
    const size_t width = fields.size_is_64bit ? sizeof(uint64_t) : sizeof(uint32_t);
    if (!pe::SafeCopy(&stored, base + fields.size_offset, width))
        return nullptr;
    size = static_cast<size_t>(stored);
    return data;
}

std::string ReadName(const void* params, const BlobFields& fields) {
    if (!fields.name_offset || !params)
        return {};
    const auto* base = static_cast<const uint8_t*>(params);
    const char* text = nullptr;
    if (!pe::SafeCopy(&text, base + fields.name_offset, sizeof(text)) || !text)
        return {};
    char buffer[kMaxNameBytes] = {};
    if (!pe::SafeCopy(buffer, text, sizeof(buffer)))
        return {};
    buffer[kMaxNameBytes - 1] = '\0';
    return std::string(buffer);
}

void WriteBlob(void* params, const BlobFields& fields, const void* data, size_t size) {
    if (!fields.valid || !params)
        return;
    auto* base = static_cast<uint8_t*>(params);
    std::memcpy(base + fields.data_offset, &data, sizeof(data));
    if (fields.size_is_64bit) {
        const uint64_t wide = size;
        std::memcpy(base + fields.size_offset, &wide, sizeof(wide));
    } else {
        const uint32_t narrow = static_cast<uint32_t>(size);
        std::memcpy(base + fields.size_offset, &narrow, sizeof(narrow));
    }
}

} // namespace odg::kernels
