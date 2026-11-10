// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/settings.h"
#include "video_core/cache_types.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/gpu.h"
#include "video_core/query_cache/types.h"
#include "video_core/rasterizer_download_area.h"

namespace Core {
class System;
}

namespace Tegra {
class MemoryManager;
namespace Engines {
class AccelerateDMAInterface;
}
namespace Control {
struct ChannelState;
}
namespace Texture {
struct TICEntry;
}
} // namespace Tegra

namespace VideoCore {

enum class LoadCallbackStage {
    Prepare,
    Build,
    Complete,
};
using DiskResourceLoadCallback = std::function<void(LoadCallbackStage, std::size_t, std::size_t)>;

/// Set while the query cache signals the fence that writes a fence-flagged report, so the fence
/// manager makes that fence even when it is empty (FenceManager::SignalFence).
inline thread_local bool g_signaling_query_report_fence = false;

class RasterizerInterface {
public:
    virtual ~RasterizerInterface() = default;

    /// Dispatches a draw invocation
    virtual void Draw(bool is_indexed, u32 instance_count) = 0;

    /// Dispatches an indirect draw invocation
    virtual void DrawIndirect() {}

    /// Dispatches an draw texture invocation
    virtual void DrawTexture() = 0;

    /// Clear the current framebuffer
    virtual void Clear(u32 layer_count) = 0;

    /// Dispatches a compute shader invocation
    virtual void DispatchCompute() = 0;

    /// Resets the counter of a query
    virtual void ResetCounter(VideoCommon::QueryType type) = 0;

    /// Records a GPU query and caches it
    virtual void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                       VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) = 0;

