#pragma once

#include <windows.h>

// Lifecycle entry points driven by DllMain. Initialize runs once, off the
// loader-lock path where it can, and wires up configuration, logging and hooks.
namespace odg::app {

void Initialize(HMODULE self);
void Shutdown();

} // namespace odg::app
