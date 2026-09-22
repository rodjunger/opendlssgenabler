#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

namespace odg::bytes {

// `size` bytes at `data`, or an empty span for a null pointer.
inline std::span<const uint8_t> View(const void* data, size_t size) {
    return data ? std::span(static_cast<const uint8_t*>(data), size) : std::span<const uint8_t>{};
}

// Reads a T stored at `offset`, which need not be aligned. Empty when it does
// not fit inside `data`.
template <typename T>
std::optional<T> Read(std::span<const uint8_t> data, size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (offset > data.size() || data.size() - offset < sizeof(T))
        return std::nullopt;
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

// The `size` bytes at `offset`. Empty when they do not fit inside `data`.
inline std::optional<std::span<const uint8_t>> Slice(std::span<const uint8_t> data, size_t offset,
                                                     size_t size) {
    if (offset > data.size() || data.size() - offset < size)
        return std::nullopt;
    return data.subspan(offset, size);
}

} // namespace odg::bytes
