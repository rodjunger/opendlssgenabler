#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

// Structured JSONL logging. Each Event writes one self-contained JSON object per
// line to <directory>/<stream>_<pid>.jsonl, flushed immediately so a crash keeps
// the record.
//
// Choosing a level:
//   Error    something the engine set out to do failed, and frame generation is
//            worse or absent because of it.
//   Warning  something is not as expected but the engine carried on, including
//            a deliberate refusal. A report worth reading names these.
//   Info     a decision, a capability or a state change, once each.
//   Trace    the running commentary: per call, per kernel, per frame.
//
// The INI exposes four settings rather than five, since a reporter wants
// warnings whenever they want errors: 0 off, 1 problems, 2 decisions, 3
// everything. LevelFromSetting maps one to the other.
namespace odg::log {

enum class Level : int { Off = 0, Error = 1, Warning = 2, Info = 3, Trace = 4 };

// The INI's Logging.Level, 0 to 3, as the lowest level that is written.
Level LevelFromSetting(int setting);

struct Field {
    std::string_view key;
    std::string token; // already-encoded JSON value

    static Field Str(std::string_view key, std::string_view value);
    static Field Str(std::string_view key, const wchar_t* value);
    // A filesystem path, with the user's profile directory replaced by
    // %USERPROFILE%. Logs are meant to be attached to public bug reports, and a
    // path under the profile carries the account name. Use this for every path.
    static Field Path(std::string_view key, const wchar_t* value);
    static Field Int(std::string_view key, long long value);
    static Field Uint(std::string_view key, unsigned long long value);
    static Field Hex(std::string_view key, unsigned long long value);
    static Field Num(std::string_view key, double value);
    static Field Bool(std::string_view key, bool value);
};

void Open(const std::wstring& directory, Level level, std::wstring_view stream);
void Close();
Level CurrentLevel();
bool Enabled(Level level);

void Event(Level level, std::string_view event, std::initializer_list<Field> fields = {});

// Written at every level except Off, labelled info: the facts that identify a
// run, which are needed to read even an errors-only log. The process and build,
// the settings in force, the GPU, the driver, the runtime and what was done with
// its kernels. One line each, once per process.
void Header(std::string_view event, std::initializer_list<Field> fields = {});

} // namespace odg::log
