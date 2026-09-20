#pragma once

#include <cstddef>

// Signature-agnostic export forwarding. Generated translation units under
// generated/ define the thunks and one Bind_<name> entry point each; the shared
// binder resolves the real system DLL and fills the thunk pointer slots.
namespace odg::proxy {

struct Export {
    const char* name;
    void** slot;
};

// Loads real_dll from the Windows system directory and resolves each export
// into its slot. Returns true when every export was resolved.
bool Bind(const wchar_t* real_dll, const Export* exports, size_t count);

// Binding runs before logging is open, so its outcome is recorded here and
// reported once the log is available.
struct BindResult {
    const wchar_t* dll = nullptr;
    size_t resolved = 0;
    size_t total = 0;
    bool attempted = false;
};
const BindResult& LastBind();

// Defined by the generated proxy translation unit linked into this target.
// Binds when loaded_file_name matches the DLL this proxy stands in for, and is
// called from DllMain before any forwarded export can run.
bool BindActive(const wchar_t* loaded_file_name);

} // namespace odg::proxy
