// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <array>
#include <bit>
#include <bitset>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/container/unordered_map.h"
#include <boost/container/static_vector.hpp>
#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/spin_recursive_mutex.h"
#include "common/div_ceil.h"
#include "common/literals.h"
#include "common/lru_cache.h"
#include "common/nx2_title_defaults.h"
#include "common/range_sets.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/slot_vector.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/buffer_cache/buffer_base.h"
#include "video_core/buffer_cache/geometry_ranges.h"
#include "video_core/buffer_cache/virtual_range_cache.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_async_readback.h"
#include "video_core/nx2_download_trace.h"
#include "video_core/rasterizer_download_area.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"

namespace VideoCommon {

using BufferId = Common::SlotId;

using VideoCore::Surface::PixelFormat;
using namespace Common::Literals;

#ifdef __APPLE__
constexpr u32 NUM_VERTEX_BUFFERS = 16;
#else
constexpr u32 NUM_VERTEX_BUFFERS = 32;
#endif
constexpr u32 NUM_TRANSFORM_FEEDBACK_BUFFERS = 4;
constexpr u32 NUM_GRAPHICS_UNIFORM_BUFFERS = 18;
constexpr u32 NUM_COMPUTE_UNIFORM_BUFFERS = 8;
// The shader recompiler can emit up to MAX_SSBOS descriptors per stage.
constexpr u32 NUM_STORAGE_BUFFERS = static_cast<u32>(Shader::Info::MAX_SSBOS);
static_assert(NUM_STORAGE_BUFFERS <= 64, "storage binding masks must fit u64");
constexpr u32 NUM_TEXTURE_BUFFERS = 32;
constexpr u32 NUM_STAGES = 5;

/// End of the writes a run-time selected global access can make through the buffer at base: the
/// backend assigns an address to the closest base at or below it, so writes through this buffer
/// stop before the next higher base. Bases are aligned down by up to alignment bytes where the
/// shader reads them, so a base may lie that much past the one bound. Returns size when no
/// other base is higher.
[[nodiscard]] inline u64 SelectedWriteExtent(GPUVAddr base, u64 size,
                                             std::span<const GPUVAddr> other_bases,
                                             u64 alignment) {
    u64 extent = size;
    for (const GPUVAddr other : other_bases) {
        if (other > base) {
            extent = (std::min)(extent, other - base + alignment);
        }
    }
    return extent;
}

/// EDEN_NX2_BUFFER_SKIP=1: vertex buffer lookups are kept while their registers, the channel,
/// the page table and the buffer set stay the same, the upload scan of vertex buffers no CPU
/// write reached is skipped, and each buffer's LRU entry is touched once a frame.
[[nodiscard]] inline bool Nx2BufferSkip() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_BUFFER_SKIP");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// The upload scan half of EDEN_NX2_BUFFER_SKIP on its own: a vertex range scanned since no CPU
/// write reached its pages is not scanned again. Re-scanning unchanged ranges was ~6% of the GPU
/// back thread in Donkey Kong Bananza (12,000 draws a frame, up to MBs of range each).
/// EDEN_NX2_VERTEX_SYNC_SKIP=0 scans every vertex buffer on every draw unless BUFFER_SKIP is on.
[[nodiscard]] inline bool Nx2VertexSyncSkip() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_VERTEX_SYNC_SKIP");
        return Nx2BufferSkip() || value == nullptr || value[0] != '0';
    }();
    return enabled;
}

/// EDEN_NX2_INDEX_SYNC_SKIP=1: the index buffer gets EDEN_NX2_VERTEX_SYNC_SKIP's skip too.
[[nodiscard]] inline bool Nx2IndexSyncSkip() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_INDEX_SYNC_SKIP");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// EDEN_NX2_FAST_DRAW_VERIFY=1 with EDEN_NX2_BUFFER_SKIP or _VERTEX_SYNC_SKIP: skipped upload
/// scans still run, and the ones that find something count as mismatches.
[[nodiscard]] inline bool Nx2BufferSkipVerify() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_FAST_DRAW_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

static_assert(NUM_GRAPHICS_UNIFORM_BUFFERS <= 32, "fast bitmask must fit u32");

using UniformBufferSizes = std::array<std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>;
using ComputeUniformBufferSizes = std::array<u32, NUM_COMPUTE_UNIFORM_BUFFERS>;

enum class ObtainBufferSynchronize : u32 {
    NoSynchronize = 0,
    FullSynchronize = 1,
    SynchronizeNoDirty = 2,
};

enum class ObtainBufferOperation : u32 {
    DoNothing = 0,
    MarkAsWritten = 1,
    DiscardWrite = 2,
    MarkQuery = 3,
};

static constexpr BufferId NULL_BUFFER_ID{0};
static constexpr u32 DEFAULT_SKIP_CACHE_SIZE = static_cast<u32>(4_KiB);

struct Binding {
    DAddr device_addr{};
    GPUVAddr gpu_addr{};
    u32 size{};
    BufferId buffer_id;
    u32 segment_first{};
    u32 segment_count{};
};

struct MultiRangeSegment {
    BufferId buffer_id;
    DAddr device_addr{};
    u32 size{};
};

struct TextureBufferBinding : Binding {
    PixelFormat format;
};

static constexpr Binding NULL_BINDING{
    .device_addr = 0,
    .size = 0,
    .buffer_id = NULL_BUFFER_ID,
};

template <typename Buffer>
struct HostBindings {
    boost::container::static_vector<Buffer*, NUM_VERTEX_BUFFERS> buffers;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> offsets;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> sizes;
    boost::container::static_vector<u64, NUM_VERTEX_BUFFERS> strides;
    u32 min_index{NUM_VERTEX_BUFFERS};
    u32 max_index{0};
};

/// Guest CPU writes collected but not applied yet, as sorted and merged [begin, end) runs. Taken
/// for every download landing, on the fencing thread under the buffer cache's lock among others: a
/// RangeSet allocated a tree node per write (~5% of that thread outdoors in Ocarina of Time,
/// measured by nemyu-coco).
class PendingWriteRanges {
public:
    void Add(DAddr begin, u64 size) {
        runs.emplace_back(begin, begin + size);
    }

    /// Sorts and merges the runs added since; call before the queries below.
    void Finish() {
        if (runs.size() < 2) {
            return;
        }
        std::ranges::sort(runs);
        size_t last = 0;
        for (size_t i = 1; i < runs.size(); ++i) {
            if (runs[i].first <= runs[last].second) {
                runs[last].second = (std::max)(runs[last].second, runs[i].second);
            } else {
                runs[++last] = runs[i];
            }
        }
        runs.resize(last + 1);
    }

