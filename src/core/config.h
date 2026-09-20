#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

// Minimal, fault-tolerant INI reader. Section and key lookups are
// case-insensitive, a UTF-8 byte-order mark is ignored, and text after `;` or
// `#` that follows whitespace is a comment. A missing value yields the caller's
// default. A value that is present but malformed also yields the default, and is
// recorded so it can be reported: silently ignoring a setting a user wrote is
// worse than rejecting it loudly.
namespace odg::config {

class Ini {
public:
    bool Load(const std::wstring& path);

    std::string GetString(std::string_view section, std::string_view key,
                          std::string_view fallback = {}) const;
    int GetInt(std::string_view section, std::string_view key, int fallback) const;
    bool GetBool(std::string_view section, std::string_view key, bool fallback) const;

    // Records that a present value was rejected, for the caller to report.
    void Reject(std::string_view section, std::string_view key, std::string_view reason) const;

    // "[Section] Key: reason" for every rejected value, in lookup order.
    const std::vector<std::string>& Rejected() const { return rejected_; }

private:
    const std::string* Find(std::string_view section, std::string_view key) const;

    std::map<std::string, std::map<std::string, std::string>> sections_;
    mutable std::vector<std::string> rejected_;
};

} // namespace odg::config
