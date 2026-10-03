#include "kernels/cubin.h"

#include "core/bytes.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace odg::kernels {
namespace {

// Elf64_Ehdr and Elf64_Shdr, as defined by the System V ABI. Every field is
// naturally aligned, so the structs match the file layout without packing.
struct ElfHeader {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_headers_offset;
    uint64_t section_headers_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_names_index;
};
static_assert(sizeof(ElfHeader) == 64);

struct SectionHeader {
    uint32_t name; // offset into the section-name table
    uint32_t type;
    uint64_t flags;
    uint64_t address;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t alignment;
    uint64_t entry_size;
};
static_assert(sizeof(SectionHeader) == 64);

constexpr uint8_t kElfMagic[] = {0x7F, 'E', 'L', 'F'};
constexpr size_t kElfClassIndex = 4;
constexpr uint8_t kElfClass64 = 2;
// EM_CUDA, the machine type NVIDIA registers for its GPU code.
constexpr uint16_t kMachineCuda = 190;
// NVIDIA stores the SM number in the low byte of e_flags: sm_89 reads as 0x59.
constexpr uint32_t kFlagsSmMask = 0xFF;

std::optional<ElfHeader> CubinHeader(std::span<const uint8_t> image) {
    const auto header = bytes::Read<ElfHeader>(image, 0);
    if (!header || std::memcmp(header->ident, kElfMagic, sizeof(kElfMagic)) != 0 ||
        header->ident[kElfClassIndex] != kElfClass64 || header->machine != kMachineCuda)
        return std::nullopt;
    return header;
}

// End of a header table, or empty when it does not fit in `limit` bytes.
std::optional<size_t> TableEnd(uint64_t offset, uint16_t entry_size, uint16_t count,
                               size_t limit) {
    const uint64_t length = uint64_t{entry_size} * count;
    if (offset > limit || length > limit - offset)
        return std::nullopt;
    return static_cast<size_t>(offset + length);
}

} // namespace

uint32_t CubinArch(const void* blob, size_t size) {
    const auto header = CubinHeader(bytes::View(blob, size));
    return header ? header->flags & kFlagsSmMask : 0;
}

size_t CubinSize(const void* blob, size_t available) {
    const auto header = CubinHeader(bytes::View(blob, available));
    if (!header)
        return 0;
    // The program headers follow the section table, so the image ends where the
    // later of the two ends. The section table alone cuts it short.
    const auto sections = TableEnd(header->section_headers_offset, header->section_header_size,
                                     header->section_header_count, available);
    const auto programs = TableEnd(header->program_headers_offset, header->program_header_size,
                                     header->program_header_count, available);
    if (!sections || !programs)
        return 0;
    const size_t size = std::max(*sections, *programs);
    return size > sizeof(ElfHeader) ? size : 0;
}

std::vector<CubinSection> CubinSections(const void* blob, size_t available) {
    std::vector<CubinSection> sections;
    const size_t size = CubinSize(blob, available);
    const auto image = bytes::View(blob, size);
    const auto header = CubinHeader(image);
    if (!header || header->section_header_size < sizeof(SectionHeader) ||
        header->section_names_index >= header->section_header_count)
        return sections;

    const auto section_header = [&](size_t index) {
        return bytes::Read<SectionHeader>(
            image, header->section_headers_offset + index * header->section_header_size);
    };
    const auto names_header = section_header(header->section_names_index);
    const auto names =
        names_header ? bytes::Slice(image, names_header->offset, names_header->size) : std::nullopt;
    if (!names)
        return sections;

    for (size_t i = 0; i < header->section_header_count; ++i) {
        const auto entry = section_header(i);
        if (!entry || entry->name >= names->size())
            continue;
        const auto data = bytes::Slice(image, entry->offset, entry->size);
        if (!data)
            continue;
        const auto* name = reinterpret_cast<const char*>(names->data() + entry->name);
        sections.push_back({std::string_view(name, strnlen(name, names->size() - entry->name)), *data});
    }
    return sections;
}

} // namespace odg::kernels
