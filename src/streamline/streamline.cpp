#include "streamline/streamline.h"

#include "core/hooks.h"
#include "core/log.h"

#include <windows.h>

#include <sl.h>
#include <sl_dlss_g.h>

#include <atomic>
#include <cstring>
#include <string>

namespace odg::streamline {
namespace {

std::atomic<uint32_t> g_force_multiplier{0};
std::atomic<bool> g_installed{false};
std::atomic<PFun_slGetFeatureFunction*> g_get_feature_function{nullptr};
std::atomic<PFun_slDLSSGSetOptions*> g_set_options{nullptr};
std::atomic<bool> g_frame_generation_on{false};
std::atomic<PFun_slDLSSGGetState*> g_get_state{nullptr};
std::atomic<uint64_t> g_set_calls{0};
std::atomic<uint64_t> g_last_request{UINT64_MAX};
std::atomic<bool> g_newer_options_reported{false};

const char* ModeName(sl::DLSSGMode mode) {
    switch (mode) {
    case sl::DLSSGMode::eOff: return "off";
    case sl::DLSSGMode::eOn: return "on";
    case sl::DLSSGMode::eAuto: return "auto";
    case sl::DLSSGMode::eDynamic: return "dynamic";
    default: return "?";
    }
}

// A raw sl::Result number is hard to read in a report, so the common ones are
// named. The number is always logged beside the name, for the ones that are not.
const char* ResultName(sl::Result result) {
    switch (result) {
    case sl::Result::eOk: return "eOk";
    case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
    case sl::Result::eErrorAdapterNotSupported: return "eErrorAdapterNotSupported";
    case sl::Result::eErrorOSDisabledHWS: return "eErrorOSDisabledHWS";
    case sl::Result::eErrorNVAPI: return "eErrorNVAPI";
    case sl::Result::eErrorNGXFailed: return "eErrorNGXFailed";
    case sl::Result::eErrorReflexAPI: return "eErrorReflexAPI";
    case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported";
    case sl::Result::eErrorMissingInputParameter: return "eErrorMissingInputParameter";
    case sl::Result::eErrorInvalidParameter: return "eErrorInvalidParameter";
    case sl::Result::eErrorInvalidState: return "eErrorInvalidState";
    case sl::Result::eWarnOutOfVRAM: return "eWarnOutOfVRAM";
    default: return "other";
    }
}

std::string DecodeStatus(sl::DLSSGStatus status) {
    const uint32_t bits = static_cast<uint32_t>(status);
    if (bits == 0)
        return "ok";
    std::string out;
    const auto add = [&](const char* name) {
        if (!out.empty())
            out += '|';
        out += name;
    };
    if (bits & static_cast<uint32_t>(sl::DLSSGStatus::eFailResolutionTooLow))
        add("ResolutionTooLow");
    if (bits & static_cast<uint32_t>(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime))
        add("ReflexNotDetected");
    if (bits & static_cast<uint32_t>(sl::DLSSGStatus::eFailHDRFormatNotSupported))
        add("HDRFormatNotSupported");
    if (bits & static_cast<uint32_t>(sl::DLSSGStatus::eFailCommonConstantsInvalid))
        add("CommonConstantsInvalid");
    return out.empty() ? "unknown" : out;
}

// Reads only the version-1 fields, so an older runtime cannot reject the query
// over a struct version it does not know.
void LogState(const sl::ViewportHandle& viewport, const sl::DLSSGOptions* options,
              const char* when) {
    PFun_slDLSSGGetState* get_state = g_get_state.load(std::memory_order_acquire);
    if (!get_state) {
        log::Event(log::Level::Info, "dlssg_state_unavailable", {log::Field::Str("when", when)});
        return;
    }
    sl::DLSSGState state{};
    state.structVersion = sl::kStructVersion1;
    const sl::Result result = get_state(viewport, state, options);
    if (result != sl::Result::eOk) {
        log::Event(log::Level::Info, "dlssg_state_failed",
                   {log::Field::Str("when", when), log::Field::Str("result", ResultName(result)),
                    log::Field::Uint("code", static_cast<uint32_t>(result))});
        return;
    }
    log::Event(log::Level::Info, "dlssg_state",
               {log::Field::Str("when", when),
                log::Field::Str("status", DecodeStatus(state.status)),
                log::Field::Uint("presented", state.numFramesActuallyPresented),
                log::Field::Uint("min_wh", state.minWidthOrHeight)});
}

// Copies only the fields the game's own struct version covers, since reading
// past them would read past the game's allocation. The extension chain is kept,
// so structures the game linked to its options still reach Streamline.
sl::DLSSGOptions CopyKnownOptions(const sl::DLSSGOptions& source) {
    sl::DLSSGOptions copy{};
    copy.structVersion = source.structVersion;
    copy.next = source.next;
    copy.mode = source.mode;
    copy.numFramesToGenerate = source.numFramesToGenerate;
    copy.flags = source.flags;
    copy.dynamicResWidth = source.dynamicResWidth;
    copy.dynamicResHeight = source.dynamicResHeight;
    copy.numBackBuffers = source.numBackBuffers;
    copy.mvecDepthWidth = source.mvecDepthWidth;
    copy.mvecDepthHeight = source.mvecDepthHeight;
    copy.colorWidth = source.colorWidth;
    copy.colorHeight = source.colorHeight;
    copy.colorBufferFormat = source.colorBufferFormat;
    copy.mvecBufferFormat = source.mvecBufferFormat;
    copy.depthBufferFormat = source.depthBufferFormat;
    copy.hudLessBufferFormat = source.hudLessBufferFormat;
    copy.uiBufferFormat = source.uiBufferFormat;
    copy.onErrorCallback = source.onErrorCallback;
    if (source.structVersion >= sl::kStructVersion2)
        copy.bReserved15 = source.bReserved15;
    if (source.structVersion >= sl::kStructVersion3)
        copy.queueParallelismMode = source.queueParallelismMode;
    if (source.structVersion >= sl::kStructVersion4)
        copy.enableUserInterfaceRecomposition = source.enableUserInterfaceRecomposition;
    if (source.structVersion >= sl::kStructVersion5)
        copy.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
    return copy;
}

// Games call slDLSSGSetOptions every frame. A call is reported when the request
// changes, such as a switch from 2x to 4x, and otherwise at exponentially
// spaced intervals so a long session still shows it is being called.
bool ShouldReport(const sl::DLSSGOptions& options) {
    const uint64_t request = (uint64_t{static_cast<uint32_t>(options.mode)} << 32) |
                             options.numFramesToGenerate;
    const bool changed = g_last_request.exchange(request, std::memory_order_relaxed) != request;
    const uint64_t call = g_set_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    return changed || (call & (call - 1)) == 0;
}

sl::Result SetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
    PFun_slDLSSGSetOptions* original = g_set_options.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    // This engine changes how many frames are generated, never whether they are.
    if (options.mode == sl::DLSSGMode::eOff)
        return original(viewport, options);

