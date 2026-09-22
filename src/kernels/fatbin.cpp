#include "kernels/fatbin.h"

#include "core/bytes.h"

#include <lz4.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace odg::kernels {
namespace {

// Container layout. The file header and the constants are those of CUDA's
// fatbinary.h. NVIDIA does not document the per-image header; its layout comes
// from the containers nvcc emits and is the one open-source fatbin tools read.
// Fields this code does not interpret are copied through unchanged.
struct FileHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint64_t fat_size; // bytes of images after the header
};
static_assert(sizeof(FileHeader) == 16);

struct EntryHeader {
    uint16_t kind;
    uint16_t unknown_02;
    uint32_t header_size; // this header, including any bytes after the fields below
    uint64_t payload_size; // bytes stored after the header
    uint32_t compressed_size;
    uint32_t unknown_14;
    uint32_t tool_version;
    uint32_t arch; // SM number
    uint32_t unknown_20[2];
    uint64_t flags;
    uint64_t unknown_30;
    uint64_t decompressed_size;
};
static_assert(sizeof(EntryHeader) == 0x40);

constexpr uint32_t kMagic = 0xBA55ED50;         // FATBIN_MAGIC
constexpr uint16_t kKindPtx = 1;                // FATBIN_2_PTX
constexpr uint64_t kFlagCompressed = 0x2000;    // FATBIN_FLAG_COMPRESS
// Bounds that keep a corrupt length from being trusted.
constexpr size_t kMaxEntryHeaderSize = 0x200;
constexpr size_t kMaxContainerSize = 256u << 20;
// nvcc aligns every payload to 8 bytes.
constexpr size_t kPayloadAlignment = 8;

std::span<const uint8_t> View(const void* blob, size_t size) {
    return blob ? std::span(static_cast<const uint8_t*>(blob), size) : std::span<const uint8_t>{};
}

// A container can be recognised from its header alone, which is all a caller
// has when it is probing a pointer whose length it does not yet know.
std::optional<FileHeader> ContainerHeader(std::span<const uint8_t> data) {
    const auto header = bytes::Read<FileHeader>(data, 0);
    if (!header || header->magic != kMagic || header->header_size < sizeof(FileHeader) ||
        header->fat_size == 0 || header->fat_size > kMaxContainerSize)
        return std::nullopt;
    return header;
}

bool Decompress(std::span<const uint8_t> in, size_t out_size, std::vector<uint8_t>& out) {
    // The decompressed length comes from the image's own header. A PTX module
    // never approaches the container limit, so a larger claim is corruption,
    // not something to allocate for.
    if (in.size() > kMaxContainerSize || out_size > kMaxContainerSize)
        return false;
    out.resize(out_size);
    const int written =
        LZ4_decompress_safe(reinterpret_cast<const char*>(in.data()), reinterpret_cast<char*>(out.data()),
                            static_cast<int>(in.size()), static_cast<int>(out_size));
    return written >= 0 && static_cast<size_t>(written) == out_size;
}

constexpr std::string_view kTargetDirective = ".target sm_";
constexpr std::string_view kDigits = "0123456789";

// Rewrites the architecture in the module's `.target sm_NN` directive. PTX
// carries the target as text, so the directive and the container's own
// architecture field both have to move.
bool RewriteTargetDirective(std::vector<uint8_t>& ptx, uint32_t arch) {
    std::string text(ptx.begin(), ptx.end());
    const std::string replacement = std::to_string(arch);
    bool rewritten = false;
    for (size_t at = text.find(kTargetDirective); at != std::string::npos;
         at = text.find(kTargetDirective, at)) {
        const size_t digits = at + kTargetDirective.size();
        const size_t count = std::min(text.find_first_not_of(kDigits, digits), text.size()) - digits;
        if (count) {
            text.replace(digits, count, replacement);
            rewritten = true;
        }
        at = digits;
    }
    ptx.assign(text.begin(), text.end());
    return rewritten;
}

// The payload of `image`, decompressed.
std::optional<std::vector<uint8_t>> Payload(std::span<const uint8_t> container, const Image& image) {
    const auto payload = container.subspan(image.payload_offset, image.payload_size);
    std::vector<uint8_t> out;
    if (!image.compressed) {
        out.assign(payload.begin(), payload.end());
        return out;
    }
    if (image.compressed_size > payload.size() ||
        !Decompress(payload.first(image.compressed_size), image.decompressed_size, out))
        return std::nullopt;
    return out;
}

// The declarations that fix how a PTX module is launched: its entry points and
// their parameters, launch bounds and shared and global memory. Two builds with
// the same interface accept the same launch.
std::string Interface(const std::vector<uint8_t>& ptx) {
    static constexpr std::string_view kDirectives[] = {
        ".visible", ".entry", ".param", ".maxntid", ".reqntid", ".minnctapersm",
        ".maxnreg", ".shared", ".extern", ".global", ".const"};
    const std::string_view text(reinterpret_cast<const char*>(ptx.data()), ptx.size());
    std::string declarations;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos)
            continue;
        line = line.substr(first);
        line = line.substr(0, line.find_last_not_of(" \t\r") + 1);
        for (const std::string_view directive : kDirectives) {
            if (line.starts_with(directive)) {
                declarations.append(line).push_back('\n');
                break;
            }
        }
    }
    return declarations;
}

} // namespace

bool CanRun(bool is_ptx, uint32_t image, uint32_t device) {
    if (!image || !device)
        return false;
    if (is_ptx)
        return image <= device;
    // SM numbers are major * 10 + minor.
    return image / 10 == device / 10 && image <= device;
}

