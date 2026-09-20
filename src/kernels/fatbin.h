#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// NVIDIA fatbin containers. The DLSS-G runtime hands one to NVAPI for every
// compute kernel it creates. NVIDIA compiles those kernels only for the
// architectures it supports, and PTX can only be JIT-compiled forward, so on an
// older card the container holds no image the driver can use. Retargeting
// rewrites the PTX inside for the architecture that is actually present.
namespace odg::kernels {

struct Image {
    bool is_ptx = false;
    uint32_t arch = 0; // SM number, e.g. 89 for sm_89
    bool compressed = false;
    size_t header_offset = 0;
    size_t payload_offset = 0;
    size_t payload_size = 0;      // bytes present in the container
    size_t compressed_size = 0;   // LZ4 stream within the payload; 0 when uncompressed
    size_t decompressed_size = 0; // 0 when stored uncompressed
};

struct Report {
    uint32_t images = 0;
    uint32_t source_arch = 0; // architecture the chosen image was built for
    bool already_runnable = false;
    // The replacement is one of the runtime's own images for this
    // architecture, used as shipped, rather than PTX rewritten by this code.
    bool native = false;
};

// Whether a GPU of architecture `device` can run an image built for `image`.
// CUDA guarantees two kinds of compatibility, and only these two. PTX is
// compiled by the driver on load and runs on its own architecture or any newer
// one. A cubin is machine code and runs only within its own major version, on
// the same or a newer minor: an sm_80 cubin runs on sm_86, an sm_89 one does not.
bool CanRun(bool is_ptx, uint32_t image, uint32_t device);

// Total container length taken from its own header, or 0 when the header does
// not validate. Lets a caller check a length it was given from elsewhere.
size_t ContainerSize(const void* blob, size_t size);

bool Describe(const void* blob, size_t size, std::vector<Image>& images);

// Which PTX a container is retargeted from, when it carries several.
enum class PtxSource {
    // The lowest architecture above the GPU: the build for the closest hardware.
    Closest,
    // The highest architecture whose kernel interface (entry points,
    // parameters, launch bounds, shared memory) is identical to the closest
    // one's, so the runtime's launch arguments mean the same to it. NVIDIA
    // writes the multi-frame paths only into its Blackwell builds: the Ada
    // builds of the same kernels assume one generated frame at the midpoint.
    // Falls back to Closest when no newer build matches.
    Newest,
};

// Rebuilds the container around a single PTX image targeting `arch`, taken from
// the PTX `choice` selects. Returns false, with `report.already_runnable` set,
// when the container already holds an image the GPU can run; nothing is changed
// that does not need to be. Also false when it holds no PTX to retarget, or
// when it does not parse.
bool Retarget(const void* blob, size_t size, uint32_t arch, std::vector<uint8_t>& out,
              Report& report, PtxSource choice = PtxSource::Closest);

// Plain PTX text, which both routes also accept as a module. Rewrites its
// `.target` for `arch`. Returns false, with `report.already_runnable` set, when
// the PTX already targets an architecture the GPU can run, and false when the
// blob is not PTX text.
bool RetargetPtxText(const void* blob, size_t size, uint32_t arch, std::vector<uint8_t>& out,
                     Report& report);

} // namespace odg::kernels