    const uint32_t force = g_force_multiplier.load(std::memory_order_acquire);
    const bool report = ShouldReport(options);

    if (force == 0 || options.mode == sl::DLSSGMode::eDynamic) {
        const sl::Result result = original(viewport, options);
        if (report) {
            log::Event(log::Level::Info, "dlssg_set_options",
                       {log::Field::Str("mode", ModeName(options.mode)),
                        log::Field::Uint("num_frames", options.numFramesToGenerate),
                        log::Field::Hex("flags", static_cast<uint32_t>(options.flags)),
                        log::Field::Uint("back_buffers", options.numBackBuffers),
                        log::Field::Str("result", ResultName(result)),
                        log::Field::Str("forced", "no")});
            if (result == sl::Result::eOk)
                LogState(viewport, &options, "after_set");
        }
        return result;
    }

    // The copy below is this build's struct. A game built against a newer SDK
    // passes fields it does not have, and Streamline would read them past the
    // end of the copy, so such a request is passed through as the game made it.
    static const uint32_t kKnownVersion = sl::DLSSGOptions{}.structVersion;
    if (options.structVersion > kKnownVersion) {
        if (!g_newer_options_reported.exchange(true))
            log::Event(log::Level::Warning, "dlssg_force_skipped",
                       {log::Field::Uint("struct_version", options.structVersion),
                        log::Field::Uint("known_version", kKnownVersion),
                        log::Field::Str("note", "the game's Streamline is newer than this build; "
                                                "its own multiplier is used")});
        return original(viewport, options);
    }

