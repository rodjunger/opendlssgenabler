#pragma once

#include <windows.h>

// Driven by DllMain. Initialize runs once, on a thread of its own, and wires up
// configuration, logging and hooks. There is no matching shutdown: the proxy is
// pinned, so it is only unloaded when the process exits.
namespace odg::app {

void Initialize(HMODULE self);

} // namespace odg::app