    /// Calls func(start, size) for each part of [begin, begin + size) no run covers.
    template <typename Func>
    void ForEachGap(DAddr begin, u64 size, Func&& func) const {
        const DAddr end = begin + size;
        DAddr cursor = begin;
        for (auto it = First(begin); it != runs.end() && it->first < end; ++it) {
            if (it->first > cursor) {
                func(cursor, it->first - cursor);
            }
            cursor = (std::max)(cursor, it->second);
        }
        if (cursor < end) {
            func(cursor, end - cursor);
        }
    }

private:
    /// The first run ending past `begin` (merged runs end in order too).
    auto First(DAddr begin) const {
        return std::ranges::upper_bound(runs, begin, std::ranges::less{},
                                        [](const std::pair<DAddr, DAddr>& run) {
                                            return run.second;
                                        });
    }

    std::vector<std::pair<DAddr, DAddr>> runs;
};

class BufferCacheChannelInfo : public ChannelInfo {
public:
    BufferCacheChannelInfo() = delete;
    BufferCacheChannelInfo(Tegra::Control::ChannelState& state) noexcept : ChannelInfo(state) {}
    BufferCacheChannelInfo(const BufferCacheChannelInfo& state) = delete;
    BufferCacheChannelInfo& operator=(const BufferCacheChannelInfo&) = delete;

    Binding index_buffer;
    std::array<Binding, NUM_VERTEX_BUFFERS> vertex_buffers;
    std::array<std::array<Binding, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES> uniform_buffers;
    std::array<std::array<Binding, NUM_STORAGE_BUFFERS>, NUM_STAGES> storage_buffers;
    std::array<std::array<TextureBufferBinding, NUM_TEXTURE_BUFFERS>, NUM_STAGES> texture_buffers;
    std::array<Binding, NUM_TRANSFORM_FEEDBACK_BUFFERS> transform_feedback_buffers;
    Binding count_buffer_binding;
    Binding indirect_buffer_binding;

    std::array<Binding, NUM_COMPUTE_UNIFORM_BUFFERS> compute_uniform_buffers;
    std::array<Binding, NUM_STORAGE_BUFFERS> compute_storage_buffers;
    std::array<TextureBufferBinding, NUM_TEXTURE_BUFFERS> compute_texture_buffers;

    std::array<u32, NUM_STAGES> enabled_uniform_buffer_masks{};
    u32 enabled_compute_uniform_buffer_mask = 0;

    const UniformBufferSizes* uniform_buffer_sizes{};
    const ComputeUniformBufferSizes* compute_uniform_buffer_sizes{};

    std::array<u64, NUM_STAGES> enabled_storage_buffers{};
    std::array<u64, NUM_STAGES> written_storage_buffers{};
    u64 enabled_compute_storage_buffers = 0;
    u64 written_compute_storage_buffers = 0;
    /// Written only by run-time selected global accesses (see SelectedWriteExtent).
    std::array<u64, NUM_STAGES> selected_write_storage_buffers{};
    u64 selected_write_compute_storage_buffers = 0;
    /// Bound from a pointer table entry.
    std::array<u64, NUM_STAGES> table_storage_buffers{};
    u64 table_compute_storage_buffers = 0;
    // Bound by a native (Switch 2) compute program, whose pointers carry no size.
    u64 native_compute_storage_buffers = 0;
    // Bytes from each compute storage binding's start that a dispatch marks GPU-written.
    std::array<u32, NUM_STORAGE_BUFFERS> compute_storage_write_marks{};

    u32 total_graphics_storage_buffers = 0;
    u32 total_compute_storage_buffers = 0;

    std::array<u32, NUM_STAGES> enabled_texture_buffers{};
    std::array<u32, NUM_STAGES> written_texture_buffers{};
    std::array<u32, NUM_STAGES> image_texture_buffers{};
    u32 enabled_compute_texture_buffers = 0;
    u32 written_compute_texture_buffers = 0;
    u32 image_compute_texture_buffers = 0;

    std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS> uniform_cache_hits{};
    std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS> uniform_cache_shots{};

    u32 uniform_buffer_skip_cache_size = DEFAULT_SKIP_CACHE_SIZE;

    bool has_deleted_buffers = false;

    std::array<u32, NUM_STAGES> dirty_uniform_buffers{};
    std::array<u32, NUM_STAGES> fast_bound_uniform_buffers{};
    std::array<std::array<u32, NUM_GRAPHICS_UNIFORM_BUFFERS>, NUM_STAGES>
        uniform_buffer_binding_sizes{};
};

template <class P>
class BufferCache : public VideoCommon::ChannelSetupCaches<BufferCacheChannelInfo> {
    // Page size for caching purposes.
    // This is unrelated to the CPU page size and it can be changed as it seems optimal.
#ifdef YUZU_LEGACY
    static constexpr u32 CACHING_PAGEBITS = 12;
#else
    static constexpr u32 CACHING_PAGEBITS = 16;
#endif
    static constexpr u64 CACHING_PAGESIZE = u64{1} << CACHING_PAGEBITS;

    static constexpr bool IS_OPENGL = P::IS_OPENGL;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS =
        P::HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT =
        P::HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = P::NEEDS_BIND_UNIFORM_INDEX;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = P::NEEDS_BIND_STORAGE_INDEX;
    static constexpr bool USE_MEMORY_MAPS = P::USE_MEMORY_MAPS;
    static constexpr bool SEPARATE_IMAGE_BUFFERS_BINDINGS = P::SEPARATE_IMAGE_BUFFER_BINDINGS;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = P::USE_MEMORY_MAPS_FOR_UPLOADS;

#ifdef YUZU_LEGACY
    static constexpr s64 TARGET_THRESHOLD = 3_GiB;
#else
    static constexpr s64 TARGET_THRESHOLD = 4_GiB;
#endif

    static constexpr s64 DEFAULT_EXPECTED_MEMORY = 512_MiB;
    static constexpr s64 DEFAULT_CRITICAL_MEMORY = 1_GiB;

    // Debug Flags.

    static constexpr bool DISABLE_DOWNLOADS = true;

    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

    using Runtime = typename P::Runtime;
    using Buffer = typename P::Buffer;
    using Async_Buffer = typename P::Async_Buffer;
    using MemoryTracker = typename P::MemoryTracker;

