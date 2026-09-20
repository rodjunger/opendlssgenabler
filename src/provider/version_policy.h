#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// Identifies the NGX DLSS-G runtime (nvngx_dlssg.dll) a game ships. Nothing is
// gated on its version: the kernel handling reads the runtime's own images and
// works the same on a version this project has never seen. The version is
// recorded so a report says which runtime was in play, and marked as tested
// when it is one that has been verified in a game.
namespace odg::provider {

struct Version {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t build = 0;
    uint16_t revision = 0;
    bool valid = false;
};

bool ReadVersion(const wchar_t* path, Version& out);
std::string ToString(const Version& version);

// Verified in a game. See docs/TESTING.md for which games and results.
bool IsTested(const Version& version);

// A frame-generation runtime, as opposed to an upscaling one that happens to be
// loaded under the same name, identified by its exports.
bool IsDlssgProvider(HMODULE module);

} // namespace odg::provider
