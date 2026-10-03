#pragma once

#include "kernels/cubin_params.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Decides what the driver receives when the DLSS-G runtime creates a kernel.
//
// The runtime believes the GPU is Ada, so every image it hands over is built for
// Ada. For each one there are three outcomes:
//   Unchanged    the GPU can already run it, or it is not a kernel image;
//   Substituted  it is replaced, preferably by NVIDIA's own image for this
//                architecture, otherwise by its PTX retargeted to it;
//   Refused      nothing runnable can be supplied. The creation fails, which
//                the runtime handles, instead of the driver receiving code this
//                GPU cannot execute. On Vulkan such code loads and then hangs
//                the GPU, so refusing is the only safe answer.
namespace odg::kernels {

struct Options {
    bool enabled = true;
    // 0 asks the CUDA driver for the architecture actually present.
    uint32_t target_sm = 0;
    // Retarget from the Blackwell PTX where it fits, for multi-frame
    // generation. See PtxSource::Newest.
    bool multi_frame = false;
    // Writes the first few images, before and after, next to the log.
    bool dump = false;
    std::wstring dump_directory;
};

void Configure(const Options& options);

// Called once NVAPI has reported an architecture this engine enables. Until
// then nothing is substituted, so a GPU that runs DLSS-G natively is never
// touched. The NVAPI ids also serve as a fallback for the target architecture
// when the CUDA driver cannot be reached.
void Activate(uint32_t architecture, uint32_t implementation);

// Configured and active on this GPU. Decisions are made whenever this holds;
// Options::enabled only decides whether a replacement is supplied or the image
// is refused.
bool Active();

// Architecture substitutions target, resolved once and cached. Called ahead of
// time from the worker thread, so the first kernel creation does not pay for
// initializing CUDA on the game's render thread. 0 when it cannot be determined.
uint32_t TargetSm();

enum class Decision { Unchanged, Substituted, Refused };

// How a kernel reached the driver, named once so the hooks and the log agree.
inline constexpr const char* kRouteD3D12 = "d3d12";
inline constexpr const char* kRouteVulkan = "vulkan";

// Who asked, for the answer and for the log.
struct Request {
    const char* route = ""; // kRouteD3D12 or kRouteVulkan
    HMODULE module = nullptr; // runtime that made the call, and whose own images
                              // are the only ones it can be answered from
    std::string caller;      // its file name, for the log
    std::string kernel;      // entry point, when the route names it

    // A request from the code at `return_address`, the hook's caller.
    static Request From(const char* route, const void* return_address);
};

// What the driver receives in place of the runtime's image.
struct Substitution {
    std::vector<uint8_t> image;
    uint32_t source_arch = 0; // architecture the replacement was built from
};

// Decides what the driver receives for `blob`. On Substituted, `out` holds the
// replacement.
Decision Decide(const void* blob, size_t size, const Request& request, Substitution& out);

// After the driver rejects `out`, a substitution built from newer PTX (see
// PtxSource::Newest), rebuilds it from the closest PTX. Returns false when there
// is nothing different to try. The caller tries the driver once more and
// reports that result.
bool Fallback(const void* blob, size_t size, uint32_t status, const Request& request,
              Substitution& out);

// Records the driver's answer for a substituted image. A refusal is reported and
// returned to the runtime unchanged; the original is never tried instead,
// because it is by construction an image this GPU cannot run.
void ReportDriverResult(uint32_t status, const char* route);

// Hands the driver `substitution` through `create`, which passes it one image
// and returns the driver's status, 0 for success. A refusal of an image built
// from newer PTX is retried once from the closest, and the final answer is
// reported. Every route calls the driver this way.
template <typename Create>
auto CreateSubstituted(const void* blob, size_t size, const Request& request,
                       Substitution& substitution, Create&& create) {
    auto status = create(substitution.image);
    if (status != 0 &&
        Fallback(blob, size, static_cast<uint32_t>(status), request, substitution))
        status = create(substitution.image);
    ReportDriverResult(static_cast<uint32_t>(status), request.route);
    return status;
}

// Writes the totals once the count has stopped moving, so a report says at a
// glance how many kernels were supplied and how. Call it from a periodic scan:
// nothing is written until two calls see the same total, and nothing more is
// written until the total changes again.
void ReportSummary();

// The image an NVAPI cubin-creation parameter block carries, on the D3D12
// cubin route. The block's layout is found once and then reused.
struct CubinCall {
    BlobFields fields;
    const void* data = nullptr;
    size_t size = 0;
    std::string kernel;
};
std::optional<CubinCall> ReadCubinCall(void* params);

} // namespace odg::kernels
