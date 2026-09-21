// Unit tests for the parts of the engine that are pure logic: configuration
// parsing, the fatbin container, cubins, the CUDA compatibility rule, x86
// decoding and text conversion. They need no GPU and no game.
//
// Build with -DODG_BUILD_TESTS=ON and run odg_unit_tests.exe on Windows.

#include "app/settings.h"
#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/text.h"
#include "core/x86.h"
#include "kernels/cubin.h"
#include "kernels/cubin_params.h"
#include "kernels/device.h"
#include "kernels/fatbin.h"
#include "kernels/substitute.h"

#include <lz4.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        ++g_checks;                                                                         \
        if (!(condition)) {                                                                 \
            ++g_failures;                                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                \
        }                                                                                   \
    } while (0)

std::wstring WriteTemp(const std::string& content) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"odg_unit_test.ini";
    FILE* file = _wfopen(path.c_str(), L"wb");
    std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);
    return path;
}

void TestConfig() {
    odg::config::Ini ini;
    const std::wstring path = WriteTemp("\xEF\xBB\xBF[General]\r\n"
                                        "Enabled=0 ; trailing comment\r\n"
                                        "[Logging]\r\n"
                                        "level = 3\r\n"
                                        "Directory=C:\\games;odd\\logs\r\n"
                                        "[Broken]\r\n"
                                        "Number=twelve\r\n"
                                        "Flag=maybe\r\n"
                                        "[Kernels] ; a comment after a section\r\n"
                                        "TargetSM=86\r\n");
    CHECK(ini.Load(path));
    // The byte-order mark must not swallow the first section.
    CHECK(ini.GetBool("General", "Enabled", true) == false);
    // Lookups are case-insensitive and whitespace around `=` is ignored.
    CHECK(ini.GetInt("LOGGING", "Level", 1) == 3);
    // A `;` that does not follow whitespace is part of the value.
    CHECK(ini.GetString("Logging", "Directory") == "C:\\games;odd\\logs");
    // Malformed values fall back and are recorded, never silently accepted.
    CHECK(ini.GetInt("Broken", "Number", 7) == 7);
    CHECK(ini.GetBool("Broken", "Flag", true) == true);
    CHECK(ini.Rejected().size() == 2);
    // A comment after a section header does not hide the section.
    CHECK(ini.GetInt("Kernels", "TargetSM", 0) == 86);
    // Missing values fall back without being recorded.
    CHECK(ini.GetInt("Missing", "Key", 5) == 5);
    CHECK(ini.Rejected().size() == 2);
    DeleteFileW(path.c_str());
}

// The shipped opendlssg.ini, next to the test executable, must parse with no
// rejected values and produce the defaults the code declares.
void TestShippedIni() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"opendlssg.ini";
    odg::config::Ini ini;
    CHECK(ini.Load(path));
    const odg::app::Settings loaded = odg::app::Settings::FromIni(ini);
    for (const std::string& rejected : ini.Rejected())
        std::printf("  rejected: %s\n", rejected.c_str());
    CHECK(ini.Rejected().empty());
    const odg::app::Settings defaults;
    CHECK(loaded.enabled == defaults.enabled);
    CHECK(loaded.spoof_callers == defaults.spoof_callers);
    CHECK(loaded.patch_flip_metering == defaults.patch_flip_metering);
    CHECK(loaded.flip_metering_value == defaults.flip_metering_value);
    CHECK(loaded.patch_frame_clamp == defaults.patch_frame_clamp);
    CHECK(loaded.retarget_kernels == defaults.retarget_kernels);
    CHECK(loaded.vulkan_hooks == defaults.vulkan_hooks);
    CHECK(loaded.log_level == defaults.log_level);
    CHECK(loaded.log_directory == defaults.log_directory);
}

