// Checks that the kernels a DLSS-G runtime carries can be retargeted to the
// architecture of the GPU in this machine and compiled by the driver. Point it
// at an nvngx_dlssg.dll and it reports, per kernel image, whether the driver
// accepted the retargeted module and what it said when it did not.
//
//   ptxprobe <nvngx_dlssg.dll> [target_sm] [newest]
//
// `newest` retargets from the PTX multi-frame generation uses (see
// kernels::PtxSource::Newest) instead of the closest.
//
// This is a development tool. It loads no game and changes nothing on disk.

#include "kernels/fatbin.h"

#include <libhat/scanner.hpp>
#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using PfnInit = int(__stdcall*)(unsigned int);
using PfnDeviceGet = int(__stdcall*)(int*, int);
using PfnDeviceGetName = int(__stdcall*)(char*, int, int);
using PfnDeviceGetAttribute = int(__stdcall*)(int*, int, int);
using PfnCtxCreate = int(__stdcall*)(void**, unsigned int, int);
using PfnModuleLoadDataEx = int(__stdcall*)(void**, const void*, unsigned int, int*, void**);
using PfnModuleUnload = int(__stdcall*)(void*);
using PfnGetErrorName = int(__stdcall*)(int, const char**);

enum : int {
    kAttributeComputeCapabilityMajor = 75,
    kAttributeComputeCapabilityMinor = 76,
    kJitInfoLogBuffer = 3,
    kJitInfoLogBufferSizeBytes = 4,
    kJitErrorLogBuffer = 5,
    kJitErrorLogBufferSizeBytes = 6,
};

struct Cuda {
    PfnInit init = nullptr;
    PfnDeviceGet device_get = nullptr;
    PfnDeviceGetName device_name = nullptr;
    PfnDeviceGetAttribute attribute = nullptr;
    PfnCtxCreate ctx_create = nullptr;
    PfnModuleLoadDataEx load = nullptr;
    PfnModuleUnload unload = nullptr;
    PfnGetErrorName error_name = nullptr;
};