    struct OverlapResult {
        boost::container::small_vector<BufferId, 16> ids;
        DAddr begin;
        DAddr end;
        bool has_stream_leap = false;
    };

public:
    explicit BufferCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, Runtime& runtime_);

    ~BufferCache();

    void TickFrame();

    bool BindMultiRangeStorage(const Binding& binding, bool is_written,
                               std::span<const MultiRangeSegment> pool,
                               u64 write_mark_limit = ~u64{0});

    void ResolveMultiRangeStorage(Binding& binding, bool is_written,
                                  std::vector<MultiRangeSegment>& pool);

    void UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size);

    void WriteMemory(DAddr device_addr, u64 size);

    void CachedWriteMemory(DAddr device_addr, u64 size);

    bool OnCPUWrite(DAddr device_addr, u64 size);

    void DownloadMemory(DAddr device_addr, u64 size);

    /// EDEN_NX2_ASYNC_READBACK: DownloadMemory for a guest CPU read. Records the copies and
    /// submits them without waiting for the host GPU, for FinishReadDownloads to write to guest
    /// memory. Their ranges stay GPU-modified until then, and another read of them waits for these
    /// copies (GetFlushArea: in_flight).
    void DownloadMemoryForRead(DAddr device_addr, u64 size);

    /// Writes the read downloads over [device_addr, device_addr + size), and those recorded before
    /// them, to guest memory, waiting for the host GPU if it must. Any thread: the reading one
    /// before it reads, the GPU's side before it writes guest memory there. Takes `mutex` itself
    /// (a caller holding it keeps it held while the host GPU finishes). Returns how long it waited
    /// for the host GPU (at least 1 ns if it did), or nothing when no download was over the range.
    std::optional<u64> FinishReadDownloads(DAddr device_addr, u64 size);

    /// A GPU write into the buffers over the range that the cache doesn't record itself (the query
    /// cache's result copies), after the read downloads taken so far: they still write their older
    /// bytes to guest memory, as synchronous flushes would have, but the range stays GPU-modified
    /// for a later read to download again. `mutex` held.
    void OutdateReadDownloads(DAddr device_addr, u64 size);

    /// Downloads until EndDownloadBatch share one wait for the GPU; their bytes reach guest memory
    /// at EndDownloadBatch.
    void BeginDownloadBatch();
    void EndDownloadBatch();

    std::optional<VideoCore::RasterizerDownloadArea> GetFlushArea(DAddr device_addr, u64 size);

    /// The core consumed the read traps of these pages for a read the texture cache answered:
    /// the next GPU write here must arm them again (ArmReadTraps skips pages counted as armed).
    void ForgetReadTraps(DAddr device_addr, u64 size) {
        const DAddr begin = Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE);
        read_trapped_ranges.Subtract(
            begin, Common::AlignUp(device_addr + size, Core::DEVICE_PAGESIZE) - begin);
    }

    /// Downloads the GPU-modified ranges in [device_addr, +window) and marks them Preflushable,
    /// as a read trap would (or not, as PreflushesOnRead says).
    void FlushAhead(DAddr device_addr, u64 window);

    /// The part of the `block`-aligned block around device_addr inside the cached buffer holding
    /// it, as (start, size); size 0 when no buffer holds it.
    std::pair<DAddr, u64> ReadRegion(DAddr device_addr, u64 block) {
        const u64 page = device_addr >> CACHING_PAGEBITS;
        if (page >= page_table.size() || !page_table[page]) {
            return {};
        }
        const Buffer& buffer = slot_buffers[page_table[page]];
        const DAddr block_begin = Common::AlignDown(device_addr, block);
        const DAddr begin = (std::max)(block_begin, buffer.CpuAddr());
        const DAddr end = (std::min)(block_begin + block, buffer.CpuAddr() + buffer.SizeBytes());
        return begin < end ? std::pair<DAddr, u64>{begin, end - begin} : std::pair<DAddr, u64>{};
    }

    /// Without downloads at every fence, a guest CPU read is the only way GPU-written buffer
    /// data reaches guest memory, so reads must ask the buffer cache (GetFlushArea).
    bool FlushesOnCpuRead() const {
        return !AsyncBufferDownloads();
    }

    bool InlineMemory(DAddr dest_address, size_t copy_size, std::span<const u8> inlined_buffer);

    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size);

    void DisableGraphicsUniformBuffer(size_t stage, u32 index);

    void UpdateGraphicsBuffers(bool is_indexed);

    void UpdateComputeBuffers();

    void BindHostGeometryBuffers(bool is_indexed);

    void BindHostStageBuffers(size_t stage);

    void BindHostComputeBuffers();

    /// Called before a draw or dispatch: the barrier the uploads since the last one need.
    void UploadBarrier();

    void SetUniformBuffersState(const std::array<u32, NUM_STAGES>& mask,
                                const UniformBufferSizes* sizes);

    void SetComputeUniformBufferState(u32 mask, const ComputeUniformBufferSizes* sizes);

    void UnbindGraphicsStorageBuffers(size_t stage);

    bool BindGraphicsStorageBuffer(size_t stage, size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                   bool is_written, u32 indirect_offset = ~0U,
                                   bool selected_writes = false);

    void UnbindGraphicsTextureBuffers(size_t stage);

    void BindGraphicsTextureBuffer(size_t stage, size_t tbo_index, GPUVAddr gpu_addr, u32 size,
                                   PixelFormat format, bool is_written, bool is_image);

    void UnbindComputeStorageBuffers();

    /// write_extent: how far the program's stores reach past the pointer (native programs).
    void BindComputeStorageBuffer(size_t ssbo_index, u32 cbuf_index, u32 cbuf_offset,
                                  bool is_written, u32 indirect_offset = ~0U,
                                  bool selected_writes = false,
                                  const Shader::StorageWriteExtent& write_extent = {});

    void UnbindComputeTextureBuffers();

    void BindComputeTextureBuffer(size_t tbo_index, GPUVAddr gpu_addr, u32 size, PixelFormat format,
                                  bool is_written, bool is_image);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainBuffer(GPUVAddr gpu_addr, u32 size,
                                                       ObtainBufferSynchronize sync_info,
                                                       ObtainBufferOperation post_op);

    [[nodiscard]] std::pair<Buffer*, u32> ObtainCPUBuffer(DAddr gpu_addr, u32 size,
                                                          ObtainBufferSynchronize sync_info,
                                                          ObtainBufferOperation post_op);

    /// GPU work recorded now reads or writes these bytes, of a buffer obtained before (work set
    /// up once that keeps reading them, as conditional rendering does). Creates no buffer.
    void MarkUsed(DAddr device_addr, u64 size);
    void FlushCachedWrites();

    /// Where the guest CPU writes into GPU-written memory that were collected but not applied yet
    /// come from: downloads must not write the GPU's older copy over them.
    using PendingCpuWrites = std::function<void(const std::function<void(DAddr, size_t)>&)>;
    void SetPendingCpuWrites(PendingCpuWrites source) {
        pending_cpu_writes = std::move(source);
    }

    /// Return true when there are uncommitted buffers to be downloaded
    [[nodiscard]] bool HasUncommittedFlushes() const noexcept;

    void AccumulateFlushes();

    /// Return true when the caller should wait for async downloads
    [[nodiscard]] bool ShouldWaitAsyncFlushes() const noexcept;

    /// Commit asynchronous downloads
    void CommitAsyncFlushes();
    void CommitAsyncFlushesHigh();

    /// Pop asynchronous downloads
    void PopAsyncFlushes();
    void PopAsyncBuffers();
    /// Pops the downloads of `count` fences the fencing thread releases together.
    void PopAsyncFlushes(size_t count);

    bool DMACopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount);

    bool DMAClear(GPUVAddr src_address, u64 amount, u32 value);

    /// Switch 2 GPU-written command lists (nx2.h, g_nx2_command_list_pages). While collecting (a
    /// compute dispatch or a DMA copy being recorded), writes to command list pages are noted;
    /// Nx2CopyListPages then records copies of those pages into host memory after the work. A
    /// draw keeps its requests for the next dispatch or DMA copy (Tegra::Nx2DrawListCopies).
    void Nx2CollectListCopies(bool collect, bool keep_requests = false) {
        nx2_collect_list_copies = collect;
        if (!collect && !keep_requests) {
            nx2_list_copy_requests.clear();
        }
    }
    /// True when it recorded copies, which the GPU completes at `tick` (the caller submits).
    bool Nx2CopyListPages(u64 tick);
    enum class Nx2ListRead { Unknown, Copied, Synced, Pending };
    /// Front thread: Synced when guest memory holds the page `device_addr` lies in (flushed and
    /// written by no GPU work since) or, with EDEN_NX2_LIST_WORDS, the bytes [device_addr,
    /// device_addr + out.size()) of it (no GPU work wrote them since the page was flushed, or the
    /// host wrote them since). Else Copied with `out` filled once the GPU is done with the copy of
    /// the page's last write (Nx2ReadListWords). Unknown otherwise: the caller drains and flushes.
    /// With `pending_tick`, a copy the GPU has not finished returns Pending and sets the tick
    /// instead of waiting for it.
    Nx2ListRead Nx2ReadListCopy(DAddr device_addr, std::span<u8> out,
                                u64* pending_tick = nullptr);
    /// Front thread, the split drained: the command list page was flushed to guest memory.
    void Nx2NoteListSynced(DAddr page);
    /// CPU, inline and copy engine writes of bytes guest memory holds: they reach guest memory, so
    /// a synced page stays synced and the words they cover whole are guest memory's again, but
    /// its copy no longer holds the page.
    void Nx2NoteListHostWrite(DAddr device_addr, u64 size);

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(DAddr addr, size_t size);

    /// Visits the uniform and storage buffers bound for the last draw or dispatch, for
    /// diagnostics. Nothing is created or synchronized. Stage 5 is compute.
    template <typename Func>
    void ForEachBoundBuffer(bool compute, Func&& func) {
        const auto visit = [&](u32 stage, bool storage, u32 index, bool written,
                               const Binding& binding, u32 size) {
            if (binding.device_addr != 0 && size != 0) {
                func(stage, storage, index, written, binding, size);
            }
        };
        if (compute) {
            const auto* const sizes = channel_state->compute_uniform_buffer_sizes;
            ForEachEnabledBit(channel_state->enabled_compute_uniform_buffer_mask, [&](u32 index) {
                const Binding& binding = channel_state->compute_uniform_buffers[index];
                visit(5, false, index, false, binding,
                      sizes ? (std::min)(binding.size, (*sizes)[index]) : binding.size);
            });
            ForEachEnabledBit(channel_state->enabled_compute_storage_buffers, [&](u32 index) {
                const Binding& binding = channel_state->compute_storage_buffers[index];
                visit(5, true, index,
                      ((channel_state->written_compute_storage_buffers >> index) & 1) != 0,
                      binding, binding.size);
            });
            return;
        }
        const auto* const sizes = channel_state->uniform_buffer_sizes;
        for (u32 stage = 0; stage < NUM_STAGES; ++stage) {
            ForEachEnabledBit(channel_state->enabled_uniform_buffer_masks[stage], [&](u32 index) {
                const Binding& binding = channel_state->uniform_buffers[stage][index];
                visit(stage, false, index, false, binding,
                      sizes ? (std::min)(binding.size, (*sizes)[stage][index]) : binding.size);
            });
            ForEachEnabledBit(channel_state->enabled_storage_buffers[stage], [&](u32 index) {
                const Binding& binding = channel_state->storage_buffers[stage][index];
                visit(stage, true, index,
                      ((channel_state->written_storage_buffers[stage] >> index) & 1) != 0, binding,
                      binding.size);
            });
        }
    }

    /// Return true when a region is registered on the cache
    [[nodiscard]] bool IsRegionRegistered(DAddr addr, size_t size);

    /// Return true when a CPU region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(DAddr addr, size_t size);

    void SetDrawIndirect(
        const Tegra::Engines::Maxwell3D::DrawManager::IndirectParams* current_draw_indirect_) {
        current_draw_indirect = current_draw_indirect_;
    }

    [[nodiscard]] std::pair<Buffer*, u32> GetDrawIndirectCount();

    [[nodiscard]] std::pair<Buffer*, u32> GetDrawIndirectBuffer();

    /// Fast draw verify (nx2_fast_draw.h): a hash of the texture buffer bindings delta draws keep.
    [[nodiscard]] u64 KeptTextureBuffersHash() const;

    /// Index and vertex stream ranges the back split's pool resolved for the current draw, or
    /// nullptr (EDEN_NX2_BACK_SPLIT): UpdateGraphicsBuffers takes them instead of its own.
    void UsePreparedGeometry(PreparedGeometry* prepared) noexcept {
        prepared_geometry = prepared;
    }

    template <typename Func>
    void BufferOperations(Func&& func) {
        do {
            channel_state->has_deleted_buffers = false;
            func();
        } while (channel_state->has_deleted_buffers);
    }

    Common::SpinRecursiveMutex mutex;
    Runtime& runtime;
    bool any_buffer_uploaded = false;
    /// Indirectly read banks of native programs already uploaded this frame.
    std::unordered_set<DAddr> native_indirect_uniforms_this_frame;
    /// Uniform bytes bound from stream copies this frame (EDEN_NX2_UNIFORM_STREAM).
    u64 uniform_stream_frame_bytes = 0;
    /// The guest contents of each page of native programs' indirectly read banks at their last
    /// check (EDEN_NX2_NATIVE_UBO_REUPLOAD=2).
    std::unordered_map<DAddr, std::array<u8, Core::DEVICE_PAGESIZE>> native_bank_pages;

