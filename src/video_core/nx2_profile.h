// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_set>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/cpu_features.h"

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    ForcedFences, ///< ReleaseFences(true): waits for the host GPU to pass the fences made so far
    DeferredCall, ///< Split GPU thread: functions the front queued for the back
    SyncRequests, ///< GPU::TickWork: sync operations (the flushes of CPU read traps)
    FrameTick,    ///< RasterizerVulkan::TickFrame, with the texture cache's collector
    Dma,          ///< MaxwellDMA::Launch
    Blit2D,       ///< Fermi2D::Blit
    DmaReadFlush, ///< Split back: the cache flushes before a front MME DMA read
    QueryWait,    ///< FlushQueryResults: waiting for the host GPU's query results
    BackSplitJoin, ///< EDEN_NX2_BACK_SPLIT: joins of the pool's jobs, those run here included
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait", "forced fences",
    "deferred call", "sync requests", "frame tick", "dma", "2d blit", "dma read flush",
    "query wait", "back split join",
};

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,  ///< Buffer data copied back from the host GPU at fences
    DownloadCopies, ///< Buffer ranges copied back
    DmaReadBytes,   ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,   ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites, ///< Of MethodWrites: the register already held the value
    AcquireChecks,  ///< Semaphore acquires evaluated
    AcquireWaits,   ///< Of AcquireChecks: not met yet (the channel waits)
    AcquiresMetInMemory,    ///< Of AcquireChecks: acquire-gequals already met in guest memory,
                            ///< which go on without a drain with EDEN_NX2_ACQUIRE_PEEK=1
                            ///< (without it they also count as met after the drain)
    AcquiresMetQueued,      ///< Of AcquireChecks: met by a release still queued for the back,
                            ///< without a drain (EDEN_NX2_ACQUIRE_QUEUED)
    AcquiresMetAfterDrain,  ///< Of AcquireChecks: met once the split's back caught up
    AcquiresMetAfterFences, ///< Of AcquireChecks: met after the forced fence release
    SemExecuteAcquires,     ///< Ampere SEM_EXECUTE acquires
    IndirectEntries,    ///< Command lists on GPU-written pages drawn indirectly, without a drain
    CommandListFlushes, ///< Command lists on GPU-written pages downloaded (IsMemoryDirty)
    UniformStreamBinds,  ///< Graphics uniform buffers bound through the stream buffer
    UniformStreamReuses, ///< Of UniformStreamBinds: bound again from the same command buffer
    UboInPlaceSame,      ///< EDEN_NX2_UBO_INPLACE: compared in guest memory, unchanged
    UboInPlaceChanged,   ///< EDEN_NX2_UBO_INPLACE: compared in guest memory, changed
    LayoutLookups,      ///< MemoryManager::GetMemoryLayoutSize calls (storage buffer sizes)
    LayoutRunHits,      ///< Of LayoutLookups: answered from the run the last one fell in
    RunCacheHits,       ///< MaxContinuousRange answered from the run through the page
    RunCacheMisses,     ///< MaxContinuousRange walked a page's run anew
    RunCacheExtends,    ///< MaxContinuousRange walked a page's run further than before
    RunCacheFallbacks,  ///< MaxContinuousRange walked from the address (the run ends before it)
    PageTableChanges,   ///< GPU page table changes seen by MaxContinuousRange on this thread
    TrackingChanges,    ///< Buffer cache calls that start or stop CPU tracking of pages
    InvalidatedRanges,  ///< Ranges FlushCaching hands the caches (constant buffer, inline data)
    InvalidationsDropped, ///< Writes FlushCaching left out: a waiting range covered them
    UploadSplits,        ///< Render passes ended by the barrier after buffer uploads
    UploadSplitsAvoided, ///< Of those barriers: skipped (EDEN_NX2_UPLOAD_BARRIER=1 records them)
    EmptyDrawsSkipped,   ///< Direct draws of no vertices or instances (EDEN_NX2_SKIP_EMPTY_DRAWS)
    DmaReadsForwarded,   ///< MME DMA reads served by a queued uniform upload (forward_reads)
    BackMacros,          ///< Split back: macro calls it ran for the front (EDEN_NX2_BACK_MACROS)
    Count,
};

/// GpuSplit::DrainSite names, in order: why the split's front waited for its back.
inline constexpr std::array<const char*, 17> drain_site_names{
    "other", "commands", "acquire", "acquire resume", "SEM_EXECUTE", "MME DMA read", "queued GMMU",
    "parameters", "sync fence", "semaphore read", "release fences", "front start",
    "front verify", "ring full", "acquire writers", "back register", "back code",
};

// Read once, when the GPU thread starts (Initialize): Enabled() runs several times per method
// write (no guard to check). Not at static initialization, which runs before main() applies
// nx2_switches.txt.
inline std::atomic<bool> g_enabled{false};

inline void Initialize() {
    g_enabled.store(std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr, std::memory_order_relaxed);
}

