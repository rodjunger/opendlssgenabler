#include "kernels/substitute.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/pe.h"
#include "kernels/cubin_params.h"
#include "kernels/device.h"
#include "kernels/cubin.h"
#include "kernels/fatbin.h"
#include "kernels/provider_index.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace odg::kernels {
namespace {

// A kernel set is around a hundred modules and a feature can be created more
// than once per session. Errors are capped per kind so a runtime that retries
// in a loop cannot fill the disk.
constexpr uint32_t kLoggedErrors = 64;

// Writing a file per kernel is not free. The runtime creates its whole set in
// one burst on the render thread, and a hundred synchronous writes there stalls
// it long enough to take the process down, so only the first few are written.
constexpr uint32_t kMaxDumps = 8;

// How much of an unrecognised NVAPI parameter block to log: the whole of the
// 0x50-byte block current drivers use, and some of whatever follows it.
constexpr size_t kLoggedParamsBytes = 0x60;

Options g_options;
std::atomic<bool> g_configured{false};
std::atomic<bool> g_active{false};
std::atomic<uint32_t> g_fallback_arch{0};
std::atomic<uint32_t> g_fallback_impl{0};
std::once_flag g_target_once;
// Written once under call_once by whichever thread creates the first kernel,
// and read by the worker thread for the summary, which has no ordering with it.
std::atomic<uint32_t> g_target_sm{0};

std::atomic<uint32_t> g_decisions{0};
std::atomic<uint32_t> g_refusals{0};
// Counted for the summary. The per-kernel lines are a decision each and stay at
// Info; the totals are what a report is read for, and they survive Level=1.
std::atomic<uint32_t> g_native{0};
std::atomic<uint32_t> g_retargeted{0};
std::atomic<uint32_t> g_unchanged{0};
std::atomic<uint32_t> g_last_total{0};
std::atomic<uint32_t> g_summarized{0};
std::atomic<uint32_t> g_driver_rejections{0};
std::atomic<uint32_t> g_dumps{0};

// The NVAPI parameter layout, found from the first block that carries a
// container. The runtime opens with a few probe calls whose blocks carry none,
// so a failed search is retried on the next call rather than cached.
BlobFields g_fields;
std::atomic<bool> g_fields_found{false};
std::atomic<bool> g_fields_miss_reported{false};
std::mutex g_fields_mutex;

std::string Hex(const uint8_t* bytes, size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        text += kDigits[bytes[i] >> 4];
        text += kDigits[bytes[i] & 0xF];
    }
    return text;
}

bool Fields(void* params, BlobFields& out) {
    if (!g_fields_found.load(std::memory_order_acquire)) {
        std::lock_guard lock(g_fields_mutex);
        if (!g_fields_found.load(std::memory_order_relaxed)) {
            const BlobFields found = LocateBlob(params);
            if (found.valid) {
                g_fields = found;
                g_fields_found.store(true, std::memory_order_release);
                log::Event(log::Level::Info, "cubin_params_located",
                           {log::Field::Hex("struct_size", found.struct_size),
                            log::Field::Hex("data_offset", found.data_offset),
                            log::Field::Hex("size_offset", found.size_offset),
                            log::Field::Hex("name_offset", found.name_offset),
                            log::Field::Bool("size_64bit", found.size_is_64bit)});
            } else if (!g_fields_miss_reported.exchange(true)) {
                // Recorded once, with the block's opening bytes: if the layout
                // ever changes, this is what shows where the fields moved to.
                uint8_t block[kLoggedParamsBytes] = {};
                const bool readable = pe::SafeCopy(block, params, sizeof(block));
                log::Event(log::Level::Info, "cubin_params_not_found_yet",
                           {log::Field::Str("block", readable ? Hex(block, sizeof(block))
                                                               : "unreadable")});
            }
        }
    }
    if (!g_fields_found.load(std::memory_order_acquire))
        return false;
    out = g_fields;
    return true;
}

void Dump(const void* data, size_t size, uint32_t index, const wchar_t* stage) {
    if (!g_options.dump || g_options.dump_directory.empty() || !data)
        return;
    if (g_dumps.fetch_add(1, std::memory_order_relaxed) >= kMaxDumps)
        return;
    CreateDirectoryW(g_options.dump_directory.c_str(), nullptr);
    wchar_t name[MAX_PATH];
    swprintf(name, MAX_PATH, L"%s\\kernel_%03u_%s.bin", g_options.dump_directory.c_str(), index,
             stage);
    HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
    CloseHandle(file);
}

uint32_t ResolveTarget() {
    std::call_once(g_target_once, [] {
        const char* source = "config";
        uint32_t sm = g_options.target_sm;
        if (!sm) {
            sm = DetectDeviceSm();
            source = "cuda";
        }
        if (!sm) {
            sm = SmFromNvApiArch(g_fallback_arch.load(std::memory_order_acquire),
                                 g_fallback_impl.load(std::memory_order_acquire));
            source = "nvapi";
        }
        g_target_sm.store(sm, std::memory_order_release);
        log::Event(sm ? log::Level::Info : log::Level::Error, "kernel_target_resolved",
                   {log::Field::Uint("sm", sm), log::Field::Str("source", sm ? source : "none")});
    });
    return g_target_sm.load(std::memory_order_acquire);
}

