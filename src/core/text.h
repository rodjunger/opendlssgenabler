#pragma once

#include <string>
#include <string_view>

// Conversions between Windows wide strings and UTF-8. The INI file and the log
// are UTF-8; paths and module names come from Windows as UTF-16.
namespace odg::text {

std::string ToUtf8(std::wstring_view wide);
std::wstring FromUtf8(std::string_view utf8);

} // namespace odg::text