private:
    template <typename Mask, typename Func>
    static void ForEachEnabledBit(Mask enabled_mask, Func&& func) {
        for (u32 index = 0; enabled_mask != 0; ++index, enabled_mask >>= 1) {
            const int disabled_bits = std::countr_zero(enabled_mask);
            index += disabled_bits;
            enabled_mask >>= disabled_bits;
            func(index);
        }
    }

    template <typename Func>
    void ForEachBufferInRange(DAddr device_addr, u64 size, Func&& func) {
        const u64 page_end = Common::DivCeil(device_addr + size, CACHING_PAGESIZE);
        for (u64 page = device_addr >> CACHING_PAGEBITS; page < page_end;) {
            const BufferId buffer_id = page_table[page];
            if (!buffer_id) {
                ++page;
                continue;
            }
            Buffer& buffer = slot_buffers[buffer_id];
            func(buffer_id, buffer);

            const DAddr end_addr = buffer.CpuAddr() + buffer.SizeBytes();
            page = Common::DivCeil(end_addr, CACHING_PAGESIZE);
        }
    }

    static bool IsRangeGranular(DAddr device_addr, size_t size) {
        return (device_addr & ~Core::DEVICE_PAGEMASK) ==
               ((device_addr + size) & ~Core::DEVICE_PAGEMASK);
    }

    void RunGarbageCollector(bool aggressive_gc);

    void BindHostIndexBuffer();

    void BindHostVertexBuffers();

    void BindHostDrawIndirectBuffers();

    void BindHostGraphicsUniformBuffers(size_t stage);

    void BindHostGraphicsUniformBuffer(size_t stage, u32 index, u32 binding_index, bool needs_bind);

    /// EDEN_NX2_UNIFORM_STREAM=1 (opt-in, Vulkan): a uniform buffer on the cached path (above
    /// the fast path's size, up to 64 KiB, 16 MiB a frame) whose CPU-written data would be uploaded
    /// into a range already in use, which ends the render pass, is copied into the stream buffer
    /// and bound from there. Splatoon Raiders' hub ended 4-7 render passes a frame this way.
    static bool StreamsUniformData() {
        static const bool enabled = [] {
            const char* const value = std::getenv("EDEN_NX2_UNIFORM_STREAM");
            return value != nullptr && value[0] == '1';
        }();
        return enabled;
    }
    bool StreamUniformBuffer(size_t stage, u32 binding_index, DAddr device_addr, u32 size,
                             Buffer& buffer);

    void BindHostGraphicsStorageBuffers(size_t stage);

    void BindHostGraphicsTextureBuffers(size_t stage);

    void BindHostTransformFeedbackBuffers();

    void BindHostVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size, u32 stride);

    void BindHostComputeUniformBuffers();

    void BindHostComputeStorageBuffers();

    void BindHostComputeTextureBuffers();

    void DoUpdateGraphicsBuffers(bool is_indexed);

    void DoUpdateComputeBuffers();

    void UpdateIndexBuffer();

    void UpdateVertexBuffers();

    void UpdateVertexBuffer(u32 index);

    void UpdateDrawIndirect();

    void UpdateUniformBuffers(size_t stage);

    void UpdateStorageBuffers(size_t stage);

    void UpdateTextureBuffers(size_t stage);

    void UpdateTransformFeedbackBuffers();

    void UpdateTransformFeedbackBuffer(u32 index);

    void UpdateComputeUniformBuffers();

    void UpdateComputeStorageBuffers();

    void UpdateComputeTextureBuffers();

    /// `capped`: `size` is EDEN_NX2_SELECTED_WRITE_BOUND's bound, below the binding's size, so
    /// the GPU may write past it (Nx2NoteListWrite).
    void MarkWrittenBuffer(BufferId buffer_id, DAddr device_addr, u32 size, bool needs_sync = false,
                           bool capped = false);

    /// EDEN_NX2_SELECTED_WRITE_BOUND=1: the GPU-written mark of a written storage binding,
    /// bounded for bindings only written by run-time selected global accesses.
    u32 WrittenMarkSize(bool compute, size_t stage, u32 index, const Binding& binding);
    /// Counts a GPU write to the command list pages in the range (invalidating their copies),
    /// notes the words it may write (Nx2ListPage::gpu_words) and, while collecting, asks for new
    /// copies. `to_page_end`: the GPU may write past the range (a capped mark), so the words up to
    /// the end of its last page count as written too.
    void Nx2NoteListWrite(BufferId buffer_id, DAddr device_addr, u64 size,
                          bool to_page_end = false);

    [[nodiscard]] BufferId FindBuffer(DAddr device_addr, u32 size, bool sparse_compatible);

    void SynchronizeBufferWrites(Buffer& buffer);

    [[nodiscard]] OverlapResult ResolveOverlaps(DAddr device_addr, u32 wanted_size);

    void JoinOverlap(BufferId new_buffer_id, BufferId overlap_id, bool accumulate_stream_score);

    [[nodiscard]] BufferId CreateBuffer(DAddr device_addr, u32 wanted_size,
                                        bool sparse_compatible);

    void Register(BufferId buffer_id);

    void Unregister(BufferId buffer_id);

    template <bool insert>
    void ChangeRegister(BufferId buffer_id);

    void TouchBuffer(Buffer& buffer, BufferId buffer_id) noexcept;

    bool SynchronizeBuffer(Buffer& buffer, DAddr device_addr, u32 size);

    void UploadMemory(Buffer& buffer, u64 total_size_bytes, u64 largest_copy,
                      std::span<BufferCopy> copies);

    void ImmediateUploadMemory(Buffer& buffer, u64 largest_copy,
                               std::span<const BufferCopy> copies);

    void MappedUploadMemory(Buffer& buffer, u64 total_size_bytes, std::span<BufferCopy> copies);

    void DownloadBufferMemory(Buffer& buffer_id);

    void DownloadBufferMemory(Buffer& buffer_id, DAddr device_addr, u64 size);

    /// Downloads the GPU-written data of several buffers with one wait for the GPU.
    void DownloadBuffersMemory(std::span<const BufferId> buffer_ids);

    /// Downloads the GPU-written data in several ranges with one wait for the GPU.
    void DownloadRangesMemory(std::span<const std::pair<DAddr, u64>> ranges);

    /// EDEN_NX2_READBACK_FENCE_COPY: writes the GPU-written bytes of the range to guest memory
    /// from the pending fence downloads that copied their newest writes, waiting for the GPU only
    /// until those copies are done. False, with nothing done, when one of them has no such copy.
    bool ReadFenceDownloads(DAddr device_addr, u64 size);

    void NoteFenceRead(bool served, std::chrono::steady_clock::duration waited);

    void DeleteBuffer(BufferId buffer_id, bool do_not_mark = false);

    [[nodiscard]] Binding StorageBufferBinding(GPUVAddr ssbo_addr, u32 cbuf_index,
                                               bool is_written, bool native_pointer = false,
                                               bool table_pointer = false,
                                               bool compute = false) const;

    [[nodiscard]] Binding ComputeStorageBufferBinding(GPUVAddr gpu_addr, u64 next_qword,
                                                      u32 cbuf_index, bool is_written,
                                                      bool native_pointer, bool table_pointer,
                                                      bool compute) const;

    /// StorageBufferBinding's answers, which hold until the page table changes (ssbo_bindings).
    struct CachedStorageBinding {
        size_t owner; ///< MemoryManager id + 1, 0 when empty
        GPUVAddr gpu_addr;
        u64 next_qword;
        u64 serial;
        u32 flags;
        Binding binding;
    };
    mutable std::array<std::array<CachedStorageBinding, 4>, 256> storage_binding_cache{};

    void TraceTablePointer(bool compute, size_t stage, size_t ssbo_index, u32 cbuf_index,
                           u32 cbuf_offset, u32 indirect_offset, GPUVAddr holder,
                           const Binding& binding);

    [[nodiscard]] TextureBufferBinding GetTextureBufferBinding(GPUVAddr gpu_addr, u32 size,
                                                               PixelFormat format);

    [[nodiscard]] std::span<const u8> ImmediateBufferWithData(DAddr device_addr, size_t size);

    [[nodiscard]] std::span<u8> ImmediateBuffer(size_t wanted_capacity);

    [[nodiscard]] bool HasFastUniformBufferBound(size_t stage, u32 binding_index) const noexcept;

    [[nodiscard]] Binding& VertexBufferSlot(u32 index);

    [[nodiscard]] const Binding& VertexBufferSlot(u32 index) const;

    void UpdateVertexBufferSlot(u32 index, const Binding& binding);

    /// Cancels pending downloads over the range (fences' and reads'), so none writes them over
    /// newer bytes. Those bytes reach guest memory later if at all (a GPU copy, a clear, an inline
    /// or a discarded write), so read downloads already taken land first: their reads get what a
    /// synchronous flush would have written.
    void ClearDownload(DAddr base_addr, u64 size);

    /// ClearDownload where guest memory gets the newer bytes before a read can find the range: a
    /// guest CPU write, a synchronous download landing under `mutex`, or a GPU-side write whose
    /// read downloads MemoryManager landed first. Read downloads leave the range rather than land,
    /// so this never waits for the host GPU.
    void ClearReplacedDownload(DAddr device_addr, u64 size);

    /// ClearDownload, for the downloads taken at fences only.
    void ClearFenceDownloads(DAddr device_addr, u64 size);

    /// [start, end) was downloaded at a fence and written: no longer GPU-modified, except where
    /// the GPU wrote it again since the copy was taken.
    void ForgetDownloaded(DAddr start, DAddr end);

    /// Whether every GPU-modified byte in the range is in a read download's clean_ranges.
    [[nodiscard]] bool ReadDownloadsCover(DAddr device_addr, u64 size) const;

    /// Writes the read downloads up to `tick` to guest memory, oldest first (`mutex` held, the
    /// host GPU past `tick`).
    void WriteReadDownloads(u64 tick);

    /// WriteReadDownloads for those the host GPU has completed: nothing waits.
    void WriteCompletedReadDownloads();

    /// WriteReadDownloads up to the newest read download over the range, if the host GPU ran it.
    /// Returns whether it did.
    bool WriteFinishedReadDownloads(DAddr device_addr, u64 size);

    /// Records a read download of the GPU-modified bytes over `ranges` that no read download
    /// brings yet, and submits it.
    void RecordReadDownload(std::span<const std::pair<DAddr, u64>> ranges);

    /// CPU writes collected but not yet applied (SetPendingCpuWrites). Downloads take them inside
    /// a VideoCore::DownloadLanding over the bytes they write, and write in it.
    [[nodiscard]] PendingWriteRanges PendingCpuWriteRanges();

    /// Writes downloaded bytes to guest memory, except over `cpu_written`.
    void WriteDownloadedMemory(const PendingWriteRanges& cpu_written, DAddr device_addr,
                               const u8* data, u64 size);

    void InlineMemoryImplementation(DAddr dest_address, size_t copy_size,
                                    std::span<const u8> inlined_buffer);

    /// A download copied into staging memory, written to guest memory at EndDownloadBatch.
    struct BatchedDownload {
        DAddr device_addr;
        const u8* staging;
        u64 size;
    };
    bool download_batch{};
    /// Only the thread that began the batch defers its downloads.
    std::thread::id download_batch_thread;
    std::vector<BatchedDownload> batched_downloads;

    Tegra::MaxwellDeviceMemoryManager& device_memory;

    Common::SlotVector<Buffer> slot_buffers;
