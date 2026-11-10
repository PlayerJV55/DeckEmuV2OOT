// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <optional>
#include <string_view>
#include <type_traits>

#include "common/common_types.h"

// Per-title defaults of EDEN_NX2_* switches. A switch read through Setting or Default takes its
// value from the environment (or a switch file) first, then from the table below for the running
// program, then its global default. EDEN_NX2_TITLE_DEFAULTS=0 ignores the table (A/B tests).
// The running program changes between emulation sessions in one GUI process, and when the home
// menu starts a game: a value that depends on it must be read again then (SessionValue does),
// never kept in a plain static.
namespace Common::Nx2 {

inline constexpr u64 OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;
inline constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
inline constexpr u64 FireEmblemFortunesWeaveProgramId = 0x04007EF01F8AE000ULL;
inline constexpr u64 TearsOfTheKingdomProgramId = 0x0400F2C0115B6001ULL;
inline constexpr u64 SplatoonRaidersProgramId = 0x0400380020056000ULL;
inline constexpr u64 XenobladeDEProgramId = 0x0400FF500E34A001ULL;
inline constexpr u64 Xenoblade2ProgramId = 0x0400E95004038001ULL;
inline constexpr u64 XenobladeXProgramId = 0x0400453019AA8001ULL;

struct TitleDefault {
    u64 program_id;
    std::string_view name;
    std::string_view value;
};

inline constexpr std::array TitleDefaults{
    TitleDefault{MarioKartWorldProgramId, "EDEN_NX2_AMD_PARALLEL_PIPELINES", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_AMD_PIPELINE_WORKERS", "3"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_COMPUTE_NOINLINE", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_COMPUTE_TABLE_256", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_DEFER_CONDITIONAL", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_FENCE_DEDUP", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_FENCE_REPORTS_AFTER_GPU", "1"},
    TitleDefault{MarioKartWorldProgramId, "EDEN_NX2_FLUSH_DRAWS", "128"},
    // Outdoors in Kokiri Forest: 38 ms a frame instead of 43 (nemyu-coco, i7-12650HX, RTX 4050
    // Laptop).
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_FLUSH_DRAWS", "128"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_GRAPHICS_NOINLINE", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_GPU_TIMESTAMP", "ns"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_INDIRECT_DISPATCH", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_INVALIDATE_BLOCKS", "0"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_NATIVE_SSBO_MAPPED", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_NATIVE_SYNC_BUDGET_US", "50000"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_NATIVE_SYNC_WINDOW_LENGTH_US", "250000"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_NATIVE_SYNC_WINDOW_US", "100000"},
    TitleDefault{MarioKartWorldProgramId, "EDEN_NX2_NATIVE_SSBO_MAPPED", "0"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_NATIVE_UBO_REUPLOAD", "2"},
    TitleDefault{SplatoonRaidersProgramId, "EDEN_NX2_NO_RESCALE_ABOVE", "4096"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_PERF_ON", "async_pipelines"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_PREFLUSH_READ_WAIT", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_QUERY_RESULT_WAIT", "1"},
    TitleDefault{MarioKartWorldProgramId, "EDEN_NX2_ROMFS_READ_LOG", "1"},
    TitleDefault{MarioKartWorldProgramId, "EDEN_NX2_SKIP_GUARD", "depth"},
    TitleDefault{MarioKartWorldProgramId, "EDEN_NX2_TC_BUDGET_GUARD_MIB", "256"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_TC_GC_PRESSURE_US", "4000"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_TC_IMAGE_POOL", "256"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_TC_SLACK", "1"},
    TitleDefault{OcarinaOfTimeProgramId, "EDEN_NX2_USED_SESSIONS", "8"},
    TitleDefault{TearsOfTheKingdomProgramId, "EDEN_NX2_LEGACY_SECTOR_LAYOUT", "1"},
    // The Xenoblade Switch 2 Editions set bit 88 on every texture header; with the Switch 2 order
    // their colour-grading LUTs (pink/cyan image), font atlas and textures came out scrambled
    // (Orchid c3ce52b0f4).
    TitleDefault{XenobladeDEProgramId, "EDEN_NX2_LEGACY_SECTOR_LAYOUT", "1"},
    TitleDefault{Xenoblade2ProgramId, "EDEN_NX2_LEGACY_SECTOR_LAYOUT", "1"},
    TitleDefault{XenobladeXProgramId, "EDEN_NX2_LEGACY_SECTOR_LAYOUT", "1"},
};

/// The switches whose readers ask Setting or Default for the running program. Only these can
/// have a title default: any other switch is read once per process.
inline constexpr std::array<std::string_view, 47> SessionSwitches{
    "EDEN_NX2_ACQUIRE_QUEUED",
    "EDEN_NX2_AMD_PARALLEL_PIPELINES",
    "EDEN_NX2_AMD_PIPELINE_WORKERS",
    "EDEN_NX2_ASYNC_READBACK",
    "EDEN_NX2_BACK_MACROS",
    "EDEN_NX2_CMD_BATCH",
    "EDEN_NX2_COMPUTE_NOINLINE",
    "EDEN_NX2_COMPUTE_TABLE_256",
    "EDEN_NX2_DEFER_CONDITIONAL",
    "EDEN_NX2_DRAW_LIST_COPIES",
    "EDEN_NX2_FENCE_DEDUP",
    "EDEN_NX2_FENCE_REPORTS_AFTER_GPU",
    "EDEN_NX2_FLUSH_DRAWS",
    "EDEN_NX2_GPU_TIMESTAMP",
    "EDEN_NX2_GRAPHICS_NOINLINE",
    "EDEN_NX2_INDIRECT_DISPATCH",
    "EDEN_NX2_INVALIDATE_BLOCKS",
    "EDEN_NX2_LEGACY_SECTOR_LAYOUT",
    "EDEN_NX2_NATIVE_SSBO_MAPPED",
    "EDEN_NX2_NATIVE_SYNC_BUDGET_US",
    "EDEN_NX2_NATIVE_SYNC_PIPELINES",
    "EDEN_NX2_NATIVE_SYNC_WINDOW_LENGTH_US",
    "EDEN_NX2_NATIVE_SYNC_WINDOW_US",
    "EDEN_NX2_NATIVE_UBO_REUPLOAD",
    "EDEN_NX2_NO_RESCALE_ABOVE",
    "EDEN_NX2_PB_RESNAPSHOT_AFTER",
    "EDEN_NX2_PENDING_POLL_US",
    "EDEN_NX2_PERF_OFF",
    "EDEN_NX2_PERF_ON",
    "EDEN_NX2_PRECOMPILE_COMPUTE",
    "EDEN_NX2_PRECOMPILE_GRAPHICS",
    "EDEN_NX2_PREFLUSH",
    "EDEN_NX2_PREFLUSH_READ_WAIT",
    "EDEN_NX2_PRESENT_QUEUE",
    "EDEN_NX2_QUERY_RESULT_WAIT",
    "EDEN_NX2_READBACK_FENCE_COPY",
    "EDEN_NX2_ROMFS_READ_LOG",
    "EDEN_NX2_SKIP_GUARD",
    "EDEN_NX2_SPLIT",
    "EDEN_NX2_TC_BUDGET_GUARD_MIB",
    "EDEN_NX2_TC_COMPACT",
    "EDEN_NX2_TC_GC_PRESSURE_US",
    "EDEN_NX2_TC_IMAGE_POOL",
    "EDEN_NX2_TC_JOIN_LAYERS",
    "EDEN_NX2_TC_SKIP_STALE",
    "EDEN_NX2_TC_SLACK",
    "EDEN_NX2_USED_SESSIONS",
};

static_assert(std::ranges::all_of(TitleDefaults, [](const TitleDefault& entry) {
                  return std::ranges::find(SessionSwitches, entry.name) != SessionSwitches.end();
              }),
              "A title default for a switch that is not read per session has no effect");

/// Changes whenever the running program does (Settings::SetCurrentProgramID and
/// SetCurrentProgramIsSwitch2).
inline std::atomic<u32> g_program_generation{1};

/// The table's value of `name` for the running program, if it has one and title defaults are on.
[[nodiscard]] std::optional<std::string_view> FindTitleDefault(std::string_view name);

/// The running program's default of `name`, else `global_default`. Ignores the environment.
[[nodiscard]] std::string_view Default(std::string_view name, std::string_view global_default);

/// The environment's value of `name` if it is set, else Default. Reads the environment: on hot
/// paths, keep the result in a SessionValue.
[[nodiscard]] std::string_view Setting(const char* name, std::string_view global_default);

/// A value worked out from switches for the running program: `compute` runs again on the first
/// Get after the program changed, and other Gets cost two atomic loads.
template <typename T>
class SessionValue {
    static_assert(std::is_trivially_copyable_v<T> &&
                  (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4));

public:
    explicit SessionValue(T (*compute_)()) : compute{compute_} {}

    [[nodiscard]] T Get() {
        const u64 generation = g_program_generation.load(std::memory_order_acquire);
        u64 cached = cache.load(std::memory_order_acquire);
        if ((cached >> 32) != generation) {
            cached = (generation << 32) | std::bit_cast<Bits>(compute());
            cache.store(cached, std::memory_order_release);
        }
        return std::bit_cast<T>(static_cast<Bits>(cached));
    }

private:
    using Bits =
        std::conditional_t<sizeof(T) == 1, u8, std::conditional_t<sizeof(T) == 2, u16, u32>>;

    T (*compute)();
    /// The generation it was computed for in the high word, the value's bits in the low one.
    std::atomic<u64> cache{};
};

} // namespace Common::Nx2