    /// Signal an uniform buffer binding
    virtual void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                           u32 size) = 0;

    /// Signal disabling of a uniform buffer
    virtual void DisableGraphicsUniformBuffer(size_t stage, u32 index) = 0;

    /// Signal a GPU based semaphore as a fence
    virtual void SignalFence(std::function<void()>&& func) = 0;

    /// Send an operation to be done after a certain amount of flushes.
    virtual void SyncOperation(std::function<void()>&& func) = 0;

    /// Signal a GPU based syncpoint as a fence
    virtual void SignalSyncPoint(u32 value) = 0;

    /// Signal a GPU based reference as point
    virtual void SignalReference() = 0;

    /// Release all pending fences.
    virtual void ReleaseFences(bool force = true) = 0;

    /// Any thread: changes whenever a fence is made or a fence operation queued (0 when the
    /// backend doesn't track it).
    [[nodiscard]] virtual u64 FenceWorkEpoch() const {
        return 0;
    }

    /// Writes the region's query reports to guest memory, waiting for pending host results.
    virtual void FlushQueryResults(DAddr addr, u64 size) {
        FlushRegion(addr, size, VideoCommon::CacheType::QueryCache);
    }

    /// Notify rasterizer that all caches should be flushed to Switch memory
    virtual void FlushAll() = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    virtual void FlushRegion(DAddr addr, u64 size,
                             VideoCommon::CacheType which = VideoCommon::CacheType::All) = 0;

    /// EDEN_NX2_ASYNC_READBACK: FlushRegion for a guest CPU read of the region (GPU::OnCPURead),
    /// on the GPU thread. A cache may submit its downloads without waiting for the host GPU, for
    /// FinishRead to write.
    virtual void FlushRegionForRead(DAddr addr, u64 size) {
        FlushRegion(addr, size);
    }

    /// Writes to guest memory what FlushRegionForRead submitted over the region (and anything
    /// submitted before it), waiting for the host GPU if it must. Any thread: the reading one
    /// before it reads, and the GPU's side before it writes guest memory there.
    virtual void FinishRead([[maybe_unused]] DAddr addr, [[maybe_unused]] u64 size) {}

    /// Buffer flushes between these calls share one wait for the GPU, at EndFlushBatch.
    virtual void BeginFlushBatch() {}
    virtual void EndFlushBatch() {}
    /// Switch 2, from the GPU thread's front: where a GPU-written command list's words are.
    enum class GpuWrittenList {
        Unknown, ///< Neither: the caller drains, flushes the page and notes it synced
        Copied,  ///< `out` (within one 4 KiB page) is filled from the copy made after its writer
        Synced,  ///< Guest memory holds the page: no GPU work wrote it since it was flushed
        /// Only with `pending_tick`: the copy is not done yet, `*pending_tick` is the host GPU
        /// tick that completes it. Ask again once IsGpuTickDone(*pending_tick).
        Pending,
    };
    /// With `pending_tick`, a copy the GPU is still making returns Pending instead of waiting.
    virtual GpuWrittenList ReadGpuWrittenCommandList([[maybe_unused]] DAddr addr,
                                                     [[maybe_unused]] std::span<u8> out,
                                                     [[maybe_unused]] u64* pending_tick = nullptr) {
        return GpuWrittenList::Unknown;
    }

    /// Whether the host GPU passed `tick`, without waiting. Asks for deferred work to be
    /// submitted when the tick is not submitted yet, so it completes.
    virtual bool IsGpuTickDone([[maybe_unused]] u64 tick) {
        return true;
    }

    /// Switch 2: the front flushed this command list page with the split drained.
    virtual void NoteGpuWrittenCommandListSynced([[maybe_unused]] DAddr page) {}

    /// Check if the the specified memory area requires flushing to CPU Memory.
    virtual bool MustFlushRegion(DAddr addr, u64 size,
                                 VideoCommon::CacheType which = VideoCommon::CacheType::All) = 0;

    virtual RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) = 0;

    /// Whether reports in the range wait for host query results not yet in guest memory.
    [[nodiscard]] virtual bool HasPendingQueries([[maybe_unused]] DAddr addr,
                                                 [[maybe_unused]] u64 size) {
        return false;
    }

    /// Whether the query cache lists a report in the range; callable from the split's front.
    [[nodiscard]] virtual bool HasCachedReports([[maybe_unused]] DAddr addr,
                                                [[maybe_unused]] u64 size) {
        return true;
    }

    /// After a CPU read trap's flush that ended at `end`: flushes GPU-written memory in
    /// [end, end + window) ahead of a sequential read, so its next traps need no wait.
    virtual void FlushAhead([[maybe_unused]] DAddr end, [[maybe_unused]] u64 window) {}

    /// After a CPU read trap's flush at `addr`: flushes GPU-written memory in the `block`-aligned
    /// block of the cached buffer holding it, so reads elsewhere in that block need no wait.
    virtual void FlushReadRegion([[maybe_unused]] DAddr addr, [[maybe_unused]] u64 block) {}

    /// Notify rasterizer that any caches of the specified region should be invalidated
    virtual void InvalidateRegion(DAddr addr, u64 size,
                                  VideoCommon::CacheType which = VideoCommon::CacheType::All) = 0;

    virtual void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
        if (!Settings::values.skip_cpu_inner_invalidation.GetValue()) {
            for (const auto& [cpu_addr, size] : sequences) {
                InvalidateRegion(cpu_addr, size);
            }
        }
    }

    /// Notify rasterizer that any caches of the specified region are desync with guest
    virtual void OnCacheInvalidation(PAddr addr, u64 size) = 0;

    /// A guest CPU write to memory the GPU may have written, held back until InvalidateGPUCache
    /// applied it: only these bytes changed.
    virtual void OnCachedWrite(DAddr addr, u64 size) {
        OnCacheInvalidation(addr, size);
    }

    /// The held writes of one InvalidateGPUCache at once, none of them empty: a backend can lock
    /// each cache once for all of them instead of once per region.
    virtual void OnCachedWrites(std::span<const std::pair<DAddr, std::size_t>> writes) {
        for (const auto& [addr, size] : writes) {
            OnCachedWrite(addr, size);
        }
    }

    virtual bool TryCacheInvalidation(DAddr addr, u64 size) {
        if (MustFlushRegion(addr, size, VideoCommon::CacheType::BufferCache)) return false;
        OnCacheInvalidation(addr, size);
        return true;
    }

    virtual bool OnCPUWrite(PAddr addr, u64 size) = 0;

    /// Sync memory between guest and host.
    virtual void InvalidateGPUCache() = 0;

    /// Unmap memory range
    virtual void UnmapMemory(DAddr addr, u64 size) = 0;

    /// Remap GPU memory range. This means underneath backing memory changed
    virtual void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    /// and invalidated
    virtual void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) = 0;

    /// Notify the host renderer to wait for previous primitive and compute operations.
    virtual void WaitForIdle() = 0;

    /// Notify the host renderer to wait for reads and writes to render targets and flush caches.
    virtual void FragmentBarrier() = 0;

    /// Notify the host renderer to make available previous render target writes.
    virtual void TiledCacheBarrier() = 0;

    /// Notify the rasterizer to send all written commands to the host GPU.
    virtual void FlushCommands() = 0;

    /// Notify rasterizer that a frame is about to finish
    virtual void TickFrame() = 0;

    virtual bool AccelerateConditionalRendering() {
        return false;
    }

    /// Attempt to use a faster method to perform a surface copy
    [[nodiscard]] virtual bool AccelerateSurfaceCopy(
        const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst,
        const Tegra::Engines::Fermi2D::Config& copy_config) {
        return false;
    }

    /// NVIDIA DLSS in place of a game's DLSS evaluation: upscales src into dst with the frame's
    /// motion vectors, depth and camera jitter (in src pixels). False when it can't run; the
    /// caller then draws the stand-in.
    [[nodiscard]] virtual bool AccelerateNvidiaDlss(
        const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst,
        const Tegra::Engines::Fermi2D::Surface& motion_vectors,
        const Tegra::Texture::TICEntry& depth, std::array<f32, 2> jitter) {
        return false;
    }

    /// NVIDIA DLSS in place of Ocarina of Time's upscaler network: upscales src (the network's
    /// RGB9E5 input) into dst with the scene depth and motion from the camera and Link's pose.
    /// False when it can't run; the caller then draws the stand-in.
    [[nodiscard]] virtual bool AccelerateOcarinaDlss(const Tegra::Engines::Fermi2D::Surface& src,
                                                     const Tegra::Engines::Fermi2D::Surface& dst) {
        return false;
    }

    /// Whether NVIDIA DLSS (the "Use NVIDIA DLSS" setting) would draw a game's DLSS evaluation.
    [[nodiscard]] virtual bool UsesNvidiaDlss() const {
        return false;
    }

    [[nodiscard]] virtual Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() = 0;

    virtual void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                          std::span<const u8> memory) = 0;

    /// Initialize disk cached resources for the game being emulated
    virtual void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                   const DiskResourceLoadCallback& callback) {}

    /// Prepare Shaders with EDEN_NX2_PRECOMPILE_COMPUTE or EDEN_NX2_PRECOMPILE_GRAPHICS: scans the
    /// running game's files for native programs on a background thread, and builds them at the
    /// end of LoadDiskResources, so call it first. Does nothing otherwise.
    virtual void PrecompileNativePrograms(Core::System& system) {}

    virtual void InitializeChannel(Tegra::Control::ChannelState& channel) {}

    virtual void BindChannel(Tegra::Control::ChannelState& channel) {}

    virtual void ReleaseChannel(s32 channel_id) {}

    /// Register the address as a Transform Feedback Object
    virtual void RegisterTransformFeedback(GPUVAddr tfb_object_addr) {}

    /// Returns true when the rasterizer has Draw Transform Feedback capabilities
    virtual bool HasDrawTransformFeedback() {
        return false;
    }
};
} // namespace VideoCore