#ifdef YUZU_LEGACY
    static constexpr size_t TICKS_TO_DESTROY = 6;
#else
    static constexpr size_t TICKS_TO_DESTROY = 8;
#endif
    DelayedDestructionRing<Buffer, TICKS_TO_DESTROY> delayed_destruction_ring;

    const Tegra::Engines::Maxwell3D::DrawManager::IndirectParams* current_draw_indirect{};

    u32 last_index_count = 0;

    u32 enabled_vertex_buffers_mask = 0;
    u64 vertex_buffers_serial = 0;
    std::array<Binding, 32> v_buffer{};

    /// EDEN_NX2_BUFFER_SKIP: the inputs and result of each vertex stream's last lookup.
    struct VertexLookup {
        GPUVAddr begin{};
        GPUVAddr end{};
        const void* owner{};
        u64 map_serial{};
        u64 deletions{};
        Binding binding{};
        bool valid{};
    };
    std::array<VertexLookup, NUM_VERTEX_BUFFERS> vertex_lookups{};
    u64 buffer_deletions = 0;
    PreparedGeometry* prepared_geometry{};
    /// The index range BindHostIndexBuffer last scanned for uploads (EDEN_NX2_INDEX_SYNC_SKIP).
    struct IndexSync {
        BufferId buffer_id{};
        DAddr device_addr{};
        u32 size{};
        u64 deletions{};
    } index_sync;
    struct SkipStats {
        std::chrono::steady_clock::time_point window_start{};
        u64 lookups{};
        u64 lookups_kept{};
        u64 syncs{};
        u64 syncs_skipped{};
        u64 verify_mismatches{};
    } skip_stats;

    boost::container::small_vector<BufferCopy, 4> upload_copies;

    MemoryTracker memory_tracker;
    Common::RangeSet<DAddr> uncommitted_gpu_modified_ranges;

    /// Whether GPU-written buffer ranges are copied back to guest memory at the next fence;
    /// otherwise only when something flushes them (as upstream yuzu below High GPU accuracy).
    /// Not for Switch 2 titles: title 1 had 250-330 MB/s copied back (a fence per compute
    /// dispatch, the fencing thread holding the buffer cache while it wrote them), while its CPU
    /// read GPU-written memory at most 64 times in 200 s (counted with reactive flushing) and
    /// the native MME's DMA reads never touched those pages. Without them a fence costs ~1 us
    /// instead of ~5 and draw setup ~7% less. EDEN_NX2_BUFFER_DOWNLOADS=0/1 forces it.
    bool AsyncBufferDownloads() const {
        static const int forced = [] {
            const char* value = std::getenv("EDEN_NX2_BUFFER_DOWNLOADS");
            return value == nullptr ? -1 : (value[0] == '0' ? 0 : 1);
        }();
        if (forced >= 0) {
            return forced == 1;
        }
        return gpu_memory == nullptr || !gpu_memory->UsesNativeTiling();
    }

    /// Whether a GPU write to this range is copied back at the next fence. Ranges the guest CPU
    /// has read (Preflushable, set by the first reactive flush) always are: later reads skip the
    /// flush and rely on these downloads, e.g. collision meshes skinned by compute shaders.
    bool DownloadsAtFence(DAddr device_addr, u64 size) noexcept {
        return AsyncBufferDownloads() || memory_tracker.IsRegionPreflushable(device_addr, size);
    }

    /// Whether a guest CPU read of GPU-written data marks it Preflushable (DownloadsAtFence).
    /// EDEN_NX2_PREFLUSH=0 (Lichii's default for Switch 2 titles) flushes it on demand instead:
    /// the next GPU write there arms the read trap again, and a read that must flush also
    /// downloads the GPU writes in every range read so before (cpu_read_ranges), which the
    /// reads after it in the same burst would otherwise each flush. Ocarina of Time reads ~30
    /// compute buffers back each frame, page by page.
    static bool PreflushesOnRead() {
        static Common::Nx2::SessionValue<bool> session_preflush{[] {
            return !Common::Nx2::Setting("EDEN_NX2_PREFLUSH", "1").starts_with('0');
        }};
        return session_preflush.Get();
    }

    /// Whether a CPU read of a Preflushable region the GPU wrote since its last fence download
    /// flushes on demand (EDEN_NX2_PREFLUSH_READ_WAIT, on in Ocarina of Time) instead of reading
    /// what the downloads brought so far.
    static bool PreflushedReadsWait() {
        static Common::Nx2::SessionValue<bool> session_wait{[] {
            return Common::Nx2::Setting("EDEN_NX2_PREFLUSH_READ_WAIT", "0").starts_with('1');
        }};
        return session_wait.Get();
    }

    /// Whether a flush of GPU-written buffer bytes (a guest CPU read) takes them from the fence
    /// downloads that already copied them (EDEN_NX2_READBACK_FENCE_COPY) instead of copying them
    /// again and waiting for the GPU to finish everything submitted.
    static bool ReadsFenceDownloads() {
        static Common::Nx2::SessionValue<bool> session_fence_reads{[] {
            return Common::Nx2::Setting("EDEN_NX2_READBACK_FENCE_COPY", "0").starts_with('1');
        }};
        return session_fence_reads.Get();
    }

    /// A native (Switch 2) program's pointer is bare, so its binding is sized from the mapping (up
    /// to 8 MiB, upstream #535). With the word after the pointer as the size, writes past it were
    /// dropped (Donkey Kong Bananza's GPU-built command lists in the first cave were read stale and
    /// the GPU stopped; Fire Emblem's DLSS input) and reads past it returned zeros. Mapping every
    /// binding, graphics stages included, is what Ocarina of Time needs (Lichii 37569c01d9: its
    /// draws read past a page), but it costs Mario Kart World and Donkey Kong Bananza about a third
    /// of their frame rate (marking and tracking 8 MiB ranges every dispatch, upstream issue #622;
    /// Satsuma f6cd2e56bc saw the same). So only Ocarina maps all of them; other titles map the
    /// written compute bindings, except Mario Kart World, which ran without them up to 0.1.4 and
    /// races at 21-22 ms per frame without them against 28-31 ms with them (title defaults in
    /// nx2_title_defaults.h). EDEN_NX2_NATIVE_SSBO_MAPPED=1 maps all for every title, =w only
    /// written compute bindings, =r only read compute bindings, =0 none.
    static bool MapsNativeStorage(bool is_written, bool graphics) {
        static Common::Nx2::SessionValue<char> session_mode{[] {
            const std::string_view value = Common::Nx2::Setting("EDEN_NX2_NATIVE_SSBO_MAPPED", "w");
            return value.empty() ? '\0' : value[0];
        }};
        const char mode = session_mode.Get();
        if (mode == '1') {
            return true;
        }
        return !graphics && ((mode == 'w' && is_written) || (mode == 'r' && !is_written));
    }

    /// Queues a GPU write for download at the next fence where DownloadsAtFence holds. With only
    /// Preflushable marks, just the marked pages are queued: a write that covers one marked page
    /// of a large buffer would otherwise copy the whole buffer back at every fence (Mario Kart
    /// World's native compute bindings, sized from their mappings: gigabytes a second).
    /// EDEN_NX2_EXACT_PREFLUSH=0 queues the whole write as before.
    void QueueFenceDownload(DAddr device_addr, u64 size) {
        static const bool exact = [] {
            const char* value = std::getenv("EDEN_NX2_EXACT_PREFLUSH");
            return value == nullptr || value[0] != '0';
        }();
        fence_fresh_ranges.Subtract(device_addr, size);
        if (AsyncBufferDownloads()) {
            uncommitted_gpu_modified_ranges.Add(device_addr, size);
            if (Nx2DownloadTrace::g_enabled) {
                Nx2DownloadTrace::Queued(device_addr, size, size);
            }
            return;
        }
        if (!exact) {
            if (memory_tracker.IsRegionPreflushable(device_addr, size)) {
                uncommitted_gpu_modified_ranges.Add(device_addr, size);
                if (Nx2DownloadTrace::g_enabled) {
                    Nx2DownloadTrace::Queued(device_addr, size, size);
                }
            } else {
                ArmReadTraps(device_addr, size);
            }
            return;
        }
        const DAddr end = device_addr + size;
        u64 queued = 0;
        memory_tracker.ForEachPreflushableRange(device_addr, size, [&](DAddr begin, u64 length) {
            const DAddr lower = (std::max)(begin, device_addr);
            const DAddr upper = (std::min)(begin + length, end);
            if (lower < upper) {
                uncommitted_gpu_modified_ranges.Add(lower, upper - lower);
                queued += upper - lower;
            }
        });
        if (queued < size) {
            ArmReadTraps(device_addr, size);
        }
        if (Nx2DownloadTrace::g_enabled && queued != 0) {
            Nx2DownloadTrace::Queued(device_addr, size, queued);
        }
    }

    /// GPU writes that are not downloaded at fences: trap the guest CPU's next read of their
    /// pages, which flushes them once and marks them Preflushable (then DownloadsAtFence), or
    /// only flushes them (PreflushesOnRead).
    void ArmReadTraps(DAddr device_addr, u64 size) {
        const DAddr begin = Common::AlignDown(device_addr, Core::DEVICE_PAGESIZE);
        const DAddr end = Common::AlignUp(device_addr + size, Core::DEVICE_PAGESIZE);
        // Only the parts not armed yet: the core visits each page it is given, and a buffer the
        // GPU rewrites every frame is mostly still armed (the guest read a few of its pages).
        boost::container::small_vector<std::pair<DAddr, DAddr>, 4> unarmed;
        DAddr next = begin;
        read_trapped_ranges.ForEachInRange(begin, end - begin, [&](DAddr lower, DAddr upper) {
            if (lower > next) {
                unarmed.emplace_back(next, lower);
            }
            next = (std::max)(next, upper);
        });
        if (next < end) {
            unarmed.emplace_back(next, end);
        }
        if (unarmed.empty()) {
            return;
        }
        read_trapped_ranges.Add(begin, end - begin);
        for (const auto& [lower, upper] : unarmed) {
            device_memory.ArmReadTraps(lower, upper - lower);
        }
    }

    Common::RangeSet<DAddr> read_trapped_ranges;
    /// EDEN_NX2_PREFLUSH=0: ranges the guest CPU read after GPU writes (PreflushesOnRead).
    Common::RangeSet<DAddr> cpu_read_ranges;
    bool download_cpu_read_ranges{};
    Common::RangeSet<DAddr> gpu_modified_ranges;
    std::deque<Common::RangeSet<DAddr>> committed_gpu_modified_ranges;

    // Async Buffers
    Common::OverlapRangeSet<DAddr> async_downloads;
    std::deque<std::optional<Async_Buffer>> async_buffers;
    std::deque<boost::container::small_vector<BufferCopy, 4>> pending_downloads;
    /// The GPU tick that completes each pending_downloads entry's copies.
    std::deque<u64> pending_download_ticks;
    /// EDEN_NX2_READBACK_FENCE_COPY: bytes a pending download copied that no GPU write, flush or
    /// CPU write touched since (QueueFenceDownload sees every GPU write), so the newest pending
    /// download holding them has their current data.
    Common::RangeSet<DAddr> fence_fresh_ranges;
    std::chrono::steady_clock::time_point fence_read_window{};
    std::chrono::steady_clock::duration fence_read_wait{};
    u64 fence_reads_served{};
    u64 fence_reads_drained{};
    PendingCpuWrites pending_cpu_writes;
    std::optional<Async_Buffer> current_buffer;

    /// EDEN_NX2_ASYNC_READBACK: a DownloadMemoryForRead's copies, and what is left of them to do
    /// once the host GPU has run them.
    struct ReadDownload {
        u64 tick{}; ///< The submission with the copies
        Async_Buffer staging{};
        /// src_offset: the device address copied; dst_offset: where it is in `staging`.
        boost::container::small_vector<BufferCopy, 4> copies;
        /// Still to write to guest memory: a newer value there (ClearReplacedDownload) takes its
        /// bytes out. A newer GPU write doesn't: until a download of its own lands, guest memory
        /// is to hold what a synchronous flush would have left.
        Common::RangeSet<DAddr> write_ranges;
        /// No longer GPU-modified once written: a newer GPU write (OutdateReadDownloads) or value
        /// in guest memory takes its bytes out.
        Common::RangeSet<DAddr> clean_ranges;
    };
    /// Oldest first, and written to guest memory in that order: none lands over newer bytes.
    std::deque<ReadDownload> read_downloads;
    /// All the read downloads' clean_ranges (they don't overlap): GPU-modified bytes already on
    /// their way, which later reads wait for rather than download again.
    Common::RangeSet<DAddr> read_download_ranges;

    std::deque<Async_Buffer> async_buffers_death_ring;

    size_t immediate_buffer_capacity = 0;
    Common::ScratchBuffer<u8> immediate_buffer_alloc;
    /// Guest memory large uploads came from since the last cold hint (device address, size).
    std::vector<std::pair<DAddr, u64>> uploaded_buffer_ranges;

    struct LRUItemParams {
        using ObjectType = BufferId;
        using TickType = u64;
    };
    Common::LeastRecentlyUsedCache<LRUItemParams> lru_cache;
    VirtualRangeCache virtual_ranges;
    std::vector<MultiRangeSegment> graphics_segments;
    std::vector<MultiRangeSegment> compute_segments;
    u64 frame_tick = 0;
    /// EDEN_NX2_SELECTED_WRITE_BOUND: bounds already logged.
    std::set<std::tuple<bool, size_t, u32, GPUVAddr, u32, u64>> logged_write_bounds;
    /// EDEN_NX2_TABLE_POINTER_TRACE: last table entry and size bound per table pointer slot.
    std::unordered_map<u64, std::pair<GPUVAddr, u32>> traced_table_pointers;
    u64 total_used_memory = 0;
    u64 buffer_bytes = 0; ///< The buffers' own size (total_used_memory may be the device's)
    u64 minimum_memory = 0;
    u64 critical_memory = 0;
    u64 native_collect_bytes = 512_MiB;  ///< Switch 2: collect from this buffer_bytes on
    u64 native_aggressive_bytes = 1_GiB; ///< and GPU-written buffers too from this
    u64 budget_check_frames = 0;
    BufferId inline_buffer_id;