void LogDecision(Decision decision, const Request& request, const Report& report, uint32_t target,
                 size_t bytes, const char* reason) {
    switch (decision) {
    case Decision::Substituted:
        (report.native ? g_native : g_retargeted).fetch_add(1, std::memory_order_relaxed);
        log::Event(log::Level::Info, "kernel_substituted",
                   {log::Field::Str("route", request.route),
                    log::Field::Str("caller", request.caller),
                    log::Field::Str("kernel", request.kernel),
                    log::Field::Str("method", report.native ? "native" : "retarget"),
                    log::Field::Uint("from_sm", report.source_arch),
                    log::Field::Uint("to_sm", target), log::Field::Uint("bytes", bytes)});
        break;
    case Decision::Refused:
        if (g_refusals.fetch_add(1, std::memory_order_relaxed) < kLoggedErrors)
            log::Event(log::Level::Error, "kernel_refused",
                       {log::Field::Str("route", request.route),
                        log::Field::Str("caller", request.caller),
                        log::Field::Str("kernel", request.kernel),
                        log::Field::Uint("image_sm", report.source_arch),
                        log::Field::Uint("device_sm", target), log::Field::Str("reason", reason)});
        break;
    case Decision::Unchanged:
        g_unchanged.fetch_add(1, std::memory_order_relaxed);
        log::Event(log::Level::Trace, "kernel_unchanged",
                   {log::Field::Str("route", request.route),
                    log::Field::Str("caller", request.caller),
                    log::Field::Str("kernel", request.kernel), log::Field::Str("reason", reason)});
        break;
    }
}

PtxSource PreferredSource() {
    return g_options.multi_frame ? PtxSource::Newest : PtxSource::Closest;
}

// Which runtime's images may answer this call.
//
// Normally it is the module the call came from: its own build's images are the
// only ones that are this kernel, and the driver refuses another build's as an
// invalid image. The caller cannot always be identified, though. Another tool
// that hooks the same entry point calls through a trampoline that belongs to no
// module, and then the return address names nothing. While only one runtime is
// indexed there is no ambiguity to protect against, so that one answers; once
// there are several, an unattributable call is refused rather than guessed.
HMODULE AnsweringProvider(HMODULE caller) {
    if (caller)
        return caller;
    return SoleIndexedProvider();
}

// A bare cubin carries machine code only. It is either runnable as it is, has a
// native twin in the runtime it came from, came from a container whose PTX can
// be retargeted, or cannot be supplied at all.
Decision DecideCubin(HMODULE caller, const void* blob, size_t size, uint32_t cubin_arch,
                     uint32_t target, PtxSource source, std::vector<uint8_t>& out, Report& report,
                     const char*& reason) {
    report.source_arch = cubin_arch;
    if (CanRun(false, cubin_arch, target)) {
        reason = "runnable";
        return Decision::Unchanged;
    }
    const HMODULE provider = AnsweringProvider(caller);
    size_t native_size = 0;
    if (const void* native = FindNativeCubin(provider, blob, size, target, native_size)) {
        const auto* bytes = static_cast<const uint8_t*>(native);
        out.assign(bytes, bytes + native_size);
        report.native = true;
        return Decision::Substituted;
    }
    size_t container_size = 0;
    if (const void* container = FindContainerForCubin(provider, blob, size, container_size)) {
        Report retarget;
        if (Retarget(container, container_size, target, out, retarget, source)) {
            report.source_arch = retarget.source_arch;
            return Decision::Substituted;
        }
    }
    // Nothing can be supplied, and the three reasons are different problems.
    if (ProviderIndexed(provider))
        reason = "no runnable image for this architecture";
    else if (provider)
        reason = "this runtime was not indexed";
    else
        reason = "the calling module could not be identified";
    return Decision::Refused;
}

Decision DecideFrom(HMODULE caller, const void* blob, size_t size, PtxSource source,
                    uint32_t target, std::vector<uint8_t>& out, Report& report,
                    const char*& reason) {
    if (!target || !blob || !size) {
        reason = !Active() ? "inactive" : target ? "empty" : "no target architecture";
        return Decision::Unchanged;
    }
    if (const uint32_t cubin_arch = CubinArch(blob, size))
        return DecideCubin(caller, blob, size, cubin_arch, target, source, out, report, reason);
    if (ContainerSize(blob, size)) {
        if (Retarget(blob, size, target, out, report, source))
            return Decision::Substituted;
        if (report.already_runnable) {
            reason = "runnable";
            return Decision::Unchanged;
        }
        // A container with no image this GPU can run and no PTX to rewrite.
        reason = "container holds no runnable image and no PTX";
        return Decision::Refused;
    }
    if (RetargetPtxText(blob, size, target, out, report))
        return Decision::Substituted;
    reason = report.already_runnable ? "runnable" : "not a kernel image";
    return Decision::Unchanged;
}

} // namespace

void Configure(const Options& options) {
    g_options = options;
    g_configured.store(true, std::memory_order_release);
}