    sl::DLSSGOptions adjusted = CopyKnownOptions(options);
    adjusted.mode = sl::DLSSGMode::eOn;
    adjusted.numFramesToGenerate = force - 1;
    sl::Result result = original(viewport, adjusted);
    if (result != sl::Result::eOk) {
        // The forced count was refused; replay the game's own request so it
        // keeps the frame generation it asked for.
        const sl::Result fallback = original(viewport, options);
        if (report)
            log::Event(log::Level::Warning, "dlssg_force_rejected",
                       {log::Field::Uint("requested", adjusted.numFramesToGenerate),
                        log::Field::Str("result", ResultName(result)),
                        log::Field::Uint("code", static_cast<uint32_t>(result)),
                        log::Field::Str("fallback", ResultName(fallback))});
        return fallback;
    }
    if (report) {
        log::Event(log::Level::Info, "dlssg_set_options",
                   {log::Field::Str("mode", ModeName(options.mode)),
                    log::Field::Uint("num_frames", adjusted.numFramesToGenerate),
                    log::Field::Str("result", ResultName(result)),
                    log::Field::Str("forced", "yes")});
        LogState(viewport, &adjusted, "after_set");
    }
    return result;
}

sl::Result HookSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
    const sl::Result result = SetOptions(viewport, options);
    if (result == sl::Result::eOk)
        g_frame_generation_on.store(options.mode != sl::DLSSGMode::eOff, std::memory_order_release);
    return result;
}

sl::Result HookGetFeatureFunction(sl::Feature feature, const char* function_name, void*& function) {
    PFun_slGetFeatureFunction* original = g_get_feature_function.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    const sl::Result result = original(feature, function_name, function);
    if (result != sl::Result::eOk || feature != sl::kFeatureDLSS_G || !function_name || !function)
        return result;

    if (std::strcmp(function_name, "slDLSSGGetState") == 0) {
        g_get_state.store(reinterpret_cast<PFun_slDLSSGGetState*>(function),
                          std::memory_order_release);
        return result;
    }
    if (std::strcmp(function_name, "slDLSSGSetOptions") == 0) {
        if (function == reinterpret_cast<void*>(&HookSetOptions))
            return result;
        g_set_options.store(reinterpret_cast<PFun_slDLSSGSetOptions*>(function),
                            std::memory_order_release);
        function = reinterpret_cast<void*>(&HookSetOptions);
        log::Event(log::Level::Info, "dlssg_set_options_intercepted", {});
    }
    return result;
}

// The interposer answers these before a game will offer frame generation. When
// DLSS-G never starts, the refusal is visible here rather than anywhere in our
// own code, so both the question and the answer are recorded.
std::atomic<PFun_slIsFeatureSupported*> g_is_supported{nullptr};
std::atomic<PFun_slIsFeatureLoaded*> g_is_loaded{nullptr};
std::atomic<PFun_slSetFeatureLoaded*> g_set_loaded{nullptr};

const char* FeatureName(sl::Feature feature) {
    switch (feature) {
    case sl::kFeatureDLSS: return "dlss";
    case sl::kFeatureDLSS_G: return "dlss_g";
    case sl::kFeatureDLSS_RR: return "dlss_rr";
    case sl::kFeatureReflex: return "reflex";
    case sl::kFeaturePCL: return "pcl";
    case sl::kFeatureCommon: return "common";
    default: return "other";
    }
}

