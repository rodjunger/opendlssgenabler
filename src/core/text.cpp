#include "core/text.h"

#include <windows.h>

namespace odg::text {

std::string ToUtf8(std::wstring_view wide) {
    if (wide.empty())
        return {};
    const int length = static_cast<int>(wide.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr,
                                          nullptr);
    if (bytes <= 0)
        return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(std::string_view utf8) {
    if (utf8.empty())
        return {};
    const int length = static_cast<int>(utf8.size());
    const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
    if (chars <= 0)
        return {};
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, out.data(), chars);
    return out;
}

} // namespace odg::text
