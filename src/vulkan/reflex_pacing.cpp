#include "vulkan/reflex_pacing.h"

#include "core/log.h"
#include "spoof/nvapi.h"
#include "streamline/streamline.h"

#include <atomic>

namespace odg::vulkan {
namespace {

// VkLatencySleepModeInfoNV, from VK_NV_low_latency2 in the Vulkan specification.
struct LatencySleepModeInfo {
    uint32_t type;
    const void* next;
    uint32_t low_latency_mode;
    uint32_t low_latency_boost;
    uint32_t minimum_interval_us;
};
using PfnSetLatencySleepMode = int32_t(__stdcall*)(void* device, uint64_t swapchain,
                                                   const LatencySleepModeInfo* info);
using PfnLatencySleep = int32_t(__stdcall*)(void* device, uint64_t swapchain, const void* info);

// Whether the wrappers are handed out, decided at the first resolution of
// either function and kept. They only work as a pair: a sleep that goes
// unwrapped is never seen, and the game would lose low latency for nothing.
enum class Wrapping : uint8_t { Undecided, Wrap, Leave };
std::atomic<Wrapping> g_wrapping{Wrapping::Undecided};

std::atomic<bool> g_present_pacing_fix{true};
std::atomic<bool> g_latency_sleep_seen{false};
std::atomic<bool> g_low_latency_off{false};

int32_t __stdcall HookedLatencySleep(void* device, uint64_t swapchain, const void* info) {
    const auto original = Original<PfnLatencySleep>(Function::LatencySleep, device);
    if (!original)
        return kVkErrorUnknown;
    g_latency_sleep_seen.store(true, std::memory_order_release);
    return original(device, swapchain, info);
}

int32_t __stdcall HookedSetLatencySleepMode(void* device, uint64_t swapchain,
                                            const LatencySleepModeInfo* info) {
    const auto original = Original<PfnSetLatencySleepMode>(Function::SetLatencySleepMode, device);
    if (!original)
        return kVkErrorUnknown;
    if (!info)
        return original(device, swapchain, info);

    const bool frame_generation = streamline::FrameGenerationOn();
    const bool game_sleeps = g_latency_sleep_seen.load(std::memory_order_acquire);
    const bool off =
        MustTurnLowLatencyOff(info->low_latency_mode != 0, frame_generation, game_sleeps);
    if (g_low_latency_off.exchange(off, std::memory_order_relaxed) != off)
        log::Event(log::Level::Info, "reflex_present_pacing",
                   {log::Field::Bool("low_latency_off", off),
                    log::Field::Bool("frame_generation", frame_generation),
                    log::Field::Bool("game_sleeps", game_sleeps)});
    if (!off)
        return original(device, swapchain, info);
    LatencySleepModeInfo sent = *info;
    sent.low_latency_mode = 0;
    sent.low_latency_boost = 0;
    return original(device, swapchain, &sent);
}

bool ShouldWrap() {
    Wrapping wrapping = g_wrapping.load(std::memory_order_acquire);
    if (wrapping == Wrapping::Undecided) {
        const auto architecture = spoof::nvapi::RealArchitecture();
        const bool wrap = architecture == spoof::nvapi::Architecture::Supported &&
                          g_present_pacing_fix.load(std::memory_order_acquire);
        Wrapping expected = Wrapping::Undecided;
        const bool decided_here =
            g_wrapping.compare_exchange_strong(expected, wrap ? Wrapping::Wrap : Wrapping::Leave);
        if (decided_here && architecture == spoof::nvapi::Architecture::Unknown)
            log::Event(log::Level::Warning, "reflex_pacing_skipped",
                       {log::Field::Str("note", "Reflex was resolved before the GPU was "
                                                "identified; the pacing fix is off")});
        wrapping = g_wrapping.load(std::memory_order_acquire);
    }
    return wrapping == Wrapping::Wrap;
}

} // namespace

bool MustTurnLowLatencyOff(bool requested, bool frame_generation, bool game_sleeps) {
    if (!requested)
        return false; // already off
    if (game_sleeps)
        return false; // the driver waits in vkLatencySleepNV, not in every present
    // Without generated frames there is one present per frame, which the
    // driver paces correctly.
    return frame_generation;
}

void SetPresentPacingFix(bool enabled) {
    g_present_pacing_fix.store(enabled, std::memory_order_release);
}

PfnVoidFunction ReflexWrapper(Function function) {
    switch (function) {
    case Function::SetLatencySleepMode:
        return ShouldWrap() ? reinterpret_cast<PfnVoidFunction>(&HookedSetLatencySleepMode)
                            : nullptr;
    case Function::LatencySleep:
        return ShouldWrap() ? reinterpret_cast<PfnVoidFunction>(&HookedLatencySleep) : nullptr;
    default:
        return nullptr;
    }
}

} // namespace odg::vulkan
