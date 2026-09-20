#pragma once

#include <libhat/signature.hpp>

#include <cstddef>
#include <cstring>
#include <string_view>
#include <type_traits>

// Helpers for libhat signatures whose bytes are not all known when the code is
// written: a constant with a name, or a value read from the image.
namespace odg::signature {

// Parses an IDA-style pattern that is a constant in this code base, so a
// failure is a programming error rather than something to recover from.
inline hat::signature Parse(std::string_view pattern) {
    return hat::parse_signature(pattern).value();
}

// Appends `value` as it is stored in memory (little-endian), matched exactly.
template <typename T>
void Append(hat::signature& signature, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::byte bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    for (const std::byte byte : bytes)
        signature.emplace_back(byte);
}

// Matches `text` byte for byte.
inline hat::signature Literal(std::string_view text) {
    hat::signature signature;
    for (const char c : text)
        signature.emplace_back(static_cast<std::byte>(c));
    return signature;
}

} // namespace odg::signature