inline bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, static_cast<size_t>(Counter::Count)> counters{};
    std::array<u64, drain_site_names.size()> drain_calls{};
    std::array<u64, drain_site_names.size()> drain_waits{}; ///< Of drain_calls: the back was behind
    std::array<u64, drain_site_names.size()> drain_ns{};    ///< Time the front waited
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : zone{zone_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Now();
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 elapsed = Now() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        data.exclusive_ns[index] += elapsed - data.child_ns;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    bool enabled;
    u64 start{};
    u64 parent_child_ns{};
};

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

/// A GpuSplit::Drain call from `site` (a GpuSplit::DrainSite).
inline void CountDrain(size_t site) {
    if (Enabled()) {
        ++t_data.drain_calls[site];
    }
}

/// One of them waited `ns` for the back to catch up.
inline void AddDrainWait(size_t site, u64 ns) {
    if (Enabled()) {
        ++t_data.drain_waits[site];
        t_data.drain_ns[site] += ns;
    }
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s.
inline void Report(const char* thread_name = "GPU thread") {
    if (!Enabled()) {
        return;
    }
    auto& data = t_data;
    const u64 now = Now();
    if (data.window_start == 0) {
        data.window_start = now;
        data.child_ns = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window > zoned ? window - zoned : 0;
    std::string line;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / window;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / window;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
    }
    line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                        "MME DMA reads) | MME DMA reads {:.0f} KiB",
                        static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[1]) * 1e9 / window,
                        static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / window);
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                        static_cast<double>(data.counters[4]) * 1e9 / window,
                        static_cast<double>(data.counters[5]) * 1e9 / window);
    const auto per_second = [&data, window](Counter counter) {
        return static_cast<double>(data.counters[static_cast<size_t>(counter)]) * 1e9 / window;
    };
    line += fmt::format(" | acquires {:.0f} ({:.0f} met in memory, {:.0f} by queued releases, "
                        "{:.0f} after the drain, {:.0f} after the fences, {:.0f} waited), "
                        "SEM_EXECUTE {:.0f}",
                        per_second(Counter::AcquireChecks), per_second(Counter::AcquiresMetInMemory),
                        per_second(Counter::AcquiresMetQueued),
                        per_second(Counter::AcquiresMetAfterDrain),
                        per_second(Counter::AcquiresMetAfterFences),
                        per_second(Counter::AcquireWaits), per_second(Counter::SemExecuteAcquires));
    line += fmt::format(" | GPU-written command lists {:.0f} drawn indirectly, {:.0f} downloaded",
                        per_second(Counter::IndirectEntries),
                        per_second(Counter::CommandListFlushes));
    line += fmt::format(" | streamed uniform buffers {:.0f} ({:.0f} reused; compared in place "
                        "{:.0f} unchanged, {:.0f} changed)",
                        per_second(Counter::UniformStreamBinds),
                        per_second(Counter::UniformStreamReuses),
                        per_second(Counter::UboInPlaceSame),
                        per_second(Counter::UboInPlaceChanged));
    line += fmt::format(" | layout size lookups {:.0f} ({:.0f} from the last run)",
                        per_second(Counter::LayoutLookups), per_second(Counter::LayoutRunHits));
    line += fmt::format(" | continuous range runs {:.0f} hits, {:.0f} misses, {:.0f} extends, "
                        "{:.0f} fallbacks, {:.0f} page table changes",
                        per_second(Counter::RunCacheHits), per_second(Counter::RunCacheMisses),
                        per_second(Counter::RunCacheExtends),
                        per_second(Counter::RunCacheFallbacks),
                        per_second(Counter::PageTableChanges));
    line += fmt::format(" | page tracking changes {:.0f}", per_second(Counter::TrackingChanges));
    line += fmt::format(" | deferred invalidations {:.0f} ranges ({:.0f} writes inside a "
                        "waiting one)",
                        per_second(Counter::InvalidatedRanges),
                        per_second(Counter::InvalidationsDropped));
    line += fmt::format(" | render passes ended by upload barriers {:.0f}, avoided {:.0f}",
                        per_second(Counter::UploadSplits),
                        per_second(Counter::UploadSplitsAvoided));
    line += fmt::format(" | empty draws skipped {:.0f}", per_second(Counter::EmptyDrawsSkipped));
    line += fmt::format(" | MME DMA reads from queued uploads {:.0f}",
                        per_second(Counter::DmaReadsForwarded));
    line += fmt::format(" | macros run for the split's front {:.0f}",
                        per_second(Counter::BackMacros));
    std::string drains;
    for (size_t i = 0; i < drain_site_names.size(); ++i) {
        if (data.drain_calls[i] == 0) {
            continue;
        }
        drains += fmt::format("{} {} {:.0f} ({:.0f} waited, {:.1f} ms)", drains.empty() ? "" : ",",
                              drain_site_names[i],
                              static_cast<double>(data.drain_calls[i]) * 1e9 / window,
                              static_cast<double>(data.drain_waits[i]) * 1e9 / window,
                              static_cast<double>(data.drain_ns[i]) * 1000.0 / window);
    }
    if (!drains.empty()) {
        line += fmt::format(" | drains:{}", drains);
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    data = {};
    data.window_start = now;
}

} // namespace VideoCommon::Nx2Profile