size_t ContainerSize(const void* blob, size_t size) {
    const auto header = ContainerHeader(View(blob, size));
    return header ? header->header_size + static_cast<size_t>(header->fat_size) : 0;
}

bool Describe(const void* blob, size_t size, std::vector<Image>& images) {
    images.clear();
    const auto container = View(blob, size);
    const auto header = ContainerHeader(container);
    // The header states its own length, so a caller's `size` can be shorter
    // than it; checked first, or the subtraction below wraps.
    if (!header || header->header_size > size || header->fat_size > size - header->header_size)
        return false;

    size_t offset = header->header_size;
    const size_t end = header->header_size + static_cast<size_t>(header->fat_size);
    const auto body = container.first(end);
    while (const auto entry = bytes::Read<EntryHeader>(body, offset)) {
        if (entry->header_size < sizeof(EntryHeader) || entry->header_size > kMaxEntryHeaderSize ||
            !bytes::Slice(body, offset + entry->header_size, entry->payload_size))
            break;
        Image image;
        image.is_ptx = entry->kind == kKindPtx;
        image.arch = entry->arch;
        image.compressed = (entry->flags & kFlagCompressed) != 0;
        image.header_offset = offset;
        image.payload_offset = offset + entry->header_size;
        image.payload_size = static_cast<size_t>(entry->payload_size);
        if (image.compressed) {
            image.compressed_size = entry->compressed_size;
            image.decompressed_size = static_cast<size_t>(entry->decompressed_size);
        }
        images.push_back(image);
        offset = image.payload_offset + image.payload_size;
    }
    return !images.empty();
}

bool Retarget(const void* blob, size_t size, uint32_t arch, std::vector<uint8_t>& out,
              Report& report, PtxSource choice) {
    std::vector<Image> images;
    if (!Describe(blob, size, images))
        return false;
    const auto container = View(blob, size);

    report.images = static_cast<uint32_t>(images.size());
    std::vector<const Image*> candidates;
    for (const Image& image : images) {
        if (CanRun(image.is_ptx, image.arch, arch)) {
            report.already_runnable = true;
            return false;
        }
        // Every PTX image left is for a newer architecture.
        if (image.is_ptx)
            candidates.push_back(&image);
    }
    if (candidates.empty())
        return false;
    std::sort(candidates.begin(), candidates.end(),
              [](const Image* a, const Image* b) { return a->arch < b->arch; });

    const Image* source = candidates.front();
    auto closest = Payload(container, *source);
    if (!closest)
        return false;
    std::vector<uint8_t> ptx = std::move(*closest);
    if (choice == PtxSource::Newest) {
        const std::string interface = Interface(ptx);
        for (size_t i = candidates.size() - 1; i > 0; --i) {
            auto newer = Payload(container, *candidates[i]);
            if (newer && Interface(*newer) == interface) {
                source = candidates[i];
                ptx = std::move(*newer);
                break;
            }
        }
    }
    report.source_arch = source->arch;
    if (!RewriteTargetDirective(ptx, arch))
        return false;
    ptx.resize((ptx.size() + kPayloadAlignment - 1) / kPayloadAlignment * kPayloadAlignment, 0);

    // One image: the source's header, rewritten to describe uncompressed PTX
    // for `arch`, followed by the PTX.
    auto entry = *bytes::Read<EntryHeader>(container, source->header_offset);
    const auto entry_extra = container.subspan(source->header_offset + sizeof(EntryHeader),
                                               entry.header_size - sizeof(EntryHeader));
    entry.payload_size = ptx.size();
    entry.compressed_size = 0;
    entry.arch = arch;
    entry.flags &= ~kFlagCompressed;
    entry.decompressed_size = 0;

    auto file = *ContainerHeader(container);
    const size_t file_extra = file.header_size - sizeof(FileHeader);
    file.fat_size = entry.header_size + ptx.size();

    const auto append = [&out](const void* data, size_t length) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + length);
    };
    out.clear();
    append(&file, sizeof(file));
    append(container.data() + sizeof(FileHeader), file_extra);
    append(&entry, sizeof(entry));
    append(entry_extra.data(), entry_extra.size());
    append(ptx.data(), ptx.size());
    return true;
}

bool RetargetPtxText(const void* blob, size_t size, uint32_t arch, std::vector<uint8_t>& out,
                     Report& report) {
    // PTX opens with a .version directive followed by .target, after optional
    // comments and whitespace. Looking only at the opening bytes keeps a binary
    // image that happens to contain the text from being mistaken for PTX.
    constexpr size_t kHeaderWindow = 4096;
    constexpr std::string_view kVersionDirective = ".version ";
    if (!blob)
        return false;
    const std::string_view head(static_cast<const char*>(blob), std::min(size, kHeaderWindow));
    const size_t target = head.find(kTargetDirective);
    if (head.find(kVersionDirective) == std::string_view::npos || target == std::string_view::npos)
        return false;

    const std::string_view digits = head.substr(target + kTargetDirective.size());
    uint32_t source_arch = 0;
    std::from_chars(digits.data(), digits.data() + digits.size(), source_arch);
    if (!source_arch)
        return false;
    report.images = 1;
    if (CanRun(true, source_arch, arch)) {
        report.already_runnable = true;
        return false;
    }
    std::vector<uint8_t> ptx(static_cast<const uint8_t*>(blob),
                             static_cast<const uint8_t*>(blob) + size);
    if (!RewriteTargetDirective(ptx, arch))
        return false;
    report.source_arch = source_arch;
    out = std::move(ptx);
    return true;
}

} // namespace odg::kernels
