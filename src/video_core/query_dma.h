// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>

#include <fmt/ranges.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_title_defaults.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon::QueryDma {

/// EDEN_NX2_QUERY_DMA_TRACE=1: logs MME DMA reads of reports whose host query results were not in
/// guest memory when the read came. Counter reports are written when a later fence pops unless
/// the read flushes them first (EDEN_NX2_DMA_READ_QUERY_FLUSH, on by default).
inline bool TraceEnabled() {
    static const bool enabled = std::getenv("EDEN_NX2_QUERY_DMA_TRACE") != nullptr;
    return enabled;
}

/// Whether [source, source + size) holds reports still waiting for host query results. Runs
/// where the rasterizer's caches are owned (the back of a split GPU thread).
inline bool HasPendingQueries(const Tegra::MemoryManager& memory,
                              VideoCore::RasterizerInterface& rasterizer, GPUVAddr source,
                              size_t size) {
    constexpr GPUVAddr page_size = 0x1000;
    const GPUVAddr end = source + size;
    for (GPUVAddr begin = source; begin < end;) {
        const GPUVAddr page_end = (std::min)((begin & ~(page_size - 1)) + page_size, end);
        if (const auto device = memory.GpuToCpuAddress(begin);
            device && rasterizer.HasPendingQueries(*device, page_end - begin)) {
            return true;
        }
        begin = page_end;
    }
    return false;
}

/// EDEN_NX2_QUERY_RESULT_WAIT=1: the query flush before an MME DMA read also waits for the reports
/// still waiting for host results. Below High GPU accuracy the flush returns before they are
/// written, so the macro reads an old occlusion count (Ocarina of Time's foliage and terrain
/// flash overexposed for a few frames when the camera moves). On for Ocarina of Time.
inline bool WaitEnabled() {
    static Common::Nx2::SessionValue<bool> enabled{[] {
        return Common::Nx2::Setting("EDEN_NX2_QUERY_RESULT_WAIT", "0").starts_with('1');
    }};
    return enabled.Get();
}

/// Writes the reports in [source, source + size) to guest memory, waiting (a bounded time) for
/// pending host results. Runs where the rasterizer's caches are owned.
inline void FlushPending(const Tegra::MemoryManager& memory,
                         VideoCore::RasterizerInterface& rasterizer, GPUVAddr source, size_t size) {
    constexpr GPUVAddr page_size = 0x1000;
    const GPUVAddr end = source + size;
    for (GPUVAddr begin = source; begin < end;) {
        const GPUVAddr page_end = (std::min)((begin & ~(page_size - 1)) + page_size, end);
        if (const auto device = memory.GpuToCpuAddress(begin)) {
            rasterizer.FlushQueryResults(*device, page_end - begin);
        }
        begin = page_end;
    }
}

/// Logs a read of reports that were pending, with the words the macro reads (after the query
/// flush, if any), and the same words once the next fence has popped.
inline void Trace(const Tegra::MemoryManager& memory, VideoCore::RasterizerInterface& rasterizer,
                  u32 macro, u32 method, GPUVAddr source, size_t size, bool flushed) {
    static constexpr u64 max_reads = 4000;
    static std::atomic<u64> reads{};
    const u64 read = reads.fetch_add(1, std::memory_order_relaxed) + 1;
    if (read > max_reads) {
        if (read % 10000 == 0) {
            LOG_INFO(HW_GPU, "Query DMA read of pending reports ({} so far)", read);
        }
        return;
    }
    std::array<u32, 4> words{};
    const size_t bytes = (std::min)(size, sizeof(words));
    memory.ReadBlockUnsafe(source, words.data(), bytes);
    LOG_INFO(HW_GPU,
             "Query DMA read #{} of pending reports: macro={:#x} method={:#x} gpu={:#x} "
             "bytes={:#x} queries flushed={} words={:08x}",
             read, macro, method, source, size, flushed, fmt::join(words, ","));
    rasterizer.SyncOperation([&memory, source, bytes, read] {
        std::array<u32, 4> landed{};
        memory.ReadBlockUnsafe(source, landed.data(), bytes);
        LOG_INFO(HW_GPU, "Query DMA read #{} landed words={:08x}", read, fmt::join(landed, ","));
    });
}

} // namespace VideoCommon::QueryDma
