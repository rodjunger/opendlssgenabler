#include "streamline/plugin_patch.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/pe.h"
#include "core/signature.h"
#include "core/x86.h"

#include <libhat/process.hpp>
#include <libhat/scanner.hpp>

#include <atomic>
#include <climits>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace odg::streamline {
namespace {

// Signatures use IDA syntax: two hex digits match a byte, `?` matches any byte.
// An eight-character token is a bit mask, so a ModRM byte can be matched by its
// fields rather than by listing every register it could name.

// lea r64, [rip + rel32]
//   01001?0?  REX prefix with W set; R and B free, so any 64-bit register
//   8D        lea
//   00???101  ModRM with mod 00 and rm 101: an address relative to the next
//             instruction; reg free, so any destination
//   ? ? ? ?   rel32
constexpr auto kLeaRipRelative = hat::compile_signature<"01001?0? 8D 00???101 ? ? ? ?">();
constexpr size_t kLeaRel32Offset = 3;

// mov edx, <limit> ; cmp ecx, edx ; cmovb edx, ecx
constexpr auto kFrameCountClamp = hat::compile_signature<"BA ? ? ? ? 3B CA 0F 42 D1">();
constexpr size_t kClampLimitOffset = 1; // the mov's imm32
constexpr size_t kClampCmovOffset = 7;  // the cmovb, 3 bytes

// A single instruction that does nothing, the same length as the cmovb it
// replaces: `nop dword ptr [rax]`, the multi-byte NOP Intel recommends.
constexpr std::byte kThreeByteNop[] = {std::byte{0x0F}, std::byte{0x1F}, std::byte{0x00}};

// Logged once by the plugin, from the code that picks flip metering.
constexpr std::string_view kFlipMeteringMarker = "FG1 DLL has been detected";

// How far past the marker's reference to look for the fallback's store. The
// store follows the log call within a few instructions in every build seen.
constexpr int kInstructionsAfterMarker = 64;

// A frame count the plugin could plausibly have been compiled to allow.
constexpr uint32_t kMaxPlausibleFrameLimit = 8;

// Which patches a mapped plugin has already received. Streamline can map both
// the copy a game ships and a newer one NGX downloaded, so each image is
// recorded separately rather than the process holding one flag per patch.
struct PatchedPlugin {
    HMODULE module = nullptr;
    bool flip_metering = false;
    bool frame_clamp = false;
};
std::vector<PatchedPlugin> g_patched;
SRWLOCK g_patched_lock = SRWLOCK_INIT;
std::atomic<bool> g_flip_enabled{true};
std::atomic<bool> g_clamp_enabled{false};
std::atomic<int> g_flip_value{-1};

// The record for `plugin`, or null. Called with g_patched_lock held.
PatchedPlugin* Find(HMODULE plugin) {
    for (PatchedPlugin& candidate : g_patched) {
        if (candidate.module == plugin)
            return &candidate;
    }
    return nullptr;
}

// `mov byte ptr [reg + displacement], value`, as a signature:
//   C6        mov r/m8, imm8
//   10000???  ModRM with mod 10 (32-bit displacement) and reg 000 (mov);
//             rm free, so any base register
//   then the displacement and the value
hat::signature ByteStoreSignature(int32_t displacement, uint8_t value) {
    hat::signature store = signature::Parse("C6 10000???");
    signature::Append(store, displacement);
    signature::Append(store, value);
    return store;
}

std::vector<const std::byte*> FindReferences(std::span<const std::byte> code, const std::byte* target) {
    std::vector<const std::byte*> references;
    for (const auto& lea : hat::find_all_pattern(code, kLeaRipRelative)) {
        if (lea.rel(kLeaRel32Offset) == target)
            references.push_back(lea.get());
    }
    return references;
}

// The first store of a boolean to a field deep in an object, after the marker's
// reference, is the fallback setting its metering flag to off. The flag lives
// in the plugin's large context object, past what an 8-bit displacement
// reaches, which also tells it apart from stores to small local structures.
std::optional<x86::ByteStore> FallbackStore(const std::byte* reference) {
    const std::byte* cursor = reference;
    for (int i = 0; i < kInstructionsAfterMarker; ++i) {
        const auto instruction = x86::Decode(cursor);
        if (!instruction)
            return std::nullopt;
        const auto store = x86::AsByteStore(*instruction);
        if (store && store->value <= 1 && store->displacement > INT8_MAX)
            return store;
        cursor = instruction->Next();
    }
    return std::nullopt;
}

void AnalyzeFlipMetering(const hat::process::module& plugin, PluginAnalysis& analysis) {
    const std::span<const std::byte> code = plugin.get_executable_data();
    const auto marker =
        hat::find_pattern(std::span<const std::byte>(plugin.get_module_data()), signature::Literal(kFlipMeteringMarker));
    if (!marker.has_result()) {
        analysis.flip_metering_problem = "marker string not found";
        return;
    }
    const auto references = FindReferences(code, marker.get());
    if (references.empty()) {
        analysis.flip_metering_problem = "marker string is never referenced";
        return;
    }

    std::optional<x86::ByteStore> fallback;
    for (const std::byte* reference : references) {
        if ((fallback = FallbackStore(reference)))
            break;
    }
    if (!fallback) {
        analysis.flip_metering_problem = "no store follows the marker";
        return;
    }

    PluginAnalysis::FlipMetering flip;
    flip.flag_offset = fallback->displacement;
    flip.off_value = fallback->value;
    const int forced = g_flip_value.load(std::memory_order_acquire);
    if (forced >= 0)
        flip.off_value = static_cast<uint8_t>(forced);

    const uint8_t on_value = flip.off_value ? 0 : 1;
    const auto signature = ByteStoreSignature(flip.flag_offset, on_value);
    for (const auto& match : hat::find_all_pattern(code, signature)) {
        // The signature can match inside a longer instruction; decoding at the
        // match confirms it is the store it looks like.
        const auto instruction = x86::Decode(match.get());
        const auto store = instruction ? x86::AsByteStore(*instruction) : std::nullopt;
        if (store && store->displacement == flip.flag_offset && store->value == on_value)
            flip.opposite_stores.push_back(match.get() + instruction->length - 1);
    }
    analysis.flip_metering = std::move(flip);
}

void AnalyzeFrameClamp(const hat::process::module& plugin, PluginAnalysis& analysis) {
    const auto matches = hat::find_all_pattern(
        std::span<const std::byte>(plugin.get_executable_data()), kFrameCountClamp);
    analysis.frame_clamp_matches = matches.size();
    if (matches.size() != 1)
        return;
    uint32_t limit = 0;
    std::memcpy(&limit, matches.front().get() + kClampLimitOffset, sizeof(limit));
    if (limit == 0 || limit > kMaxPlausibleFrameLimit)
        return;
    analysis.frame_clamp = PluginAnalysis::FrameClamp{limit, matches.front().get() + kClampCmovOffset};
}

void PatchFlipMetering(const PluginAnalysis& analysis, const wchar_t* plugin) {
    if (!analysis.flip_metering) {
        log::Event(log::Level::Warning, "flip_metering_not_patched",
                   {log::Field::Path("plugin", plugin),
                    log::Field::Str("reason", analysis.flip_metering_problem),
                    log::Field::Str("note", "generated frames may not be shown")});
        return;
    }
    const auto& flip = *analysis.flip_metering;
    size_t patched = 0;
    for (const std::byte* immediate : flip.opposite_stores)
        patched += pe::PatchCode(immediate, &flip.off_value, sizeof(flip.off_value)) ? 1 : 0;
    log::Event(patched ? log::Level::Info : log::Level::Warning, "flip_metering_forced",
               {log::Field::Path("plugin", plugin),
                log::Field::Hex("flag_offset", static_cast<uint32_t>(flip.flag_offset)),
                log::Field::Uint("off_value", flip.off_value),
                log::Field::Bool("overridden", g_flip_value.load(std::memory_order_acquire) >= 0),
                log::Field::Uint("stores_patched", patched)});
}

void PatchFrameClamp(const PluginAnalysis& analysis, const wchar_t* plugin) {
    if (!analysis.frame_clamp) {
        log::Event(log::Level::Info, "frame_clamp_not_found",
                   {log::Field::Path("plugin", plugin),
                    log::Field::Uint("matches", analysis.frame_clamp_matches)});
        return;
    }
    const bool ok =
        pe::PatchCode(analysis.frame_clamp->cmov, kThreeByteNop, sizeof(kThreeByteNop));
    log::Event(ok ? log::Level::Info : log::Level::Warning, "frame_clamp_removed",
               {log::Field::Path("plugin", plugin),
                log::Field::Uint("compiled_limit", analysis.frame_clamp->limit)});
}

} // namespace

