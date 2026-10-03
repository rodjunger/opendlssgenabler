#include "kernels/provider_index.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/pe.h"
#include "kernels/cubin.h"
#include "kernels/fatbin.h"

#include <libhat/scanner.hpp>

#include <algorithm>
#include <cctype>
#include <span>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace odg::kernels {
namespace {

// FATBIN_MAGIC and the ELF magic, as they appear in memory.
constexpr auto kFatbinMagic = hat::compile_signature<"50 ED 55 BA">();
constexpr auto kElfMagic = hat::compile_signature<"7F 45 4C 46">();

// 64-bit FNV-1a.
constexpr uint64_t kFnvBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct Blob {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// What a cubin is, independent of the architecture it was built for: the kernel
// name and its machine code. NVIDIA's sm_86 and sm_89 builds of the same kernel
// variant carry byte-identical code and differ only in metadata, while two
// variants of one kernel differ in code, so this pair picks out exactly one
// variant. Pairing by storage order does not: the variants of a kernel are
// stored in a different order for each architecture.
struct Identity {
    std::string name;
    uint64_t code = 0;
    uint32_t arch = 0;
    size_t size = 0;
};

// One runtime's images. Several runtimes can be mapped at once, and a kernel is
// only ever answered from the one that created it.
struct Index {
    std::unordered_map<uint64_t, Blob> containers;                   // cubin -> its fatbin
    std::unordered_map<uint64_t, std::map<uint32_t, Blob>> variants; // kernel -> arch -> cubin
};

std::unordered_map<HMODULE, Index> g_indexes;
std::mutex g_mutex;

uint64_t Hash(std::span<const uint8_t> bytes, uint64_t hash = kFnvBasis) {
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

// A cubin inside a container is found by a hash of the whole image. Its length
// is the one its own headers state: the runtime rounds the length it passes up.
// Only the image's opening bytes are not enough; two builds of one kernel share
// their name tables, and 310.5 through 310.7 hold such pairs within 512 bytes.
std::optional<uint64_t> Fingerprint(const uint8_t* bytes, size_t available) {
    const size_t size = CubinSize(bytes, available);
    if (!size)
        return std::nullopt;
    return Hash({bytes, size});
}

uint64_t VariantKey(const std::string& name, uint64_t code) {
    const uint64_t hash = Hash({reinterpret_cast<const uint8_t*>(name.data()), name.size()});
    return Hash({reinterpret_cast<const uint8_t*>(&code), sizeof(code)}, hash);
}

bool IsIdentifier(std::string_view text) {
    if (text.empty())
        return false;
    return std::all_of(text.begin(), text.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    });
}

// Reads the kernel name and a hash of its code from the `.text.<kernel>`
// section.
bool Identify(const uint8_t* elf, size_t available, Identity& out) {
    static constexpr std::string_view kText = ".text.";
    const size_t size = CubinSize(elf, available);
    for (const CubinSection& section : CubinSections(elf, size)) {
        if (!section.name.starts_with(kText))
            continue;
        const std::string_view kernel = section.name.substr(kText.size());
        if (!IsIdentifier(kernel))
            return false;
        out.name.assign(kernel);
        out.code = Hash(section.data);
        out.arch = CubinArch(elf, size);
        out.size = size;
        return true;
    }
    return false;
}

const uint8_t* AsBytes(const std::byte* address) {
    return reinterpret_cast<const uint8_t*>(address);
}

// Calls visit(address, bytes_to_section_end) at each occurrence of `magic` in
// the provider's readable sections. visit returns how many bytes it consumed,
// so a match inside an image already taken is skipped.
template <typename Signature, typename Visit>
void ForEachMatch(HMODULE provider, const Signature& magic, Visit&& visit) {
    for (const std::span<const std::byte> section : pe::ReadableSections(provider)) {
        const std::byte* resume = section.data();
        const std::byte* end = section.data() + section.size();
        for (const auto& match : hat::find_all_pattern(section, magic)) {
            if (match.get() < resume)
                continue;
            const size_t consumed =
                visit(AsBytes(match.get()), static_cast<size_t>(end - match.get()));
            resume = match.get() + consumed;
        }
    }
}

} // namespace

std::optional<IndexSummary> BuildProviderIndex(HMODULE provider) {
    if (!provider || ProviderIndexed(provider))
        return std::nullopt;

    std::vector<Blob> containers;
    std::unordered_map<uint64_t, Blob> contained;
    // The same image in two containers could be answered from either, which
    // is a guess, so such a key answers nothing.
    std::unordered_set<uint64_t> ambiguous;
    ForEachMatch(provider, kFatbinMagic, [&](const uint8_t* blob, size_t available) -> size_t {
        const size_t total = ContainerSize(blob, available);
        std::vector<Image> images;
        if (!total || total > available || !Describe(blob, total, images))
            return 0;
        containers.push_back(Blob{blob, total});
        for (const Image& image : images) {
            if (image.is_ptx || image.compressed)
                continue;
            const auto key = Fingerprint(blob + image.payload_offset, image.payload_size);
            if (!key)
                continue;
            const auto [entry, added] = contained.try_emplace(*key, Blob{blob, total});
            if (!added && entry->second.data != blob)
                ambiguous.insert(*key);
        }
        return total;
    });
    for (const uint64_t key : ambiguous)
        contained.erase(key);

    const auto inside_container = [&](const uint8_t* address) {
        return std::any_of(containers.begin(), containers.end(), [&](const Blob& span) {
            return address >= span.data && address < span.data + span.size;
        });
    };

    size_t cubins = 0;
    std::unordered_map<uint64_t, std::map<uint32_t, Blob>> variants;
    ForEachMatch(provider, kElfMagic, [&](const uint8_t* elf, size_t available) -> size_t {
        Identity identity;
        if (inside_container(elf) || !Identify(elf, available, identity))
            return 0;
        variants[VariantKey(identity.name, identity.code)][identity.arch] = Blob{elf, identity.size};
        ++cubins;
        return identity.size;
    });

    size_t paired = 0;
    for (const auto& entry : variants) {
        if (entry.second.size() > 1)
            ++paired;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_indexes.try_emplace(provider, Index{std::move(contained), std::move(variants)});
    }
    const IndexSummary summary{containers.size(), cubins, paired, ambiguous.size()};
    log::Event(log::Level::Info, "provider_index_built",
               {log::Field::Path("provider", paths::ModulePath(provider).c_str()),
                log::Field::Uint("containers", summary.containers),
                log::Field::Uint("ambiguous_images", summary.ambiguous_images),
                log::Field::Uint("cubins", summary.cubins),
                log::Field::Uint("kernels_with_alternatives", summary.kernels_with_alternatives)});
    return summary;
}

bool ProviderIndexed(HMODULE provider) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_indexes.count(provider) != 0;
}