void TestCanRun() {
    using odg::kernels::CanRun;
    // PTX is compiled by the driver and runs on its architecture or newer.
    CHECK(CanRun(true, 80, 86));
    CHECK(CanRun(true, 86, 86));
    CHECK(!CanRun(true, 89, 86));
    CHECK(CanRun(true, 89, 120));
    // A cubin runs only within its major version, on the same or newer minor.
    CHECK(CanRun(false, 80, 86));
    CHECK(CanRun(false, 86, 86));
    CHECK(!CanRun(false, 89, 86));
    CHECK(!CanRun(false, 89, 120));
    CHECK(!CanRun(false, 75, 86));
    CHECK(!CanRun(true, 0, 86));
}

template <typename T>
void Put(std::vector<uint8_t>& out, size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

struct PtxImage {
    uint32_t arch;
    std::string text;
};

// A container of PTX images, laid out as nvcc does. Offsets are written out here
// rather than taken from the parser, so the two are checked against each other.
// When `compressed`, each payload is LZ4 and padded past the stream, as nvcc
// stores it.
std::vector<uint8_t> MakeContainer(const std::vector<PtxImage>& ptx_images, bool compressed) {
    constexpr size_t kFileHeader = 16;
    constexpr size_t kEntryHeader = 0x48; // longer than the fixed fields, as in real containers
    constexpr uint64_t kFlagCompressed = 0x2000;

    std::vector<uint8_t> out(kFileHeader, 0);
    for (const PtxImage& image : ptx_images) {
        std::string text = image.text;
        text.push_back('\0');
        std::string payload = text;
        if (compressed) {
            payload.resize(static_cast<size_t>(LZ4_compressBound(static_cast<int>(text.size()))));
            payload.resize(static_cast<size_t>(LZ4_compress_default(
                text.data(), payload.data(), static_cast<int>(text.size()),
                static_cast<int>(payload.size()))));
        }
        const size_t stream = payload.size();
        while (payload.size() % 8)
            payload.push_back('\0');

        const size_t entry = out.size();
        out.resize(entry + kEntryHeader + payload.size(), 0);
        Put<uint16_t>(out, entry + 0x00, 1); // kind: PTX
        Put<uint32_t>(out, entry + 0x04, kEntryHeader);
        Put<uint64_t>(out, entry + 0x08, payload.size());
        Put<uint32_t>(out, entry + 0x1C, image.arch);
        if (compressed) {
            Put<uint32_t>(out, entry + 0x10, static_cast<uint32_t>(stream));
            Put<uint64_t>(out, entry + 0x28, kFlagCompressed);
            Put<uint64_t>(out, entry + 0x38, text.size());
        }
        std::memcpy(out.data() + entry + kEntryHeader, payload.data(), payload.size());
    }
    Put<uint32_t>(out, 0x00, 0xBA55ED50); // magic
    Put<uint16_t>(out, 0x04, 1);          // version
    Put<uint16_t>(out, 0x06, kFileHeader);
    Put<uint64_t>(out, 0x08, out.size() - kFileHeader);
    return out;
}

std::vector<uint8_t> MakeContainer(uint32_t arch, const std::string& ptx, bool compressed = false) {
    return MakeContainer({{arch, ptx}}, compressed);
}

std::string PtxFor(uint32_t arch, const std::string& parameters = "",
                   const std::string& body = "ret;") {
    return ".version 8.3\n.target sm_" + std::to_string(arch) +
           "\n.address_size 64\n.visible .entry k(" + parameters + ")\n{\n    " + body +
           "\n}\n";
}

// Multi-frame retargets from the newest PTX, but only when it is launched the
// same way as the closest build.
void TestPtxSource() {
    using namespace odg::kernels;
    const std::string params = "\n.param .align 8 .b8 k_param_0[144]\n";
    const std::string ada = PtxFor(89, params, "mul.f32 %f1, %f2, 0f3F000000;");
    const std::string blackwell = PtxFor(120, params, "ld.param.f32 %f1, [k_param_0+32];");
    const std::vector<uint8_t> both = MakeContainer({{89, ada}, {120, blackwell}}, true);

    std::vector<uint8_t> out;
    Report closest;
    CHECK(Retarget(both.data(), both.size(), 86, out, closest, PtxSource::Closest));
    CHECK(closest.source_arch == 89);
    Report newest;
    CHECK(Retarget(both.data(), both.size(), 86, out, newest, PtxSource::Newest));
    CHECK(newest.source_arch == 120);
    std::vector<Image> images;
    CHECK(Describe(out.data(), out.size(), images) && images.size() == 1 && images[0].arch == 86);
    const std::string text(reinterpret_cast<const char*>(out.data()) + images[0].payload_offset,
                           images[0].payload_size);
    CHECK(text.find("k_param_0+32") != std::string::npos && text.find("sm_120") == std::string::npos);

    // A driver that rejects the newest source gets the closest one once.
    Options options;
    options.target_sm = 86;
    options.multi_frame = true;
    Configure(options);
    Activate(kNvApiAmpere, 2);
    Request request;
    request.route = "test";
    CHECK(Decide(both.data(), both.size(), request, out) == Decision::Substituted);
    CHECK(Describe(out.data(), out.size(), images));
    CHECK(Fallback(both.data(), both.size(), 1, request, out));
    CHECK(Describe(out.data(), out.size(), images) && images.size() == 1);
    const std::string closest_text(reinterpret_cast<const char*>(out.data()) +
                                       images[0].payload_offset,
                                   images[0].payload_size);
    CHECK(closest_text.find("0f3F000000") != std::string::npos);
    CHECK(!Fallback(both.data(), both.size(), 1, request, out)); // only once

    // A different parameter block would be launched with the wrong arguments.
    const std::string other = PtxFor(120, "\n.param .u64 k_param_0\n", "ret;");
    const std::vector<uint8_t> mismatched = MakeContainer({{89, ada}, {120, other}}, false);
    Report fallback;
    CHECK(Retarget(mismatched.data(), mismatched.size(), 86, out, fallback, PtxSource::Newest));
    CHECK(fallback.source_arch == 89);
}

void TestRetarget() {
    using namespace odg::kernels;

    const std::vector<uint8_t> ada = MakeContainer(89, PtxFor(89));
    CHECK(ContainerSize(ada.data(), ada.size()) == ada.size());

    std::vector<uint8_t> out;
    Report report;
    CHECK(Retarget(ada.data(), ada.size(), 86, out, report));
    CHECK(report.source_arch == 89);
    std::vector<Image> images;
    CHECK(Describe(out.data(), out.size(), images));
    CHECK(images.size() == 1 && images[0].is_ptx && images[0].arch == 86);
    const std::string text(reinterpret_cast<const char*>(out.data()) + images[0].payload_offset,
                           images[0].payload_size);
    CHECK(text.find(".target sm_86") != std::string::npos);
    CHECK(text.find("sm_89") == std::string::npos);

    // An image the GPU can already run is left alone.
    const std::vector<uint8_t> ampere = MakeContainer(80, PtxFor(80));
    Report untouched;
    CHECK(!Retarget(ampere.data(), ampere.size(), 86, out, untouched));
    CHECK(untouched.already_runnable);

    // A newer GPU runs Ada PTX as it is, so nothing is changed there either.
    Report blackwell;
    CHECK(Retarget(ada.data(), ada.size(), 120, out, blackwell) == false);
    CHECK(blackwell.already_runnable);

    // Truncated input is rejected rather than read past its end.
    CHECK(!Describe(ada.data(), 20, images));

    // A compressed image comes out uncompressed, with its flag cleared.
    const std::vector<uint8_t> packed = MakeContainer(89, PtxFor(89), true);
    CHECK(Describe(packed.data(), packed.size(), images));
    CHECK(images.size() == 1 && images[0].compressed);
    Report unpacked;
    CHECK(Retarget(packed.data(), packed.size(), 86, out, unpacked));
    CHECK(Describe(out.data(), out.size(), images));
    CHECK(images.size() == 1 && !images[0].compressed && images[0].arch == 86);
    const std::string plain(reinterpret_cast<const char*>(out.data()) + images[0].payload_offset,
                            images[0].payload_size);
    CHECK(plain.find(".target sm_86") != std::string::npos);
}

// A cubin with a section-name table and one kernel section, as ptxas lays it
// out: ELF header, section data, section table, program headers.
std::vector<uint8_t> MakeCubin(uint32_t sm) {
    const std::string names = std::string("\0.shstrtab\0.text.kernel\0", 25);
    const std::string code = "\x11\x22\x33\x44";
    constexpr size_t kHeader = 64, kSection = 64, kProgram = 56;
    const size_t names_at = kHeader, code_at = names_at + names.size();
    const size_t table_at = (code_at + code.size() + 7) / 8 * 8;
    const size_t programs_at = table_at + 3 * kSection;

    std::vector<uint8_t> out(programs_at + kProgram, 0);
    std::memcpy(out.data(), "\x7F" "ELF\x02", 5); // 64-bit
    Put<uint16_t>(out, 0x12, 190);                  // EM_CUDA
    Put<uint64_t>(out, 0x20, programs_at);
    Put<uint64_t>(out, 0x28, table_at);
    Put<uint32_t>(out, 0x30, sm);
    Put<uint16_t>(out, 0x36, kProgram);
    Put<uint16_t>(out, 0x38, 1);
    Put<uint16_t>(out, 0x3A, kSection);
    Put<uint16_t>(out, 0x3C, 3);
    Put<uint16_t>(out, 0x3E, 1); // names are section 1
    std::memcpy(out.data() + names_at, names.data(), names.size());
    std::memcpy(out.data() + code_at, code.data(), code.size());
    const auto section = [&](size_t index, uint32_t name, size_t offset, size_t size) {
        const size_t at = table_at + index * kSection;
        Put<uint32_t>(out, at + 0x00, name);
        Put<uint64_t>(out, at + 0x18, offset);
        Put<uint64_t>(out, at + 0x20, size);
    };
    section(1, 1, names_at, names.size());
    section(2, 11, code_at, code.size());
    return out;
}

void TestCubin() {
    using namespace odg::kernels;
    std::vector<uint8_t> cubin = MakeCubin(86);
    CHECK(CubinArch(cubin.data(), cubin.size()) == 86);
    // The image ends after the program headers, not the section table.
    CHECK(CubinSize(cubin.data(), cubin.size() + 100) == cubin.size());
    CHECK(CubinSize(cubin.data(), cubin.size() - 1) == 0);

    const auto sections = CubinSections(cubin.data(), cubin.size());
    CHECK(sections.size() == 3);
    CHECK(sections.size() == 3 && sections[2].name == ".text.kernel" && sections[2].data.size() == 4);

    Put<uint16_t>(cubin, 0x12, 62); // x86-64, not a cubin
    CHECK(CubinArch(cubin.data(), cubin.size()) == 0);
    CHECK(CubinSections(cubin.data(), cubin.size()).empty());
}

// NvAPI_D3D12_CreateCubinComputeShaderExV2's parameter block, laid out as the
// driver has it today: size, then the container, its length and the kernel name
// at offsets the search has to find without being told.
void TestCubinParams() {
    using namespace odg::kernels;
    const std::vector<uint8_t> container = MakeContainer(89, PtxFor(89));
    static const char kName[] = "cuda_clear_view_kernel";
    uint8_t block[0x50] = {};
    const uint64_t block_size = sizeof(block);
    const void* data = container.data();
    const uint64_t data_size = container.size();
    const char* name = kName;
    std::memcpy(block + 0x00, &block_size, sizeof(block_size));
    std::memcpy(block + 0x18, &data, sizeof(data));
    std::memcpy(block + 0x20, &data_size, sizeof(data_size));
    std::memcpy(block + 0x38, &name, sizeof(name));

    const BlobFields fields = LocateBlob(block);
    CHECK(fields.valid && fields.struct_size == sizeof(block));
    CHECK(fields.data_offset == 0x18 && fields.size_offset == 0x20 && fields.size_is_64bit);
    CHECK(fields.name_offset == 0x38 && ReadName(block, fields) == kName);
    size_t size = 0;
    CHECK(ReadBlob(block, fields, size) == data && size == container.size());
}

void TestX86() {
    using namespace odg::x86;
    const std::byte* none = nullptr;

    // mov byte ptr [rbx+38BCh], 0
    const uint8_t store[] = {0xC6, 0x83, 0xBC, 0x38, 0x00, 0x00, 0x00};
    const auto decoded = Decode(reinterpret_cast<const std::byte*>(store));
    CHECK(decoded && decoded->length == sizeof(store));
    const auto parsed = decoded ? AsByteStore(*decoded) : std::nullopt;
    CHECK(parsed && parsed->displacement == 0x38BC && parsed->value == 0);

    // mov byte ptr [rsp], 1 addresses through a SIB byte: not an object field.
    const uint8_t stack[] = {0xC6, 0x04, 0x24, 0x01};
    const auto on_stack = Decode(reinterpret_cast<const std::byte*>(stack));
    CHECK(on_stack && !AsByteStore(*on_stack));

    // jmp +0x10, relative to the end of the 5-byte instruction.
    const uint8_t near_jump[] = {0xE9, 0x10, 0x00, 0x00, 0x00};
    const auto jump = Decode(reinterpret_cast<const std::byte*>(near_jump));
    const auto target = jump ? JumpTarget(*jump) : std::optional(none);
    CHECK(target && *target == reinterpret_cast<const std::byte*>(near_jump) + 5 + 0x10);

    // jmp qword ptr [rip+2]: the destination is stored in the pointer-aligned
    // slot two bytes past the end of the instruction.
    struct {
        uint8_t code[6] = {0xFF, 0x25, 0x02, 0x00, 0x00, 0x00};
        const void* slot = nullptr;
    } thunk;
    thunk.slot = &thunk;
    const auto indirect = Decode(reinterpret_cast<const std::byte*>(thunk.code));
    const auto destination = indirect ? JumpTarget(*indirect) : std::optional(none);
    CHECK(destination && *destination == reinterpret_cast<const std::byte*>(&thunk));

    // lea rcx, [rip+0x10]
    const uint8_t lea[] = {0x48, 0x8D, 0x0D, 0x10, 0x00, 0x00, 0x00};
    const auto load = Decode(reinterpret_cast<const std::byte*>(lea));
    const auto operand = load ? RipRelativeOperand(*load) : std::optional(none);
    CHECK(operand && *operand == reinterpret_cast<const std::byte*>(lea) + sizeof(lea) + 0x10);
    CHECK(!RipRelativeOperand(*decoded)); // [rbx+disp32] is not relative to rip

    CHECK(!AsByteStore(*jump));
    CHECK(!JumpTarget(*decoded));
}

void TestPtxText() {
    using namespace odg::kernels;
    const std::string ptx = PtxFor(89);
    std::vector<uint8_t> out;
    Report report;
    CHECK(RetargetPtxText(ptx.data(), ptx.size(), 86, out, report));
    CHECK(std::string(out.begin(), out.end()).find(".target sm_86") != std::string::npos);

    Report runnable;
    const std::string older = PtxFor(75);
    CHECK(!RetargetPtxText(older.data(), older.size(), 86, out, runnable));
    CHECK(runnable.already_runnable);

    // A three-digit target is replaced whole, not digit by digit.
    Report wide;
    const std::string ada = PtxFor(89);
    CHECK(RetargetPtxText(ada.data(), ada.size(), 100, out, wide) == false); // 89 runs on 100
    const std::string future = PtxFor(120);
    Report down;
    CHECK(RetargetPtxText(future.data(), future.size(), 86, out, down));
    CHECK(std::string(out.begin(), out.end()).find(".target sm_86\n") != std::string::npos);

    // Binary data is not mistaken for PTX.
    const uint8_t binary[64] = {0x7F, 'E', 'L', 'F'};
    Report none;
    CHECK(!RetargetPtxText(binary, sizeof(binary), 86, out, none));
    CHECK(!none.already_runnable);
}

size_t CountLogs(const std::wstring& directory) {
    size_t count = 0;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((directory + L"\\loader_*.jsonl").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    do {
        ++count;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return count;
}

void TestLogPruning() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring directory = std::wstring(temp) + L"odg_log_test";
    CreateDirectoryW(directory.c_str(), nullptr);
    for (int i = 0; i < 25; ++i) {
        const std::wstring name = directory + L"\\loader_" + std::to_wstring(900000 + i) + L".jsonl";
        FILE* file = _wfopen(name.c_str(), L"wb");
        std::fclose(file);
    }
    odg::log::Open(directory, odg::log::Level::Info, L"loader");
    odg::log::Event(odg::log::Level::Info, "test");
    odg::log::Close();
    // The newest ten are kept, counting the file just opened.
    CHECK(CountLogs(directory) == 10);
}

void TestConditions() {
    using odg::x86::Condition;
    using odg::x86::ConditionTested;
    auto at = [](const unsigned char* bytes) {
        return *odg::x86::Decode(reinterpret_cast<const std::byte*>(bytes));
    };
    // An architecture floor is read with an ordering test, whatever form the
    // compiler picked for it.
    const unsigned char jl_short[] = {0x7C, 0x05};
    const unsigned char jl_near[] = {0x0F, 0x8C, 0x05, 0x00, 0x00, 0x00};
    const unsigned char setae[] = {0x0F, 0x93, 0xC0};
    const unsigned char cmovl[] = {0x0F, 0x4C, 0xC8};
    CHECK(ConditionTested(at(jl_short)) == Condition::Ordering);
    CHECK(ConditionTested(at(jl_near)) == Condition::Ordering);
    CHECK(ConditionTested(at(setae)) == Condition::Ordering);
    CHECK(ConditionTested(at(cmovl)) == Condition::Ordering);
    // Equality asks a different question and must never be rewritten.
    const unsigned char je[] = {0x74, 0x05};
    const unsigned char sete[] = {0x0F, 0x94, 0xC0};
    const unsigned char jne_near[] = {0x0F, 0x85, 0x05, 0x00, 0x00, 0x00};
    CHECK(ConditionTested(at(je)) == Condition::Equality);
    CHECK(ConditionTested(at(sete)) == Condition::Equality);
    CHECK(ConditionTested(at(jne_near)) == Condition::Equality);
    // Neither, and not conditional at all.
    const unsigned char jo[] = {0x70, 0x05};
    const unsigned char nop[] = {0x90};
    CHECK(ConditionTested(at(jo)) == Condition::Other);
    CHECK(ConditionTested(at(nop)) == Condition::NotConditional);

    // A gate as the runtime compiles it: cmp eax, 0x1B0 ; setae al. The reader
    // is the next instruction, which is what the gate finder decodes.
    const unsigned char gate[] = {0x3D, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x93, 0xC0};
    const auto compare = odg::x86::Decode(reinterpret_cast<const std::byte*>(gate));
    CHECK(compare.has_value());
    CHECK(compare && compare->length == 5);
    CHECK(compare && ConditionTested(*odg::x86::Decode(compare->Next())) == Condition::Ordering);
}

void TestLogLevels() {
    using odg::log::Level;
    using odg::log::LevelFromSetting;
    // The INI has four settings; the engine has five levels, because warnings
    // belong in an errors-only log.
    CHECK(LevelFromSetting(0) == Level::Off);
    CHECK(LevelFromSetting(1) == Level::Warning);
    CHECK(LevelFromSetting(2) == Level::Info);
    CHECK(LevelFromSetting(3) == Level::Trace);

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::wstring directory = std::wstring(temp) + L"odg_level_test";
    odg::log::Open(directory, LevelFromSetting(1), L"loader");
    CHECK(odg::log::Enabled(Level::Error));
    CHECK(odg::log::Enabled(Level::Warning));
    CHECK(!odg::log::Enabled(Level::Info));
    CHECK(!odg::log::Enabled(Level::Trace));
    odg::log::Close();
    CHECK(!odg::log::Enabled(Level::Error));
}

void TestPathRedaction() {
    // Logs are attached to public bug reports, so a path under the user's
    // profile must not carry the account name out of the machine.
    SetEnvironmentVariableW(L"USERPROFILE", L"C:\\Users\\somebody");
    const odg::log::Field under = odg::log::Field::Path("path",
                                                        L"C:\\Users\\somebody\\Games\\a.exe");
    CHECK(under.token.find("somebody") == std::string::npos);
    CHECK(under.token.find("%USERPROFILE%") != std::string::npos);
    CHECK(under.token.find("Games") != std::string::npos);
    // Windows paths are case insensitive, and nothing outside the profile is
    // touched.
    const odg::log::Field mixed = odg::log::Field::Path("path",
                                                        L"c:\\users\\SOMEBODY\\x.dll");
    CHECK(mixed.token.find("%USERPROFILE%") != std::string::npos);
    const odg::log::Field elsewhere =
        odg::log::Field::Path("path", L"C:\\Program Files\\Game\\game.exe");
    CHECK(elsewhere.token.find("Program Files") != std::string::npos);
    CHECK(elsewhere.token.find("%USERPROFILE%") == std::string::npos);
    CHECK(odg::log::Field::Path("path", nullptr).token == "\"\"");
}

void TestComponentName() {
    using odg::paths::ComponentFileName;
    // A copy NGX downloaded is named after the architecture and the application,
    // so only its directory in the model store says what it is.
    const std::wstring store = L"C:\\ProgramData\\NVIDIA\\NGX\\models\\";
    CHECK(ComponentFileName(store + L"sl_common_0\\versions\\134656\\files\\170_E658703.dll") ==
          L"sl.common.dll");
    CHECK(ComponentFileName(store + L"sl_common_override_0\\versions\\1\\files\\190_E658700.dll") ==
          L"sl.common.dll");
    CHECK(ComponentFileName(store + L"sl_dlss_g_0\\versions\\134656\\files\\170_E658703.dll") ==
          L"sl.dlss_g.dll");
    CHECK(ComponentFileName(store + L"dlssg\\versions\\1\\files\\170_E658703.bin") ==
          L"nvngx_dlssg.dll");
    // Case is NGX's to choose, and it varies within one path.
    CHECK(ComponentFileName(L"c:\\programdata\\nvidia\\NGX\\Models\\SL_Common_0\\v\\170_E.dll") ==
          L"sl.common.dll");
    // A component the engine does not act on keeps its own name, and so does
    // every module outside the store.
    CHECK(ComponentFileName(store + L"sl_reflex_0\\versions\\1\\files\\170_E658703.dll") ==
          L"170_E658703.dll");
    CHECK(ComponentFileName(L"C:\\Game\\Binaries\\Win64\\sl.common.dll") == L"sl.common.dll");
    CHECK(ComponentFileName(L"sl.dlss_g.dll") == L"sl.dlss_g.dll");
    CHECK(ComponentFileName(L"").empty());
    // The engine's own module is a component of nothing.
    CHECK(ComponentFileName(L"C:\\Game\\version.dll") == L"version.dll");
}

void TestText() {
    const std::wstring wide = L"C:\\Users\\Jos\u00e9\\\u30b2\u30fc\u30e0";
    CHECK(odg::text::FromUtf8(odg::text::ToUtf8(wide)) == wide);
    CHECK(odg::text::ToUtf8(L"").empty());
}

} // namespace

int main() {
    TestConfig();
    TestShippedIni();
    TestCanRun();
    TestRetarget();
    TestPtxText();
    TestPtxSource();
    TestCubin();
    TestX86();
    TestCubinParams();
    TestText();
    TestComponentName();
    TestPathRedaction();
    TestLogLevels();
    TestConditions();
    TestLogPruning();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