PluginAnalysis AnalyzePlugin(HMODULE plugin) {
    PluginAnalysis analysis;
    const auto module = hat::process::module_at(plugin);
    if (!module) {
        analysis.flip_metering_problem = "not a loaded module";
        return analysis;
    }
    AnalyzeFlipMetering(*module, analysis);
    AnalyzeFrameClamp(*module, analysis);
    return analysis;
}

void SetFlipMeteringValue(int value) {
    g_flip_value.store(value, std::memory_order_release);
}

void SetPatchesEnabled(bool flip_metering, bool frame_clamp) {
    g_flip_enabled.store(flip_metering, std::memory_order_release);
    g_clamp_enabled.store(frame_clamp, std::memory_order_release);
}

void PatchPlugin(HMODULE plugin) {
    if (!plugin)
        return;

    AcquireSRWLockExclusive(&g_patched_lock);
    const PatchedPlugin* record = Find(plugin);
    const bool flip = g_flip_enabled.load(std::memory_order_acquire) &&
                      !(record && record->flip_metering);
    const bool clamp = g_clamp_enabled.load(std::memory_order_acquire) &&
                       !(record && record->frame_clamp);
    ReleaseSRWLockExclusive(&g_patched_lock);
    if (!flip && !clamp)
        return;

    // Streamline can unload a plugin it did not choose, and the image is read
    // and patched directly below. Pinned first, outside the lock because
    // pinning takes the loader lock, so the image cannot go away underneath
    // the scan, and so a handle recorded here can never name a different
    // module later.
    if (!paths::PinModule(plugin)) {
        log::Event(log::Level::Warning, "plugin_pin_failed",
                   {log::Field::Str("note", "plugin not patched; it may already be unloaded")});
        return;
    }

    AcquireSRWLockExclusive(&g_patched_lock);
    PatchedPlugin* done = Find(plugin);
    if (!done)
        done = &g_patched.emplace_back(PatchedPlugin{plugin, false, false});
    done->flip_metering = done->flip_metering || flip;
    done->frame_clamp = done->frame_clamp || clamp;
    ReleaseSRWLockExclusive(&g_patched_lock);

    const std::wstring path = paths::ModulePath(plugin);
    const PluginAnalysis analysis = AnalyzePlugin(plugin);
    if (flip)
        PatchFlipMetering(analysis, path.c_str());
    if (clamp)
        PatchFrameClamp(analysis, path.c_str());
}

} // namespace odg::streamline