// A game may ask on every frame, so each feature's answer is logged when it
// changes rather than every time it is given.
constexpr size_t kTrackedFeatures = 8;
// 64-bit so the empty marker cannot collide with a feature id: sl::kFeatureCommon
// is UINT32_MAX.
constexpr uint64_t kEmptySlot = UINT64_MAX;
struct LastAnswer {
    std::atomic<uint64_t> feature{kEmptySlot};
    std::atomic<uint32_t> result{UINT32_MAX};
};
// One table per question asked, so a feature's support and its loaded state do
// not overwrite each other's record.
LastAnswer g_last_supported[kTrackedFeatures];
LastAnswer g_last_loaded[kTrackedFeatures];

bool AnswerChanged(LastAnswer (&answers)[kTrackedFeatures], sl::Feature feature, uint32_t state) {
    for (LastAnswer& slot : answers) {
        uint64_t owner = slot.feature.load(std::memory_order_acquire);
        if (owner == kEmptySlot &&
            slot.feature.compare_exchange_strong(owner, feature, std::memory_order_acq_rel))
            owner = feature;
        if (owner == feature)
            return slot.result.exchange(state, std::memory_order_acq_rel) != state;
    }
    return false; // more features than tracked; stay quiet rather than flood
}

sl::Result HookIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapter) {
    PFun_slIsFeatureSupported* original = g_is_supported.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    const sl::Result result = original(feature, adapter);
    if (AnswerChanged(g_last_supported, feature, static_cast<uint32_t>(result)))
        log::Event(result == sl::Result::eOk || feature != sl::kFeatureDLSS_G ? log::Level::Info
                                                                               : log::Level::Error,
                   "sl_is_feature_supported",
                   {log::Field::Str("feature", FeatureName(feature)),
                    log::Field::Str("result", ResultName(result)),
                    log::Field::Uint("code", static_cast<uint32_t>(result))});
    return result;
}

sl::Result HookIsFeatureLoaded(sl::Feature feature, bool& loaded) {
    PFun_slIsFeatureLoaded* original = g_is_loaded.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    const sl::Result result = original(feature, loaded);
    // Asked on every frame, for every feature the game uses: Indiana Jones and
    // the Great Circle asked 223,132 times in seven minutes. Only a change is
    // worth a line.
    const uint32_t state = (static_cast<uint32_t>(result) << 1) | (loaded ? 1u : 0u);
    if (AnswerChanged(g_last_loaded, feature, state))
        log::Event(log::Level::Trace, "sl_is_feature_loaded",
                   {log::Field::Str("feature", FeatureName(feature)),
                    log::Field::Bool("loaded", loaded), log::Field::Str("result", ResultName(result))});
    return result;
}

sl::Result HookSetFeatureLoaded(sl::Feature feature, bool loaded) {
    PFun_slSetFeatureLoaded* original = g_set_loaded.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;
    const sl::Result result = original(feature, loaded);
    log::Event(log::Level::Info, "sl_set_feature_loaded",
               {log::Field::Str("feature", FeatureName(feature)), log::Field::Bool("loaded", loaded),
                log::Field::Str("result", ResultName(result)),
                log::Field::Uint("code", static_cast<uint32_t>(result))});
    return result;
}

// Streamline reports, in its own words, why it accepts or refuses a feature. A
// production interposer ignores its JSON override, so the only way to read that
// reasoning is to raise the level on the host's own preferences and take the
// messages through a callback. The caller's structure is restored immediately,
// and only the two fields that version 1 of the structure already defines are
// touched, so a newer structure passes through unharmed.
std::atomic<PFun_slInit*> g_init{nullptr};
std::atomic<int> g_diagnostics{0};