void Activate(uint32_t architecture, uint32_t implementation) {
    g_fallback_arch.store(architecture, std::memory_order_release);
    g_fallback_impl.store(implementation, std::memory_order_release);
    if (!g_active.exchange(true, std::memory_order_acq_rel))
        log::Event(log::Level::Info, "kernels_active",
                   {log::Field::Bool("enabled", g_options.enabled)});
}

bool Active() {
    return g_configured.load(std::memory_order_acquire) && g_active.load(std::memory_order_acquire);
}

uint32_t TargetSm() {
    return Active() ? ResolveTarget() : 0;
}

Request Request::From(const char* route, const void* return_address) {
    Request request;
    request.route = route;
    request.module = paths::ModuleForAddress(return_address);
    request.caller = paths::ModuleNameForAddress(return_address);
    return request;
}

Decision Decide(const void* blob, size_t size, const Request& request, Substitution& out) {
    Report report;
    const char* reason = "";
    const uint32_t target = TargetSm();
    const uint32_t index = g_decisions.fetch_add(1, std::memory_order_relaxed);
    Decision decision = DecideFrom(request.module, blob, size, PreferredSource(), target,
                                   out.image, report, reason);
    // With retargeting switched off the decision is still made, because an
    // image this GPU cannot run must not reach the driver either way: on
    // Vulkan it loads and then hangs the GPU. Only the replacement is withheld.
    if (decision == Decision::Substituted && !g_options.enabled) {
        decision = Decision::Refused;
        reason = "retargeting is switched off";
    }
    out.source_arch = decision == Decision::Substituted ? report.source_arch : 0;

    // Only decisions are dumped: the cap is small, and images passed through
    // unchanged, such as the upscaler's, would otherwise use it up first.
    if (decision != Decision::Unchanged) {
        Dump(blob, size, index, L"in");
        if (decision == Decision::Substituted)
            Dump(out.image.data(), out.image.size(), index, L"out");
    }
    LogDecision(decision, request, report, target,
                decision == Decision::Substituted ? out.image.size() : size, reason);
    return decision;
}

bool Fallback(const void* blob, size_t size, uint32_t status, const Request& request,
              Substitution& out) {
    const uint32_t rejected_source = out.source_arch;
    if (PreferredSource() == PtxSource::Closest || !rejected_source)
        return false;
    Report report;
    const char* reason = "";
    std::vector<uint8_t> image;
    if (DecideFrom(request.module, blob, size, PtxSource::Closest, TargetSm(), image, report,
                   reason) != Decision::Substituted ||
        report.source_arch == rejected_source)
        return false;
    out.image = std::move(image);
    out.source_arch = report.source_arch;
    log::Event(log::Level::Warning, "kernel_fallback",
               {log::Field::Str("route", request.route), log::Field::Str("kernel", request.kernel),
                log::Field::Hex("status", status), log::Field::Uint("rejected_sm", rejected_source),
                log::Field::Uint("retry_sm", report.source_arch)});
    return true;
}

void ReportSummary() {
    const uint32_t native = g_native.load(std::memory_order_relaxed);
    const uint32_t retargeted = g_retargeted.load(std::memory_order_relaxed);
    const uint32_t refused = g_refusals.load(std::memory_order_relaxed);
    const uint32_t unchanged = g_unchanged.load(std::memory_order_relaxed);
    // Every decision counts towards the settle, including the kernels left
    // alone: a run that changed nothing still has to say so.
    const uint32_t total = native + retargeted + refused + unchanged;
    // Written once the runtime has stopped creating kernels: one scan to see the
    // total settle, and then only if this total has not been written already.
    if (total == 0 || g_last_total.exchange(total, std::memory_order_relaxed) != total)
        return;
    if (g_summarized.exchange(total, std::memory_order_relaxed) == total)
        return;
    log::Header("kernels_summary",
                {log::Field::Uint("native", native), log::Field::Uint("retargeted", retargeted),
                 log::Field::Uint("refused", refused),
                 log::Field::Uint("unchanged", unchanged),
                 log::Field::Uint("driver_rejected",
                                  g_driver_rejections.load(std::memory_order_relaxed)),
                 log::Field::Uint("device_sm", g_target_sm.load(std::memory_order_acquire))});
}

void ReportDriverResult(uint32_t status, const char* route) {
    if (status == 0)
        return;
    if (g_driver_rejections.fetch_add(1, std::memory_order_relaxed) < kLoggedErrors)
        log::Event(log::Level::Error, "kernel_driver_rejected",
                   {log::Field::Str("route", route), log::Field::Hex("status", status),
                    log::Field::Uint("device_sm", g_target_sm.load(std::memory_order_acquire))});
}

std::optional<CubinCall> ReadCubinCall(void* params) {
    CubinCall call;
    if (!params || !Fields(params, call.fields))
        return std::nullopt;
    call.data = ReadBlob(params, call.fields, call.size);
    if (!call.data || !call.size)
        return std::nullopt;
    call.kernel = ReadName(params, call.fields);
    return call;
}

} // namespace odg::kernels