HMODULE SoleIndexedProvider() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_indexes.size() == 1 ? g_indexes.begin()->first : nullptr;
}

const void* FindNativeCubin(HMODULE provider, const void* cubin, size_t size, uint32_t arch,
                            size_t& out_size) {
    out_size = 0;
    Identity identity;
    if (!provider || !cubin || !Identify(static_cast<const uint8_t*>(cubin), size, identity))
        return nullptr;

    std::lock_guard<std::mutex> lock(g_mutex);
    const auto indexed = g_indexes.find(provider);
    if (indexed == g_indexes.end())
        return nullptr;
    const auto entry = indexed->second.variants.find(VariantKey(identity.name, identity.code));
    if (entry == indexed->second.variants.end())
        return nullptr;
    const auto image = entry->second.find(arch);
    if (image == entry->second.end())
        return nullptr;
    out_size = image->second.size;
    return image->second.data;
}

const void* FindContainerForCubin(HMODULE provider, const void* cubin, size_t size,
                                  size_t& container_size) {
    container_size = 0;
    if (!provider || !cubin || !size)
        return nullptr;
    const auto key = Fingerprint(static_cast<const uint8_t*>(cubin), size);
    if (!key)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_mutex);
    const auto indexed = g_indexes.find(provider);
    if (indexed == g_indexes.end())
        return nullptr;
    const auto found = indexed->second.containers.find(*key);
    if (found == indexed->second.containers.end())
        return nullptr;
    container_size = found->second.size;
    return found->second.data;
}

} // namespace odg::kernels
