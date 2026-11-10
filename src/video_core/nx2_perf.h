// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/nx2_title_defaults.h"

// GPU-thread optimizations from Lichii's Ocarina of Time work, with its switch names so the same
// settings carry over. EDEN_NX2_PERF_OPT=0 turns every one off for an A/B; EDEN_NX2_PERF_OFF=
// <name>,... turns off those named and EDEN_NX2_PERF_ON=<name>,... turns on those that are off by
// default (names below). EDEN_NX2_PERF_VERIFY=1 computes the answers the old way too where an
// optimization supports it, logs any difference and keeps the old answer. The state is worked out
// and logged for each program, with its title defaults (nx2_title_defaults.h).
namespace VideoCommon::Nx2Perf {

enum class Opt : unsigned {
    Method,            ///< Maxwell3D fast method dispatch on the split back
    CbBatch,           ///< Consecutive constant buffer words uploaded in one write
    IndexRebind,       ///< Repeated vkCmdBindIndexBuffer skipped
    UsageTracker,      ///< UsageTracker::Track skips pages known to be used
    SortedPageBatches, ///< Sorted, separate cached page runs taken without a copy and sort
    FragmentBarrier,   ///< FragmentBarrier deferred to the next draw that reads the pass
    UnswizzleWorkers,  ///< Large odd-pitch 2D layers unswizzled half on a texture worker
    AsyncPipelines,    ///< Native graphics SPIR-V emitted by the pipeline build, not the GPU thread
    SsboBindings,      ///< Storage buffer bindings kept until the page table changes
    NativeBankUpload,  ///< Native 64 KiB uniform banks uploaded again without marking them
    SplitOverlap,      ///< Split front MME DMA reads wait only for the work that can write them.
                       ///< Unsafe, kept off: Lichii removed its version (#184, 1bb45da83d)
    ForwardReads,      ///< Split front MME DMA reads take the words of a queued uniform upload
    FrontSleep,        ///< The split front sleeps instead of yielding while it waits long
    Count,
};

inline constexpr std::array<std::string_view, static_cast<size_t>(Opt::Count)> opt_names{
    "method",           "cb_batch",         "index_rebind",      "usage_tracker",
    "sorted_page_batches", "fragment_barrier", "unswizzle_workers", "async_pipelines",
    "ssbo_bindings",    "native_bank_upload", "split_overlap",    "forward_reads",
    "front_sleep",
};

/// Lichii's names for its optimizations Nemyu doesn't have (or has under its own switches):
/// accepted in the lists so a Lichii switch file loads without warnings, and ignored.
inline constexpr std::array<std::string_view, 13> other_lichii_names{
    "bounds",          "clean_words",       "bank_check",
    "submit_budget",   "fence_flush",       "frames_ahead",
    "pipeline_retention", "write_intersections", "qmd_scratch",      "query_diagnostics",
    "immediate_callbacks", "query_upload_scratch", "descriptor_workers",
};

/// On without EDEN_NX2_PERF_ON: the ones equivalent to the old code.
inline constexpr unsigned default_on = (1U << static_cast<unsigned>(Opt::Method)) |
                                       (1U << static_cast<unsigned>(Opt::CbBatch)) |
                                       (1U << static_cast<unsigned>(Opt::IndexRebind)) |
                                       (1U << static_cast<unsigned>(Opt::UsageTracker)) |
                                       (1U << static_cast<unsigned>(Opt::SortedPageBatches)) |
                                       (1U << static_cast<unsigned>(Opt::UnswizzleWorkers)) |
                                       (1U << static_cast<unsigned>(Opt::SsboBindings)) |
                                       (1U << static_cast<unsigned>(Opt::NativeBankUpload));

namespace Detail {
inline unsigned ParseNames(std::string_view list, std::string_view source) {
    unsigned mask = 0;
    while (!list.empty()) {
        const size_t comma = list.find(',');
        const std::string_view name = list.substr(0, comma);
        bool known = false;
        for (size_t i = 0; i < opt_names.size(); ++i) {
            if (name == opt_names[i]) {
                mask |= 1U << i;
                known = true;
            }
        }
        for (const std::string_view other : other_lichii_names) {
            known |= name == other;
        }
        if (!known && !name.empty()) {
            LOG_WARNING(HW_GPU, "Nx2Perf: unknown name {} in {}", name, source);
        }
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    return mask;
}

inline unsigned ParseVariable(const char* variable) {
    const char* const value = std::getenv(variable);
    return ParseNames(value != nullptr ? value : "", variable);
}

/// The running title's defaults (nx2_title_defaults.h) take from and add to default_on; the
/// environment's lists apply on top of them.
inline unsigned ComputeMask() {
    const char* const master = std::getenv("EDEN_NX2_PERF_OPT");
    unsigned result = 0;
    if (master == nullptr || master[0] != '0') {
        // Selevet's EDEN_NX2_DMA_RANGES=1 turns on split_overlap too.
        const char* const dma_ranges = std::getenv("EDEN_NX2_DMA_RANGES");
        const unsigned selevet = dma_ranges != nullptr && dma_ranges[0] == '1'
                                     ? 1U << static_cast<unsigned>(Opt::SplitOverlap)
                                     : 0U;
        const unsigned title_on = ParseNames(Common::Nx2::Default("EDEN_NX2_PERF_ON", ""),
                                             "the title's EDEN_NX2_PERF_ON");
        const unsigned title_off = ParseNames(Common::Nx2::Default("EDEN_NX2_PERF_OFF", ""),
                                              "the title's EDEN_NX2_PERF_OFF");
        result = ((default_on & ~title_off) | title_on | selevet |
                  ParseVariable("EDEN_NX2_PERF_ON")) &
                 ~ParseVariable("EDEN_NX2_PERF_OFF");
    }
    std::string state;
    for (size_t i = 0; i < opt_names.size(); ++i) {
        state += fmt::format(" {}={}", opt_names[i], (result >> i & 1) != 0 ? "on" : "off");
    }
    LOG_INFO(HW_GPU, "Nx2Perf:{}", state);
    return result;
}

/// Worked out again for each program (logged each time).
inline unsigned Mask() {
    static Common::Nx2::SessionValue<unsigned> mask{ComputeMask};
    return mask.Get();
}
} // namespace Detail

inline bool On(Opt opt) {
    return (Detail::Mask() >> static_cast<unsigned>(opt) & 1) != 0;
}

inline bool Verify() {
    static const bool verify = [] {
        const char* const value = std::getenv("EDEN_NX2_PERF_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

/// The frame period the game asks for (nx2:vi2 present word 12 at the speed limit), 0 while
/// unknown or unpaced. The host frame pacer (vk_host_pacer.h) never holds frames longer.
inline std::atomic<long long> g_guest_frame_interval_ns{0};

/// The display refresh period while the host frame pacer is on, else 0. The game's present
/// pacing (nx2_gsv.h) locks its frame period to whole refreshes of it (genlock).
inline std::atomic<long long> g_host_refresh_ns{0};

} // namespace VideoCommon::Nx2Perf