void OnStreamlineLog(sl::LogType type, const char* message) {
    if (!message)
        return;
    std::string text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    if (text.empty())
        return;
    const log::Level level =
        type == sl::LogType::eError ? log::Level::Error : log::Level::Trace;
    log::Event(level, "sl_log", {log::Field::Str("text", text)});
}

struct PreferencesOverride {
    sl::Preferences* preferences = nullptr;
    sl::LogLevel level{};
    sl::PFun_LogMessageCallback* callback = nullptr;
    DWORD protection = 0;
    bool applied = false;

    explicit PreferencesOverride(const sl::Preferences& source) {
        auto* target = const_cast<sl::Preferences*>(&source);
        if (!VirtualProtect(target, sizeof(sl::Preferences), PAGE_READWRITE, &protection))
            return;
        preferences = target;
        level = target->logLevel;
        callback = target->logMessageCallback;
        target->logLevel = sl::LogLevel::eVerbose;
        target->logMessageCallback = &OnStreamlineLog;
        applied = true;
    }

    ~PreferencesOverride() {
        if (!applied)
            return;
        preferences->logLevel = level;
        preferences->logMessageCallback = callback;
        DWORD ignored = 0;
        VirtualProtect(preferences, sizeof(sl::Preferences), protection, &ignored);
    }
};

sl::Result HookInit(const sl::Preferences& preferences, uint64_t sdk_version) {
    PFun_slInit* original = g_init.load(std::memory_order_acquire);
    if (!original)
        return sl::Result::eErrorNotInitialized;

    std::string features;
    for (uint32_t i = 0; i < preferences.numFeaturesToLoad && preferences.featuresToLoad; ++i) {
        if (!features.empty())
            features += ',';
        features += FeatureName(preferences.featuresToLoad[i]);
    }
    log::Event(log::Level::Info, "sl_init",
               {log::Field::Str("features", features),
                log::Field::Uint("sdk_version", sdk_version),
                log::Field::Uint("struct_version", preferences.structVersion)});

    if (!g_diagnostics.load(std::memory_order_acquire))
        return original(preferences, sdk_version);
    PreferencesOverride override(preferences);
    const sl::Result result = original(preferences, sdk_version);
    log::Event(result == sl::Result::eOk ? log::Level::Info : log::Level::Error, "sl_init_result",
               {log::Field::Str("result", ResultName(result)),
                log::Field::Uint("code", static_cast<uint32_t>(result)),
                log::Field::Bool("diagnostics", override.applied)});
    return result;
}

} // namespace

bool FrameGenerationOn() {
    return g_frame_generation_on.load(std::memory_order_acquire);
}

void SetForceMultiplier(uint32_t multiplier) {
    g_force_multiplier.store(multiplier, std::memory_order_release);
}

void SetDiagnostics(bool enabled) {
    g_diagnostics.store(enabled ? 1 : 0, std::memory_order_release);
}

bool InstallInterposerHooks(HMODULE interposer) {
    if (!interposer)
        return false;
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true))
        return true;

    // Each hook stands alone: a missing export in some Streamline version only
    // loses the diagnostics that hook provides.
    const bool options = hooks::InstallExport(interposer, "slGetFeatureFunction",
                                              reinterpret_cast<void*>(&HookGetFeatureFunction),
                                              g_get_feature_function);
    hooks::InstallExport(interposer, "slInit", reinterpret_cast<void*>(&HookInit), g_init);
    hooks::InstallExport(interposer, "slIsFeatureSupported",
                         reinterpret_cast<void*>(&HookIsFeatureSupported), g_is_supported);
    hooks::InstallExport(interposer, "slIsFeatureLoaded",
                         reinterpret_cast<void*>(&HookIsFeatureLoaded), g_is_loaded);
    hooks::InstallExport(interposer, "slSetFeatureLoaded",
                         reinterpret_cast<void*>(&HookSetFeatureLoaded), g_set_loaded);
    return options;
}

} // namespace odg::streamline
