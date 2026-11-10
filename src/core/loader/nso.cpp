// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_funcs.h"
#include "common/cpu_features.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/loader/nso_compatibility.h"
#include "core/memory.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

namespace Loader {
namespace {
struct MODHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

constexpr u32 PageAlignSize(u32 size) {
    return static_cast<u32>((size + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK);
}

// EDEN_NX2_REPORT_VRR=1: the Switch 2 SDK's nn::vi::detail::DefaultDisplay::QueryVrrSupported learns
// VRR support from a display mode capability event we never signal, so it reports none. A game
// Selevet tested then assumed a fixed 60 Hz display and its speed swung with the frame rate; told
// VRR works, it timed its frames itself. The exported function (found by name, any SDK version) is
// made to store true and succeed.
void ReportVrrSupported(std::span<u8> image, std::string_view module_name) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_REPORT_VRR");
        return value != nullptr && value[0] == '1';
    }();
    if (!enabled || image.size() < 8) {
        return;
    }
    const auto read32 = [&](u64 offset) -> std::optional<u32> {
        if (offset + 4 > image.size()) {
            return std::nullopt;
        }
        u32 value;
        std::memcpy(&value, image.data() + offset, 4);
        return value;
    };
    const auto read64 = [&](u64 offset) -> std::optional<u64> {
        if (offset + 8 > image.size()) {
            return std::nullopt;
        }
        u64 value;
        std::memcpy(&value, image.data() + offset, 8);
        return value;
    };
    const auto mod0 = read32(4);
    if (!mod0 || read32(*mod0) != Common::MakeMagic('M', 'O', 'D', '0')) {
        return;
    }
    const auto dynamic_rel = read32(*mod0 + 4);
    if (!dynamic_rel) {
        return;
    }
    u64 symtab = 0, strtab = 0, strsz = 0, hash = 0;
    for (u64 entry = *mod0 + static_cast<s32>(*dynamic_rel);; entry += 16) {
        const auto tag = read64(entry), value = read64(entry + 8);
        if (!tag || !value || *tag == 0) {
            break;
        }
        switch (*tag) {
        case 4: // DT_HASH
            hash = *value;
            break;
        case 5: // DT_STRTAB
            strtab = *value;
            break;
        case 6: // DT_SYMTAB
            symtab = *value;
            break;
        case 10: // DT_STRSZ
            strsz = *value;
            break;
        default:
            break;
        }
    }
    if (symtab == 0 || strtab == 0) {
        return;
    }
    // DT_HASH gives the symbol count; with only DT_GNU_HASH (the Switch 2 SDK) the table runs up
    // to the string table, which follows it.
    std::optional<u32> count = hash != 0 ? read32(hash + 4) : std::nullopt;
    if (!count && strtab > symtab) {
        count = static_cast<u32>((strtab - symtab) / 24);
    }
    if (!count) {
        return;
    }
    constexpr std::string_view Target = "_ZNK2nn2vi6detail14DefaultDisplay17QueryVrrSupportedEPb";
    for (u32 index = 0; index < *count; ++index) {
        const u64 symbol = symtab + u64{index} * 24;
        const auto name_offset = read32(symbol);
        const auto value = read64(symbol + 8);
        if (!name_offset || !value || *value == 0 || *name_offset + Target.size() >= strsz ||
            strtab + *name_offset + Target.size() >= image.size()) {
            continue;
        }
        const auto* name = reinterpret_cast<const char*>(image.data() + strtab + *name_offset);
        if (std::string_view{name, Target.size()} != Target || name[Target.size()] != '\0') {
            continue;
        }
        // mov w8, #1; strb w8, [x1]; mov w0, #0; ret
        static constexpr std::array<u32, 4> Code{0x52800028, 0x39000028, 0x52800000, 0xd65f03c0};
        if (*value + sizeof(Code) > image.size()) {
            return;
        }
        std::memcpy(image.data() + *value, Code.data(), sizeof(Code));
        LOG_INFO(Loader, "{}: QueryVrrSupported at {:#x} now reports VRR support", module_name,
                 *value);
        return;
    }
}

// EDEN_NX2_STARFOX_FPS=30..60: Star Fox's output rate. Its frame loop branches to an explicit-rate
// path that sets the present interval and the render delta together; the patch takes that path
// with this rate, leaving simulation timing and the guest clock alone. Only the main module of
// build A7730D0F74D168D90422354C8317F76FF78AD938 is patched, and only if the three replaced
// instructions match. Tested at 60.
void PatchStarFoxOutputRate(std::span<u8> image, const NSOHeader& header) {
    static const u32 fps = []() -> u32 {
        const char* value = std::getenv("EDEN_NX2_STARFOX_FPS");
        if (value == nullptr) {
            return 0;
        }
        const std::string_view text{value};
        u32 parsed{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (error != std::errc{} || end != text.data() + text.size() || parsed < 30 || parsed > 60) {
            LOG_WARNING(Loader, "EDEN_NX2_STARFOX_FPS={} is not a rate from 30 to 60", value);
            return 0;
        }
        return parsed;
    }();
    if (fps == 0) {
        return;
    }
    constexpr size_t Offset = 0xB444ADC;
    constexpr std::array<u32, 3> Original{0xBD4223E1, 0x1E202028, 0x540000ED};
    constexpr std::array<u8, 20> BuildId{0xA7, 0x73, 0x0D, 0x0F, 0x74, 0xD1, 0x68,
                                         0xD9, 0x04, 0x22, 0x35, 0x4C, 0x83, 0x17,
                                         0xF7, 0x6F, 0xF7, 0x8A, 0xD9, 0x38};
    const auto& text = header.segments[0];
    if (!std::equal(BuildId.begin(), BuildId.end(), header.build_id.begin()) ||
        Offset < text.location || Offset + sizeof(Original) > u64{text.location} + text.size ||
        Offset + sizeof(Original) > image.size() ||
        std::memcmp(image.data() + Offset, Original.data(), sizeof(Original)) != 0) {
        LOG_WARNING(Loader, "Star Fox output rate not patched: another build, or the code differs");
        return;
    }
    const std::array<u32, 3> replacement{
        0x52800019U | (fps << 5), // mov w25, #fps
        0x1E2E1000,               // fmov s0, #1.0
        0x14000035,               // b main+0xB444BB8 (the explicit-rate path)
    };
    std::memcpy(image.data() + Offset, replacement.data(), sizeof(replacement));
    LOG_INFO(Loader, "Star Fox output rate: {} FPS; the guest clock is unchanged", fps);
}

// EDEN_NX2_MKW_FPS_MODE=off|dynamic|30 (any case; 0 and lock30 too), which overrides the
// per-game setting.
std::optional<Settings::MkwFrameRate> ForcedMkwFrameRate() {
    static const std::optional<Settings::MkwFrameRate> forced =
        []() -> std::optional<Settings::MkwFrameRate> {
        const char* value = std::getenv("EDEN_NX2_MKW_FPS_MODE");
        if (value == nullptr) {
            return std::nullopt;
        }
        const std::string mode = Common::ToLower(value);
        if (mode == "off" || mode == "0") {
            return Settings::MkwFrameRate::Off;
        }
        if (mode == "dynamic") {
            return Settings::MkwFrameRate::Dynamic;
        }
        if (mode == "30" || mode == "lock30") {
            return Settings::MkwFrameRate::Lock30;
        }
        LOG_WARNING(Loader, "EDEN_NX2_MKW_FPS_MODE={} is not off, dynamic or 30; using the setting",
                    value);
        return std::nullopt;
    }();
    return forced;
}

// Whether the guest counter stops following real time at the 31.25 MHz the game converts it with:
// a CPU clock other than Normal and synced core speed off 100% scale it, EDEN_NX2_CNTFRQ changes
// its rate, and single-core timing counts emulated time. Changes of fast_cpu_time or the speed
// mode while the game runs are not caught.
bool GuestClockOffRealTime() {
    return Settings::values.cpu_clock.GetValue() != Settings::CpuClock::Normal ||
           (Settings::values.sync_core_speed.GetValue() && Settings::SpeedLimit() != 100) ||
           !Settings::values.use_multi_core.GetValue() ||
           Common::g_wall_clock.GetGuestCNTFRQ() != Common::WallClock::Switch2CNTFRQ;
}

// Mario Kart World advances its clock by its present interval in vblanks every frame, and takes
// the interval from a mode its frame loop loads: 0 = from the frame time, 1 = every vblank
// (60 fps), 2 = every other vblank (30 fps). Under emulation it counts one vblank on most frames;
// its speed depends on the intervals it picks, not only its frame rate. The mode's load must
// match once:
//   cmp w9, w1; b.ne; ldr w8, [x19, #0x210]; cbnz w8; then within 6 words mov w8, #0x42700000
// (60.0f): main+0x3A9EC8 in 1.0.0, main+0x69CC54 in 1.8.0.
// Dynamic forces mode 0. Its interval code times the frame by [x19, #0x128], the game's own CPU
// work, which stays short under emulation where the GPU thread is the slow part and would keep
// interval 1: that load is pointed at [x19, #0xe8], the whole frame. It times frames on the guest
// counter, so it is left off when that counter is off real time (ZendNevs saw the game stuck at
// 30 fps with a CPU overclock) unless EDEN_NX2_MKW_FPS_MODE=dynamic forces it. Lock30 forces
// mode 2. text: the main module's text segment. Returns the mode applied.
Settings::MkwFrameRate PatchMkwFrameRate(std::span<u8> text, size_t text_location,
                                         std::string_view build_id) {
    using Settings::MkwFrameRate;
    const std::optional<MkwFrameRate> forced = ForcedMkwFrameRate();
    const MkwFrameRate mode = forced.value_or(Settings::values.mkw_frame_rate.GetValue());
    if (mode == MkwFrameRate::Off) {
        LOG_INFO(Loader, "Mario Kart World: frame-rate mode off, as released (build id {})",
                 build_id);
        return MkwFrameRate::Off;
    }
    const bool dynamic = mode == MkwFrameRate::Dynamic;
    if (dynamic && GuestClockOffRealTime()) {
        LOG_WARNING(Loader,
                    "Mario Kart World: dynamic frame rate {}: the game times its frames on the "
                    "guest clock, which is off real time (fast_cpu_time {}, sync_core_speed {} at "
                    "{}% speed, multicore {}, counter {} Hz; build id {})",
                    forced ? "forced by EDEN_NX2_MKW_FPS_MODE, its game speed will be off"
                           : "not applied (EDEN_NX2_MKW_FPS_MODE=dynamic forces it)",
                    Settings::CanonicalizeEnum(Settings::values.cpu_clock.GetValue()),
                    Settings::values.sync_core_speed.GetValue(), Settings::SpeedLimit(),
                    Settings::values.use_multi_core.GetValue(),
                    Common::g_wall_clock.GetGuestCNTFRQ(), build_id);
        if (!forced) {
            return MkwFrameRate::Off;
        }
    }
    constexpr u32 BranchMask = 0xFF00001F;
    constexpr u32 CmpW9W1 = 0x6B01013F;
    constexpr u32 Bne = 0x54000001;
    constexpr u32 ModeLoad = 0xB9421268; // ldr w8, [x19, #0x210]
    constexpr u32 CbnzW8 = 0x35000008;
    constexpr u32 Mov60f = 0x52A84E08;        // mov w8, #0x42700000
    constexpr u32 CpuTimeLoad = 0xF9409660;   // ldr xN, [x19, #0x128], N in bits 0-4
    constexpr u32 FrameTimeLoad = 0xF9407660; // ldr xN, [x19, #0xe8]
    constexpr u32 MovW8Zero = 0x52800008;     // mov w8, #0
    constexpr u32 MovW8Two = 0x52800048;      // mov w8, #2
    const auto word = [text](size_t index) {
        u32 value;
        std::memcpy(&value, text.data() + index * sizeof(u32), sizeof(value));
        return value;
    };
    const auto put = [text](size_t index, u32 value) {
        std::memcpy(text.data() + index * sizeof(u32), &value, sizeof(value));
    };
    const size_t count = text.size() / sizeof(u32);
    size_t found = 0;
    size_t matches = 0;
    for (size_t i = 2; i + 8 < count; ++i) {
        if (word(i) != ModeLoad || word(i - 2) != CmpW9W1 ||
            (word(i - 1) & BranchMask) != Bne || (word(i + 1) & BranchMask) != CbnzW8) {
            continue;
        }
        for (size_t j = i + 2; j < i + 8; ++j) {
            if (word(j) == Mov60f) {
                found = i;
                ++matches;
                break;
            }
        }
    }
    const char* const what = dynamic ? "dynamic frame rate" : "30 fps lock";
    if (matches != 1) {
        LOG_WARNING(Loader,
                    "Mario Kart World: {} not applied, its mode load matched {} times instead of "
                    "once (another game version, or an exefs mod already changed the code; build "
                    "id {})",
                    what, matches, build_id);
        return MkwFrameRate::Off;
    }
    const size_t mode_at = text_location + found * sizeof(u32);
    if (!dynamic) {
        put(found, MovW8Two);
        LOG_INFO(Loader, "Mario Kart World: 30 fps lock on (mode load at main+{:#x}, build id {})",
                 mode_at, build_id);
        return MkwFrameRate::Lock30;
    }
    size_t frame_time = 0;
    for (size_t j = found + 1; j < found + 32 && j < count; ++j) {
        if ((word(j) & 0xFFFFFFE0) == CpuTimeLoad) {
            frame_time = j;
            break;
        }
    }
    if (frame_time == 0) {
        LOG_WARNING(Loader,
                    "Mario Kart World: dynamic frame rate not applied, no frame time load after "
                    "main+{:#x} (build id {})",
                    mode_at, build_id);
        return MkwFrameRate::Off;
    }
    put(found, MovW8Zero);
    put(frame_time, FrameTimeLoad | (word(frame_time) & 0x1F));
    LOG_INFO(Loader,
             "Mario Kart World: dynamic frame rate on (mode load at main+{:#x}, frame time load at "
             "main+{:#x}, build id {})",
             mode_at, text_location + frame_time * sizeof(u32), build_id);
    return MkwFrameRate::Dynamic;
}
} // Anonymous namespace

std::atomic<Settings::MkwFrameRate> g_mkw_frame_rate{Settings::MkwFrameRate::Off};

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const {
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {}

FileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic)) {
        return FileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic) {
        return FileType::Error;
    }

    return FileType::NSO;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Kernel::KProcess& process, Core::System& system, const FileSys::VfsFile& nso_file, VAddr load_base, bool should_pass_arguments, bool load_into_process, std::optional<FileSys::PatchManager> pm, std::vector<Core::NCE::Patcher>* patches, s32 patch_index) {
    if (nso_file.GetSize() < sizeof(NSOHeader))
        return std::nullopt;
    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header))
        return std::nullopt;
    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
        return std::nullopt;
    if (nso_header.segments.empty())
        return std::nullopt;

    // Allocate some space at the beginning if we are patching in PreText mode.
    const size_t module_start = [&]() -> size_t {
#ifdef HAS_NCE
        if (patches && load_into_process) {
            auto* patch = &patches->operator[](patch_index);
            if (patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
                return patch->GetSectionSize();
            } else if (patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
                return patch->GetPreSectionSize();
            }
        }
#endif
        return 0;
    }();

    auto const last_segment_it = &nso_header.segments[nso_header.segments.size() - 1];
    // Build program image directly in codeset memory :)
    Kernel::CodeSet codeset;
    codeset.memory.resize(module_start + last_segment_it->location + last_segment_it->size);
    {
        std::vector<u8> compressed_data(*std::ranges::max_element(nso_header.segments_compressed_size));
        std::vector<u8> decompressed_size(std::ranges::max_element(nso_header.segments, [](auto const& a, auto const& b) {
            return a.size < b.size;
        })->size);
        for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
            nso_file.Read(compressed_data.data(), nso_header.segments_compressed_size[i], nso_header.segments[i].offset);
            if (nso_header.IsSegmentCompressed(i)) {
                const u32 segment_size = nso_header.segments[i].size;
                if (nso_header.IsZBICCompressed()) {
                    // ZBIC compression
                    const int r = Common::Compression::DecompressDataZBIC(
                        std::span<u8>{decompressed_size}.first(segment_size),
                        std::span<const u8>{compressed_data}.first(nso_header.segments_compressed_size[i])
                    );
                    // A failed segment would leave the previous segment's bytes in the buffer and
                    // boot a corrupt image that hangs without presenting; refuse to load instead.
                    if (r <= 0) {
                        LOG_ERROR(Loader, "{}: segment {} failed ZBIC decompression",
                                  nso_file.GetName(), i);
                        return std::nullopt;
                    }
                    if (static_cast<u32>(r) != segment_size) {
                        LOG_WARNING(Loader, "{}: segment {} decompressed to {:#x} of {:#x} bytes",
                                    nso_file.GetName(), i, r, segment_size);
                        std::fill(decompressed_size.begin() + r,
                                  decompressed_size.begin() + segment_size, u8{0});
                    }
                } else {
                    // LZ4 compression
                    int r = Common::Compression::DecompressDataLZ4(
                        decompressed_size.data(),
                        segment_size,
                        compressed_data.data(),
                        nso_header.segments_compressed_size[i]
                    );
                    ASSERT(r == int(segment_size));
                }

                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    decompressed_size.data(),
                    nso_header.segments[i].size
                );
            } else {
                // Not compressed
                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    compressed_data.data(),
                    nso_header.segments[i].size
                );
            }
            codeset.segments[i].addr = module_start + nso_header.segments[i].location;
            codeset.segments[i].offset = module_start + nso_header.segments[i].location;
            codeset.segments[i].size = nso_header.segments[i].size;
        }
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty()) {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = codeset.memory.size();
        codeset.memory.resize(u32(codeset.memory.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(codeset.memory.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(codeset.memory.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size = PageAlignSize(u32(codeset.memory.size()) + nso_header.segments[2].bss_size);
    codeset.memory.resize(image_size);

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso)) {
        std::span<u8> patchable_section(codeset.memory.data() + module_start, codeset.memory.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }

    // EDEN_NX2_COMPAT_PLATFORM=1: a Switch 2 Edition of a Switch 1 game asks nn::pl::IsRunningOnOunce
    // whether it runs on a Switch 2, which the Switch 2 OS answers through nnCompat. Only that
    // query is answered; it grants no add-on content and selects no other program.
    if (load_into_process && !Settings::CurrentProgramIsSwitch2()) {
        static const bool compat_platform = [] {
            const char* value = std::getenv("EDEN_NX2_COMPAT_PLATFORM");
            return value != nullptr && std::string_view{value} == "1";
        }();
        if (compat_platform) {
            if (const auto offset = ApplyOuncePlatformCompatibility(
                    std::span<u8>{codeset.memory}.subspan(module_start), nso_header)) {
                LOG_INFO(Loader, "Switch 2 compatibility: IsRunningOnOunce answers true in {} at {:#x}",
                         name, *offset);
            }
        }
    }

    if (load_into_process && name == "sdk") {
        ReportVrrSupported(std::span<u8>{codeset.memory}.subspan(module_start), name);
    }

    // Once, on the pass that loads the code, after any exefs patch.
    if (load_into_process && name == "main" && process.GetProgramId() == MarioKartWorldProgramId) {
        const auto& text = nso_header.segments[0];
        const std::string mkw_build_id =
            Common::HexToString(std::span<const u8>{nso_header.build_id}.first(16));
        g_mkw_frame_rate = PatchMkwFrameRate(
            std::span<u8>{codeset.memory}.subspan(module_start + text.location, text.size),
            text.location, mkw_build_id);
    }
    if (load_into_process && name == "main" && process.GetProgramId() == StarFoxProgramId) {
        PatchStarFoxOutputRate(std::span<u8>{codeset.memory}.subspan(module_start), nso_header);
    }

#ifdef HAS_NCE
    // If we are computing the process code layout and using nce backend, patch.
    const auto& code = codeset.CodeSegment();
    auto* patch = patches ? &patches->operator[](patch_index) : nullptr;
    if (patch && !load_into_process) {
        //Set module ID using build_id from the NSO header
        patch->SetModuleID(nso_header.build_id);
        // Patch SVCs and MRS calls in the guest code
        while (!patch->PatchText(codeset.memory, code)) {
            patch = &patches->emplace_back();
            patch->SetModuleID(nso_header.build_id);  // In case the patcher is changed for big modules, the new patcher should also have the build_id
        }
    } else if (patch) {
        // Relocate code patch and copy to the program image.
        // Save size before RelocateAndCopy (which may resize)
        const size_t size_before_relocate = codeset.memory.size();
        if (patch->RelocateAndCopy(load_base, code, codeset.memory, &process.GetPostHandlers())) {
            // Update patch section.
            auto& patch_segment = codeset.PatchSegment();
            auto& post_patch_segment = codeset.PostPatchSegment();
            const auto patch_mode = patch->GetPatchMode();
            if (patch_mode == Core::NCE::PatchMode::PreText) {
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else if (patch_mode == Core::NCE::PatchMode::Split) {
                // For Split-mode, we are using pre-patch buffer at start, post-patch buffer at end
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetPreSectionSize());
                post_patch_segment.addr = size_before_relocate;
                post_patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else {
                patch_segment.addr = image_size;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            }
        }

        // Refresh image_size to take account the patch section if it was added by RelocateAndCopy
        image_size = static_cast<u32>(codeset.memory.size());
    }
#endif

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process) {
#ifdef HAS_NCE
        // Ok, so for Split mode, we need to account for pre-patch and post-patch space
        // which will be added during RelocateAndCopy in the second pass. Where it crashed
        // in Android Studio at PreText. May be a better way. Works for now.
        if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
            return load_base + patch->GetPreSectionSize() + image_size + patch->GetSectionSize();
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
            return load_base + patch->GetSectionSize() + image_size;
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PostData) {
            return load_base + image_size + patch->GetSectionSize();
        }
#endif
        return load_base + image_size;
    }

    // Apply cheats if they exist and the program has a valid title ID
    if (pm) {
        // TODO(Maufeat): Check if there is a better way to check
        if (name == "main")
            system.SetApplicationProcessBuildID(nso_header.build_id);

        const auto cheats = pm->CreateCheatList(nso_header.build_id);
        if (!cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);
        }
    }

    // Load codeset for current process
    LOG_INFO(Loader, "Module {} loaded at {:#x} (size {:#x})", nso_file.GetName(), load_base, image_size);
    process.LoadModule(system.Kernel(), std::move(codeset), load_base);
    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    if (!LoadModule(process, system, *file, base_address, true, true)) {
        return {ResultStatus::ErrorLoadingNSO, {}};
    }

    modules.insert_or_assign(base_address, file->GetName());
    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", file->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NSO::ReadNSOModules(Modules& out_modules) {
    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
