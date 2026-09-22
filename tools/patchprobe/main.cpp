// Reports what the in-memory patches would change in a Streamline DLSS-G plugin
// (sl.dlss_g.dll) or an NVIDIA DLSS-G runtime (nvngx_dlssg.dll), and what the
// kernel index finds in a runtime, without changing either. Use it to check a new build before a game runs it.
//
//   patchprobe <dll>...
//
// This is a development tool. Each file is mapped but never initialised.

#include "kernels/provider_index.h"
#include "provider/multi_frame.h"
#include "streamline/plugin_patch.h"

#include <windows.h>

#include <cstdio>

namespace {

size_t Rva(const std::byte* address, HMODULE module) {
    return static_cast<size_t>(address - reinterpret_cast<const std::byte*>(module));
}

// Returns false when the plugin's flip-metering patch would not apply.
bool ReportPlugin(HMODULE plugin) {
    const auto analysis = odg::streamline::AnalyzePlugin(plugin);
    bool ok = true;
    if (const auto& flip = analysis.flip_metering) {
        std::printf("  flip metering: flag +0x%x, off %u, stores to rewrite %zu\n",
                    static_cast<unsigned>(flip->flag_offset), flip->off_value,
                    flip->opposite_stores.size());
        for (const std::byte* store : flip->opposite_stores)
            std::printf("    immediate at rva 0x%zx\n", Rva(store, plugin));
        ok = !flip->opposite_stores.empty();
    } else {
        std::printf("  flip metering: not found (%s)\n", analysis.flip_metering_problem);
        ok = false;
    }
    if (const auto& clamp = analysis.frame_clamp)
        std::printf("  frame clamp: limit %u, cmovb at rva 0x%zx\n", clamp->limit,
                    Rva(clamp->cmov, plugin));
    else
        std::printf("  frame clamp: not found (%zu matches)\n", analysis.frame_clamp_matches);
    return ok;
}

void ReportRuntime(HMODULE runtime) {
    if (const auto index = odg::kernels::BuildProviderIndex(runtime))
        std::printf("  kernel index: %zu containers, %zu ambiguous images, %zu cubins, "
                    "%zu kernels with a cubin for more than one architecture\n",
                    index->containers, index->ambiguous_images, index->cubins,
                    index->kernels_with_alternatives);
    const auto gates = odg::provider::FindMultiFrameGates(runtime);
    std::printf("  comparisons against Blackwell: %zu\n", gates.size());
    for (const auto& gate : gates)
        std::printf("    %s rva 0x%zx  read as %-8s  publishes %s\n",
                    gate.unlock ? "unlock" : "leave ", Rva(gate.immediate, runtime),
                    odg::x86::ConditionName(gate.condition),
                    gate.publishes.empty() ? "-" : gate.publishes.c_str());
}

int Probe(const char* path) {
    // Maps the image without running its entry point or loading its imports.
    HMODULE module = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) {
        std::printf("%s: cannot map (error %lu)\n", path, GetLastError());
        return 1;
    }
    std::printf("%s\n", path);
    int status = 0;
    if (GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"))
        ReportRuntime(module);
    else if (!ReportPlugin(module))
        status = 1;
    // Kept mapped: the kernel index is keyed by module, and a later file
    // mapped at the same address would otherwise be taken as indexed already.
    return status;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: patchprobe <dll>...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i)
        status |= Probe(argv[i]);
    return status;
}
