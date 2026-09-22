#include "provider/multi_frame.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/pe.h"
#include "core/signature.h"
#include "core/x86.h"
#include "kernels/device.h"

#include <libhat/process.hpp>
#include <libhat/scanner.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace odg::provider {
namespace {

// The builds checked carry two to four comparisons. None means the runtime
// gates some other way; many more means the pattern is matching something else.
// Either way nothing is rewritten.
constexpr size_t kMaxGates = 4;

// Parameters the runtime publishes for the game to read.
constexpr std::string_view kParameterPrefix = "DLSSG.";
constexpr std::string_view kMultiFrameParameter = "DLSSG.MultiFrameCountMax";
constexpr size_t kMaxParameterLength = 64;
// How far past a comparison its result is published. The builds checked load
// the parameter name within four instructions of the comparison.
constexpr int kInstructionsToPublication = 8;

std::atomic<bool> g_enabled{true};

// `cmp r32, imm32` against Blackwell's id, in its two encodings:
//   3D id          cmp eax, imm32
//   81 11111??? id cmp r32, imm32: ModRM mod 11 (register), reg 111 (/7 cmp)
// The second also matches the opcode of the form with a REX prefix, which
// leaves the immediate at the same place.
struct Encoding {
    hat::signature pattern;
    size_t immediate; // offset of the imm32 within the match
};

std::vector<Encoding> Encodings() {
    hat::signature eax = signature::Parse("3D");
    signature::Append(eax, uint32_t{kernels::kNvApiBlackwell});
    hat::signature reg = signature::Parse("81 11111???");
    signature::Append(reg, uint32_t{kernels::kNvApiBlackwell});
    return {{eax, 1}, {reg, 2}};
}

// The NUL-terminated text at `address`, when it lies inside `image` and names a
// published parameter.
std::string ParameterAt(std::span<const std::byte> image, const std::byte* address) {
    if (address < image.data() || address >= image.data() + image.size())
        return {};
    char text[kMaxParameterLength] = {};
    const size_t available = static_cast<size_t>(image.data() + image.size() - address);
    if (!pe::SafeCopy(text, address, std::min(available, sizeof(text) - 1)))
        return {};
    const std::string name(text);
    return name.starts_with(kParameterPrefix) ? name : std::string{};
}

// The DLSSG parameter whose name is loaded shortly after `compare`, if any.
std::string PublishedParameter(std::span<const std::byte> image, const std::byte* compare) {
    const std::byte* cursor = compare;
    for (int i = 0; i < kInstructionsToPublication; ++i) {
        const auto instruction = x86::Decode(cursor);
        if (!instruction)
            break;
        if (const auto operand = x86::RipRelativeOperand(*instruction)) {
            std::string name = ParameterAt(image, *operand);
            if (!name.empty())
                return name;
        }
        cursor = instruction->Next();
    }
    return {};
}

} // namespace

std::vector<Gate> FindMultiFrameGates(HMODULE provider) {
    const auto module = hat::process::module_at(provider);
    if (!module)
        return {};
    return FindMultiFrameGates(module->get_module_data(), module->get_executable_data());
}

std::vector<Gate> FindMultiFrameGates(std::span<const std::byte> image,
                                      std::span<const std::byte> code) {
    std::vector<Gate> gates;
    for (const Encoding& encoding : Encodings()) {
        for (const auto& match : hat::find_all_pattern(code, encoding.pattern)) {
            // Decoding at the match confirms the bytes read as a comparison of
            // the expected length. It cannot tell whether they are really the
            // tail of an earlier instruction; the reader below, and the bound
            // on how many gates a build may have, are what keep such a match
            // from being rewritten.
            const auto compare = x86::Decode(match.get());
            if (!compare || compare->length != encoding.immediate + sizeof(uint32_t))
                continue;
            // The instruction directly after a comparison is the one that reads
            // its flags: nothing can have come in between to change them. Every
            // gate in every build checked is written that way, so a reader
            // further off is left alone rather than guessed at.
            const auto consumer = x86::Decode(compare->Next());
            Gate gate;
            gate.immediate = match.get() + encoding.immediate;
            gate.condition =
                consumer ? x86::ConditionTested(*consumer) : x86::Condition::NotConditional;
            gate.publishes = PublishedParameter(image, compare->Next());
            // An ordering test is what an architecture floor is read with, and
            // a parameter of its own marks a capability that is not this one.
            gate.unlock = gate.condition == x86::Condition::Ordering &&
                          (gate.publishes.empty() || gate.publishes == kMultiFrameParameter);
            gates.push_back(std::move(gate));
        }
    }
    return gates;
}

void SetMultiFrameEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
}

bool MultiFrameEnabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void UnlockMultiFrame(HMODULE provider, bool at_load) {
    if (!provider || !MultiFrameEnabled())
        return;
    const auto gates = FindMultiFrameGates(provider);
    const auto unlocked = std::count_if(gates.begin(), gates.end(),
                                        [](const Gate& gate) { return gate.unlock; });
    if (unlocked == 0 || gates.size() > kMaxGates) {
        log::Event(log::Level::Warning, "multi_frame_gates_not_found",
                   {log::Field::Path("provider", paths::ModulePath(provider).c_str()),
                    log::Field::Uint("matches", gates.size()),
                    log::Field::Uint("ordering_gates", static_cast<size_t>(unlocked)),
                    log::Field::Str("note", "no multi-frame gate found in this runtime")});
        return;
    }

    const uint32_t reported = kernels::kNvApiAda;
    size_t patched = 0;
    for (const Gate& gate : gates) {
        if (!gate.unlock) {
            log::Event(log::Level::Info, "multi_frame_gate_left",
                       {log::Field::Str("publishes", gate.publishes),
                        log::Field::Str("condition", x86::ConditionName(gate.condition))});
            continue;
        }
        patched += pe::PatchCode(gate.immediate, &reported, sizeof(reported)) ? 1 : 0;
    }
    log::Event(patched == static_cast<size_t>(unlocked) ? log::Level::Info : log::Level::Warning,
               "multi_frame_unlocked",
               {log::Field::Path("provider", paths::ModulePath(provider).c_str()),
                log::Field::Uint("gates", static_cast<size_t>(unlocked)),
                log::Field::Uint("patched", patched), log::Field::Bool("at_load", at_load)});
}

} // namespace odg::provider