template <typename T>
T Resolve(HMODULE module, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

bool LoadCuda(Cuda& cuda) {
    HMODULE module = LoadLibraryA("nvcuda.dll");
    if (!module)
        return false;
    cuda.init = Resolve<PfnInit>(module, "cuInit");
    cuda.device_get = Resolve<PfnDeviceGet>(module, "cuDeviceGet");
    cuda.device_name = Resolve<PfnDeviceGetName>(module, "cuDeviceGetName");
    cuda.attribute = Resolve<PfnDeviceGetAttribute>(module, "cuDeviceGetAttribute");
    cuda.ctx_create = Resolve<PfnCtxCreate>(module, "cuCtxCreate_v2");
    cuda.load = Resolve<PfnModuleLoadDataEx>(module, "cuModuleLoadDataEx");
    cuda.unload = Resolve<PfnModuleUnload>(module, "cuModuleUnload");
    cuda.error_name = Resolve<PfnGetErrorName>(module, "cuGetErrorName");
    return cuda.init && cuda.device_get && cuda.attribute && cuda.ctx_create && cuda.load;
}

std::vector<uint8_t> ReadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

// Every fatbin container in the image, located by its magic (FATBIN_MAGIC).
std::vector<std::pair<size_t, size_t>> FindContainers(const std::vector<uint8_t>& image) {
    constexpr auto kMagic = hat::compile_signature<"50 ED 55 BA">();
    const auto bytes = std::as_bytes(std::span(image));
    std::vector<std::pair<size_t, size_t>> found;
    size_t resume = 0;
    for (const auto& match : hat::find_all_pattern(bytes, kMagic)) {
        const size_t offset = static_cast<size_t>(match.get() - bytes.data());
        if (offset < resume)
            continue;
        const size_t total = odg::kernels::ContainerSize(image.data() + offset, image.size() - offset);
        std::vector<odg::kernels::Image> images;
        if (!total || total > image.size() - offset ||
            !odg::kernels::Describe(image.data() + offset, total, images))
            continue;
        found.emplace_back(offset, total);
        resume = offset + total;
    }
    return found;
}

// Names of the kernels a PTX module defines, from its `.entry` directives.
std::string EntryNames(const std::vector<uint8_t>& module) {
    constexpr std::string_view kEntry = ".entry ";
    const std::string_view text(reinterpret_cast<const char*>(module.data()), module.size());
    std::set<std::string_view> seen;
    std::string names;
    for (size_t at = text.find(kEntry); at != std::string_view::npos; at = text.find(kEntry, at)) {
        at += kEntry.size();
        size_t end = at;
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
            ++end;
        const std::string_view name = text.substr(at, end - at);
        if (name.empty() || !seen.insert(name).second)
            continue;
        if (!names.empty())
            names += ',';
        names += name;
    }
    return names;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: ptxprobe <nvngx_dlssg.dll> [target_sm] [newest]\n");
        return 2;
    }

    Cuda cuda;
    if (!LoadCuda(cuda)) {
        std::printf("nvcuda.dll not available\n");
        return 1;
    }
    if (cuda.init(0) != 0) {
        std::printf("cuInit failed\n");
        return 1;
    }
    int device = 0;
    cuda.device_get(&device, 0);
    char name[128] = {};
    if (cuda.device_name)
        cuda.device_name(name, sizeof(name), device);
    int major = 0, minor = 0;
    cuda.attribute(&major, kAttributeComputeCapabilityMajor, device);
    cuda.attribute(&minor, kAttributeComputeCapabilityMinor, device);
    const uint32_t device_sm = static_cast<uint32_t>(major * 10 + minor);
    const uint32_t requested = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 0;
    const uint32_t target = requested ? requested : device_sm;
    const bool newest = argc > 3 && std::string_view(argv[3]) == "newest";
    const auto source = newest ? odg::kernels::PtxSource::Newest : odg::kernels::PtxSource::Closest;

    void* context = nullptr;
    if (cuda.ctx_create(&context, 0, device) != 0) {
        std::printf("cuCtxCreate failed\n");
        return 1;
    }
    std::printf("device: %s (sm_%u), target: sm_%u\n", name, device_sm, target);

    const std::vector<uint8_t> image = ReadFile(argv[1]);
    if (image.empty()) {
        std::printf("cannot read %s\n", argv[1]);
        return 1;
    }
    const auto containers = FindContainers(image);
    std::printf("containers: %zu\n\n", containers.size());

    size_t retargeted = 0, accepted = 0, failed = 0, skipped = 0;
    for (size_t index = 0; index < containers.size(); ++index) {
        const uint8_t* blob = image.data() + containers[index].first;
        const size_t size = containers[index].second;

        std::vector<uint8_t> module;
        odg::kernels::Report report;
        if (!odg::kernels::Retarget(blob, size, target, module, report, source)) {
            ++skipped;
            std::printf("[%3zu] skipped        images=%u runnable=%d\n", index, report.images,
                        report.already_runnable);
            continue;
        }
        ++retargeted;

        char error_log[4096] = {};
        char info_log[4096] = {};
        int options[] = {kJitErrorLogBuffer, kJitErrorLogBufferSizeBytes, kJitInfoLogBuffer,
                         kJitInfoLogBufferSizeBytes};
        void* values[] = {error_log, reinterpret_cast<void*>(static_cast<uintptr_t>(sizeof(error_log))),
                          info_log, reinterpret_cast<void*>(static_cast<uintptr_t>(sizeof(info_log)))};
        void* loaded = nullptr;
        const int status = cuda.load(&loaded, module.data(), 4, options, values);
        const std::string entries = EntryNames(module);
        if (status == 0) {
            ++accepted;
            std::printf("[%3zu] ok   sm_%u->sm_%u  %s\n", index, report.source_arch, target,
                        entries.c_str());
            if (loaded && cuda.unload)
                cuda.unload(loaded);
        } else {
            ++failed;
            const char* code = nullptr;
            if (cuda.error_name)
                cuda.error_name(status, &code);
            std::printf("[%3zu] FAIL sm_%u->sm_%u  %s\n      %s: %s\n", index, report.source_arch,
                        target, entries.c_str(), code ? code : "?", error_log);
        }
    }

    std::printf("\nretargeted %zu, accepted %zu, failed %zu, skipped %zu\n", retargeted, accepted,
                failed, skipped);
    return failed == 0 ? 0 : 1;
}