#ifdef YUZU_LEGACY
    bool immediately_free = false;
#endif

    // Covers the device address space (MaxwellDeviceTraits::device_virtual_bits).
    std::array<BufferId, ((1ULL << 36) >> CACHING_PAGEBITS)> page_table;
    Common::ScratchBuffer<u8> tmp_buffer;

    /// A command list page (Nx2CopyListPages): the GPU writes noted to it and the last copy.
    using Nx2Staging = decltype(std::declval<Runtime&>().DownloadStagingBuffer(size_t{}, true));
    static constexpr size_t Nx2ListPageWords = (size_t{1} << Tegra::Nx2WrittenPageBits) / 4;
    struct Nx2ListPage {
        u64 writes{};      ///< GPU and host writes noted to the page
        u64 copy_writes{}; ///< `writes` when the copy was made: the copy is current while equal
        u64 copy_tick{};   ///< GPU tick that completes the copy
        /// `writes` when the front last flushed the page: guest memory holds it while equal
        u64 synced_writes{~u64{0}};
        u32 copy_begin{};  ///< Bytes of the page the copy holds (a buffer can end inside it)
        u32 copy_end{};
        /// Written outside a compute dispatch or a DMA copy (a draw's storage writes) at least
        /// once: the front can't tell from what it sent whether such a write is still queued, so
        /// the page's lists always drain the split instead of reading a copy.
        bool draw_written{};
        /// The page's 4-byte words GPU work wrote that guest memory does not hold: set by the GPU
        /// writes noted to the page (the whole range a dispatch or copy may write), cleared for
        /// the words the host writes whole and for every word once the front flushed the page.
        /// Once it did (`flushed`), guest memory holds the other words. Before, GPU work may have
        /// written the page before it became a command list page, unnoted.
        std::bitset<Nx2ListPageWords> gpu_words;
        bool flushed{};
        std::optional<Nx2Staging> copy;
    };
    /// EDEN_NX2_LIST_WORDS (default on): lists read the words no GPU work wrote from guest memory.
    [[nodiscard]] static bool Nx2ListWordsEnabled();
    /// Sets (`set`) or clears the words of `entry.gpu_words` within [begin, end) bytes of its
    /// page: every word the bytes touch when setting, only the words they cover when clearing.
    static void Nx2MarkListWords(Nx2ListPage& entry, u64 begin, u64 end, bool set);
    /// Whether GPU work wrote any word of [begin, end) bytes of the page that guest memory lacks.
    [[nodiscard]] static bool Nx2HasListWords(const Nx2ListPage& entry, u64 begin, u64 end);
    /// Fills `out` with the page's bytes from `offset` (`device_addr`) on: with
    /// EDEN_NX2_LIST_WORDS, once the page was flushed, the words of `entry.gpu_words` from its copy
    /// and the others from guest memory; else all from the copy.
    void Nx2ReadListWords(const Nx2ListPage& entry, DAddr device_addr, u64 offset,
                          std::span<u8> out);
    std::mutex nx2_list_mutex; ///< nx2_list_pages: the split's front reads the copies
    std::unordered_map<DAddr, Nx2ListPage> nx2_list_pages;
    /// Copies replaced while the GPU may still write them: freed once it is past their tick.
    std::vector<std::pair<u64, Nx2Staging>> nx2_retired_list_copies;
    boost::container::small_vector<std::pair<BufferId, DAddr>, 8> nx2_list_copy_requests;
    bool nx2_collect_list_copies{};
};

} // namespace VideoCommon
