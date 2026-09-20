#include "core/log.h"

#include "core/text.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

namespace odg::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::atomic<Level> g_level{Level::Off};

std::string EscapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

void CreateDirectories(const std::wstring& path) {
    std::wstring current;
    current.reserve(path.size());
    for (wchar_t c : path) {
        if (c == L'\\' || c == L'/') {
            if (current.size() > 2 || (current.size() == 2 && current[1] != L':'))
                CreateDirectoryW(current.c_str(), nullptr);
            current += L'\\';
        } else {
            current += c;
        }
    }
    if (!current.empty())
        CreateDirectoryW(current.c_str(), nullptr);
}

// Each process writes its own file, so without pruning a game folder would
// collect one file per launch indefinitely. The newest few are kept, which is
// enough to compare a working run against a failing one.
constexpr size_t kKeptLogs = 10;

void PruneOldLogs(const std::wstring& directory, std::wstring_view stream) {
    struct Entry {
        std::wstring name;
        FILETIME written;
    };
    std::vector<Entry> logs;
    const std::wstring pattern = directory + L"\\" + std::wstring(stream) + L"_*.jsonl";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            logs.push_back({data.cFileName, data.ftLastWriteTime});
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (logs.size() < kKeptLogs)
        return;

    std::sort(logs.begin(), logs.end(), [](const Entry& a, const Entry& b) {
        return CompareFileTime(&a.written, &b.written) > 0;
    });
    // One slot is left for the file about to be opened.
    for (size_t i = kKeptLogs - 1; i < logs.size(); ++i)
        DeleteFileW((directory + L"\\" + logs[i].name).c_str());
}

const char* LevelName(Level level) {
    switch (level) {
    case Level::Error: return "error";
    case Level::Warning: return "warn";
    case Level::Info: return "info";
    case Level::Trace: return "trace";
    default: return "off";
    }
}

std::string Quote(std::string_view value) {
    return '"' + EscapeJson(value) + '"';
}

} // namespace

Level LevelFromSetting(int setting) {
    switch (setting) {
    case 0: return Level::Off;
    case 1: return Level::Warning; // errors and warnings
    case 2: return Level::Info;
    default: return Level::Trace;
    }
}

Field Field::Str(std::string_view key, std::string_view value) {
    return {key, Quote(value)};
}
Field Field::Str(std::string_view key, const wchar_t* value) {
    return {key, Quote(value ? text::ToUtf8(value) : std::string())};
}
// The profile directory, read once. An account name appears in a logged path
// only through this prefix: everything else is an install location.
const std::wstring& ProfileDirectory() {
    static const std::wstring profile = [] {
        wchar_t buffer[MAX_PATH];
        const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
        return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring();
    }();
    return profile;
}

Field Field::Path(std::string_view key, const wchar_t* value) {
    if (!value)
        return Str(key, std::string_view{});
    std::wstring path = value;
    const std::wstring& profile = ProfileDirectory();
    if (!profile.empty() && path.size() >= profile.size() &&
        _wcsnicmp(path.c_str(), profile.c_str(), profile.size()) == 0)
        path.replace(0, profile.size(), L"%USERPROFILE%");
    return Str(key, path.c_str());
}

Field Field::Int(std::string_view key, long long value) {
    return {key, std::to_string(value)};
}
Field Field::Uint(std::string_view key, unsigned long long value) {
    return {key, std::to_string(value)};
}
Field Field::Hex(std::string_view key, unsigned long long value) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "\"0x%llx\"", value);
    return {key, buf};
}
Field Field::Num(std::string_view key, double value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", value);
    return {key, buf};
}
Field Field::Bool(std::string_view key, bool value) {
    return {key, value ? "true" : "false"};
}

void Open(const std::wstring& directory, Level level, std::wstring_view stream) {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_level.store(level, std::memory_order_release);
    if (level == Level::Off)
        return;

    CreateDirectories(directory);
    PruneOldLogs(directory, stream);
    std::wstring path = directory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path += L'\\';
    path.append(stream);
    wchar_t suffix[32];
    std::swprintf(suffix, 32, L"_%lu.jsonl", GetCurrentProcessId());
    path += suffix;
    g_file = _wfopen(path.c_str(), L"ab");
}

void Close() {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_level.store(Level::Off, std::memory_order_release);
}

Level CurrentLevel() {
    return g_level.load(std::memory_order_acquire);
}

bool Enabled(Level level) {
    return static_cast<int>(level) <= static_cast<int>(g_level.load(std::memory_order_acquire));
}

namespace {

void Write(Level level, std::string_view event, std::initializer_list<Field> fields) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::string line = "{\"t\":";
    line += std::to_string(ms);
    line += ",\"lvl\":\"";
    line += LevelName(level);
    line += "\",\"event\":";
    line += Quote(event);
    for (const Field& field : fields) {
        line += ",\"";
        line += EscapeJson(field.key);
        line += "\":";
        line += field.token;
    }
    line += "}\n";

    std::lock_guard lock(g_mutex);
    if (g_file) {
        std::fwrite(line.data(), 1, line.size(), g_file);
        std::fflush(g_file);
    }
}

} // namespace

void Event(Level level, std::string_view event, std::initializer_list<Field> fields) {
    if (Enabled(level))
        Write(level, event, fields);
}

void Header(std::string_view event, std::initializer_list<Field> fields) {
    if (Enabled(Level::Error))
        Write(Level::Info, event, fields);
}

} // namespace odg::log
