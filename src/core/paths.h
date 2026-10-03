#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace odg::paths {

std::wstring ModulePath(HMODULE module);
std::wstring ModuleFileName(HMODULE module);

// Module a code address belongs to, without changing its reference count.
// Null when the address is in no loaded module.
HMODULE ModuleForAddress(const void* address);

// Keeps `module` mapped for the life of the process. NGX loads a runtime,
// reads what it needs and unloads it again, so a handle this engine keeps, or a
// pointer into the image behind it, is otherwise only valid until it does.
// False when the pin was refused, which leaves both of those unsafe to hold.
// It takes the loader lock, so it is never called with a lock of this engine's
// held.
bool PinModule(HMODULE module);

// File name of the module a code address belongs to, in UTF-8, for logging and
// for deciding which component made a call. Empty when the address is in no
// loaded module.
std::string ModuleNameForAddress(const void* address);

std::wstring ParentDirectory(const std::wstring& path);

bool FileNameEqualsInsensitive(const std::wstring& path, const wchar_t* name);

// Loads `name` from the Windows system directory, never from the game's folder.
// This engine is itself loaded under the name of a system DLL, so a plain load
// by name could find it instead of the real one.
HMODULE LoadSystemLibrary(const wchar_t* name);

// The file name that says which NVIDIA component a module is.
//
// NGX can replace a Streamline plugin or a feature runtime a game ships with a
// copy it downloaded, which Streamline then loads in preference to the game's.
// Those copies live in <ProgramData>\NVIDIA\NGX\models\<component>\versions\
// <build>\files and are named <architecture>_<application id>.dll, so their own
// file name identifies nothing. For them this returns the name the same
// component ships under; for every other module, its own file name.
std::wstring ComponentFileName(const std::wstring& path);

// Every loaded module that is the named NVIDIA component. A game's own copy and
// a downloaded one can both be mapped, so this returns all of them rather than
// the first, in the order Windows reports.
std::vector<HMODULE> LoadedComponents(const wchar_t* module_name);

} // namespace odg::paths
