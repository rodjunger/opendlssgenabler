#include "core/config.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace odg::config {
namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool IsSpace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

// A `;` or `#` starts a comment only after whitespace, so a value that contains
// one, such as a path, is kept whole.
std::string_view StripComment(std::string_view value) {
    for (size_t i = 1; i < value.size(); ++i) {
        if ((value[i] == ';' || value[i] == '#') && IsSpace(value[i - 1]))
            return value.substr(0, i);
    }
    return value;
}

} // namespace

bool Ini::Load(const std::wstring& path) {
    sections_.clear();
    rejected_.clear();

    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file)
        return false;
    std::string content;
    char chunk[4096];
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
        content.append(chunk, read);
    std::fclose(file);

    std::string_view text(content);
    if (text.substr(0, kUtf8Bom.size()) == kUtf8Bom)
        text.remove_prefix(kUtf8Bom.size());

    std::string section;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        const std::string_view line = Trim(StripComment(text.substr(0, end)));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);

        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue;
        std::string key = Lower(Trim(line.substr(0, eq)));
        if (!key.empty())
            sections_[section][std::move(key)] = std::string(Trim(line.substr(eq + 1)));
    }
    return true;
}

const std::string* Ini::Find(std::string_view section, std::string_view key) const {
    const auto s = sections_.find(Lower(section));
    if (s == sections_.end())
        return nullptr;
    const auto k = s->second.find(Lower(key));
    return k == s->second.end() ? nullptr : &k->second;
}

void Ini::Reject(std::string_view section, std::string_view key, std::string_view reason) const {
    std::string entry = "[";
    entry.append(section).append("] ").append(key).append(": ").append(reason);
    rejected_.push_back(std::move(entry));
}

std::string Ini::GetString(std::string_view section, std::string_view key,
                           std::string_view fallback) const {
    const std::string* value = Find(section, key);
    return value ? *value : std::string(fallback);
}

int Ini::GetInt(std::string_view section, std::string_view key, int fallback) const {
    const std::string* value = Find(section, key);
    if (!value || value->empty())
        return fallback;
    int parsed = 0;
    const char* end = value->data() + value->size();
    const auto [stop, error] = std::from_chars(value->data(), end, parsed);
    if (error != std::errc() || stop != end) {
        Reject(section, key, "'" + *value + "' is not a whole number");
        return fallback;
    }
    return parsed;
}

bool Ini::GetBool(std::string_view section, std::string_view key, bool fallback) const {
    const std::string* value = Find(section, key);
    if (!value || value->empty())
        return fallback;
    const std::string lowered = Lower(*value);
    if (lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes")
        return true;
    if (lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no")
        return false;
    Reject(section, key, "'" + *value + "' is not 0 or 1");
    return fallback;
}

} // namespace odg::config
