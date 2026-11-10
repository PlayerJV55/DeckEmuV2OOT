// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include "common/container/unordered_map.h"
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_shader_reuse.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_cache_v2.h"

namespace Core {
class System;
}

namespace Shader::IR {
struct Program;
}

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::ComputePipelineCacheKey> {
    size_t operator()(const Vulkan::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std

namespace Vulkan {

class ComputePipeline;
class DescriptorPool;
class Device;
class PipelineStatistics;
class RenderPassCache;
struct NativePrecompileState;
class Scheduler;

using VideoCommon::ShaderInfo;

struct ShaderPools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device,
                           Scheduler& scheduler, DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           DescriptorBufferRing& descriptor_buffer_ring,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();

    /// Whether the last CurrentGraphicsPipeline returned none because it is still building.
    [[nodiscard]] bool PipelineStillBuilding() const noexcept {
        return last_pipeline_building;
    }

    /// Counts a draw skipped while its pipeline builds, for the runtime pipelines line.
    void CountDrawSkippedWhileBuilding() noexcept {
        ++draws_skipped_while_building;
    }

    void TickFrame();

    /// A clear of the whole render target (every layer) at these addresses (0: none), for the
    /// skip guard: a target cleared whole every frame is redrawn every frame.
    void NoteClear(u64 color_address, u64 depth_address);

    /// native_key: the native program's NativeProgramKey, 0 for a Maxwell program.
    [[nodiscard]] ComputePipeline* CurrentComputePipeline(u64& native_key);

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

    /// Prepare Shaders with EDEN_NX2_PRECOMPILE_COMPUTE or EDEN_NX2_PRECOMPILE_GRAPHICS
    /// (vk_pipeline_precompile.cpp): scans the game's files for native programs on a background
    /// thread. LoadDiskResources builds them at its end and drops them, so the driver's cache
    /// holds them when the game first needs them. Call it before LoadDiskResources.
    void PrecompileNativePrograms(Core::System& system);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    /// A cached pipeline whose key differs from graphics_key only in vertex attributes its
    /// vertex stage doesn't read, or null.
    [[nodiscard]] GraphicsPipeline* FindAttributeAlias();

    /// Counts why a draw needed a pipeline the cache did not have, logged every 10 s.
    void ReportNewPipelineKey(const GraphicsPipelineCacheKey& key);

    /// EDEN_NX2_PIPELINE_CLEAN: CurrentGraphicsPipeline without FixedPipelineState::Refresh nor
    /// the key comparison while nothing Refresh reads changed since the last lookup.
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineClean();
    /// The registers Refresh reads with these features, as (first word, words) ranges.
    void BuildCleanWatch();
    [[nodiscard]] bool CleanInputsUnchanged() const;
    void TakeCleanSnapshot();

    struct CleanLookup {
        struct Range {
            u32 first;
            u32 count;
        };
        bool built{};
        std::vector<Range> ranges;     ///< Read by every Refresh
        std::vector<Range> xfb_ranges; ///< Read while transform feedback is enabled
        std::vector<u32> snapshot;     ///< ranges' words at the last Refresh
        std::vector<u32> xfb_snapshot;
        decltype(GraphicsPipelineCacheKey::unique_hashes) hashes{}; ///< At the last Refresh
        const Tegra::Engines::Maxwell3D* maxwell3d{};
        Maxwell::PrimitiveTopology topology{};
        Tegra::Engines::Maxwell3D::EngineHint engine_state{};
        /// current_pipeline when its key was graphics_key after the last Refresh, else null.
        GraphicsPipeline* pipeline{};
        u64 hits{};
        u64 misses{};
        std::chrono::steady_clock::time_point last_log{};
    } clean;

    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) const noexcept;

    /// EDEN_NX2_SKIP_GUARD (on by default): whether a Switch 2 draw may be skipped while its
    /// pipeline builds. Only when its targets are redrawn every frame (cleared whole this frame
    /// and in one of the two before) and `pipeline` (when known) writes no storage or transform
    /// feedback; a skipped draw into a texture the game draws once (a baked shadow, lighting or
    /// terrain texture) stays missing for good, so those draws wait for the build.
    [[nodiscard]] bool MaySkipNativeDraw(const GraphicsPipeline* pipeline) const;
    struct ClearFrames {
        u64 last{};
        u64 previous{};
    };
    std::unordered_map<u64, ClearFrames> cleared_frames; ///< Target address -> frames cleared
    mutable u32 skip_guard_held{};
    mutable std::chrono::steady_clock::time_point skip_guard_logged{};

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline();

    /// Binds the slots an evicted pipeline of graphics_key bound as storage buffer pointers,
    /// whatever they hold now, and logs those this draw's state would have left out.
    void KeepEvictedPointers(GraphicsEnvironments& environments,
                             const VideoCommon::EvictedPointerSlots::Slots& slots);

    /// The slow path's end: makes the pipeline the current one.
    [[nodiscard]] GraphicsPipeline* MakeCurrent(GraphicsPipeline* pipeline);

    /// Stores a pipeline built at run time in the disk cache (unless it is there already).
    void StoreRuntimePipeline(const GraphicsPipelineCacheKey& key, const GraphicsPipeline& pipeline,
                              GraphicsEnvironments& environments);

    /// EDEN_NX2_PIPELINE_ASYNC: what a build off the GPU thread takes in place of maxwell3d.
    struct AsyncBuildContext;
    struct AsyncGraphicsJob;
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineAsync();
    /// Whether the current draw may be skipped while its pipeline builds (BuiltPipeline's rule).
    [[nodiscard]] bool CanDiscardAsyncDraw() const;
    /// Queues graphics_key's build; null when it has to be built on the GPU thread.
    [[nodiscard]] std::shared_ptr<AsyncGraphicsJob> QueueAsyncGraphicsPipeline();
    /// Waits for the job within the native per-draw and per-frame budget.
    [[nodiscard]] bool WaitForAsyncBuild(AsyncGraphicsJob& job);
    void BuildAsyncGraphicsPipeline(const std::shared_ptr<AsyncGraphicsJob>& job,
                                    bool on_gpu_thread);
    /// Runs the job on the GPU thread if no worker took it yet, else waits for the worker.
    void FinishAsyncGraphicsPipeline(const std::shared_ptr<AsyncGraphicsJob>& job);
    /// Moves the finished builds into graphics_cache (GPU thread).
    void DrainAsyncPipelines();
    void ReportAsyncPipelines(std::chrono::steady_clock::time_point now);

    /// reuse_shaders (run-time builds): reuses the shaders of an earlier pipeline that fit, and
    /// keeps this one's for later ones (EDEN_NX2_SHADER_REUSE); links the pipeline from shader
    /// libraries with EDEN_NX2_GPL. translate_at_once (the GPU thread's live environments):
    /// translates the programs at once on stage_helpers when there are any.
    /// stand_in_vertex (Prepare Shaders' precompile of a pixel program in no pair): the pixel
    /// program takes every input it reads as written, as by the vertex program it is drawn with.
    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel, std::shared_ptr<ShaderPools> emit_pools = nullptr,
        const AsyncBuildContext* async = nullptr, bool reuse_shaders = false,
        bool translate_at_once = false, bool stand_in_vertex = false);

    /// Released pools from spare_pools, or new ones
    [[nodiscard]] std::shared_ptr<ShaderPools> TakeSparePools();
    /// Releases the pools' contents and keeps them in spare_pools while it has room
    void ReturnSparePools(std::shared_ptr<ShaderPools> pools);

    /// What a native compute translation needs to be rebuilt once a pointer it saw null is set
    struct ComputeRebuildInfo {
        /// Constant buffer slots that were null pointers at translation
        std::vector<std::pair<u32, u32>> null_pointer_slots;
        /// Constant buffer slots bound as storage buffer pointers (MappedStoragePointerKeys)
        std::vector<u64> mapped_pointers;
        u32 rebuilds{};
    };

    [[nodiscard]] bool StaleNullPointerSlot(const ComputeRebuildInfo& info) const;

    /// Translates the current compute program, binding the pointers in keep_pointers whatever
    /// their slots hold now (see GenericEnvironment::KeepStoragePointers). A native program's
    /// pointer slots go to rebuild_info.
    std::unique_ptr<ComputePipeline> CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                           const ShaderInfo* shader,
                                                           ComputeRebuildInfo& rebuild_info,
                                                           std::span<const u64> keep_pointers = {});

    std::unique_ptr<ComputePipeline> CreateComputePipeline(ShaderPools& pools,
                                                           const ComputePipelineCacheKey& key,
                                                           Shader::Environment& env,
                                                           PipelineStatistics* statistics,
                                                           bool build_in_parallel);

    void SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                      const vk::PipelineCache& pipeline_cache, u32 cache_version);

    vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                              u32 expected_cache_version);

    void QueueVulkanPipelineCacheFlush();

    const Device& device;
    Scheduler& scheduler;
    DescriptorPool& descriptor_pool;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    RenderPassCache& render_pass_cache;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};
    bool use_vulkan_pipeline_cache{};
    bool pipeline_binaries_enabled{};
    const size_t num_pipeline_workers;

    /// Graphics pipelines whose SPIR-V the build emits (async_pipelines), at most one per worker.
    std::atomic<size_t> pending_emissions{};
    /// Shader pools of finished emissions, kept for the next ones.
    std::mutex spare_pools_mutex;
    std::vector<std::shared_ptr<ShaderPools>> spare_pools;

    GraphicsPipelineCacheKey graphics_key{};
    GraphicsPipeline* current_pipeline{};
    mutable bool last_pipeline_building{}; ///< Set by BuiltPipeline when the draw is skipped
    /// Time native draws spent waiting for their pipelines in the current ~16 ms window
    mutable std::chrono::steady_clock::time_point native_wait_window{};
    mutable std::chrono::microseconds native_wait_spent{};
    u32 draws_skipped_while_building{};

    /// Destroys graphics pipelines unused for a while, once the GPU is done with them.
    void EvictUnusedPipelines();
    u64 frame_count{};
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> evicted_pipelines; ///< (tick, pipeline)
    std::unordered_set<u64> used_and_evicted; ///< Key hashes of pipelines used this session, evicted since
    /// Pointer slots of evicted pipelines, kept for their next translation
    VideoCommon::EvictedPointerSlots evicted_pointer_slots;

    ::Common::unordered_map<ComputePipelineCacheKey, std::unique_ptr<ComputePipeline>> compute_cache;
    /// Native compute pipelines of compute_cache with pointer slots that were null at translation
    ::Common::unordered_map<ComputePipelineCacheKey, ComputeRebuildInfo> compute_rebuild_info;
    /// Replaced compute pipelines, destroyed once the GPU and their worker builds are done with
    /// them (tick, pipeline)
    std::deque<std::pair<u64, std::unique_ptr<ComputePipeline>>> retired_compute_pipelines;
    /// EDEN_NX2_GPL: shader libraries the run-time graphics pipelines are linked from. Declared
    /// before the pipelines and the workers that build them.
    GraphicsPipelineLibraryCache graphics_libraries;
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;
    /// Keys FindAttributeAlias matched to a pipeline of graphics_cache (cleared when one is evicted)
    std::unordered_map<GraphicsPipelineCacheKey, GraphicsPipeline*> attribute_aliases;
    u32 attribute_alias_count{};
    std::chrono::steady_clock::time_point attribute_alias_logged{};

    ShaderPools main_pools;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::filesystem::path pipeline_cache_filename;
    /// Task/mesh pipelines, whose records carry the mesh registers (vulkan.bin's readers before
    /// these were stored can't skip such records).
    std::filesystem::path mesh_pipeline_cache_filename;

    /// Key hashes of the graphics pipelines in the disk cache. Only those a recent session used
    /// (used_pipelines) are built at boot, or all while memory allows with pipeline_preload=All;
    /// the rest are built when first needed, and not stored again. Each built pipeline holds
    /// driver memory (title 1: 6.5k cached, a quarter used).
    std::unordered_set<u64> disk_pipelines;
    /// The last key seen for each set of shaders, from the disk cache and this session: a new
    /// key for known shaders differs from it in state only (ReportNewPipelineKey).
    std::unordered_map<u64, GraphicsPipelineCacheKey> known_shader_keys;
    std::mutex known_shader_keys_mutex;
    struct NewKeyStats {
        static constexpr size_t NumFields = 24;
        std::array<u32, NumFields> window_fields{};
        std::array<u32, NumFields> session_fields{};
        std::array<u32, 3> window{};  ///< New shaders, state variants, cached but not built
        std::array<u32, 3> session{};
        /// Variants whose vertex attributes differ, and of those, the ones that differ only in
        /// attributes the vertex shader doesn't read (when the earlier pipeline is known), and
        /// the ones whose read attributes differ in format but keep the type the shader sees.
        u32 attribute_variants{};
        u32 unread_attribute_variants{};
        u32 format_only_attribute_variants{};
        std::chrono::steady_clock::time_point window_start{};
        u32 diff_lines{};
    } new_key_stats;
    std::unordered_map<u64, u8> used_pipelines; ///< Key hash -> sessions since it was last used
    std::unordered_set<const GraphicsPipeline*> used_this_session;
    std::filesystem::path used_pipelines_filename;
    std::chrono::steady_clock::time_point used_pipelines_saved{};
    /// in_background: the file is written on the serialization thread.
    void SaveUsedPipelines(bool in_background);

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;
    size_t pipelines_since_flush{};
    std::chrono::steady_clock::time_point last_flush{};
    std::atomic<size_t> last_cache_size{};
    std::atomic_bool flush_in_flight{};

    // EDEN_NX2_PIPELINE_ASYNC state, declared before workers so a job still running while the
    // cache is destroyed (workers joins it) never sees it destroyed.
    u32 async_pipelines{}; ///< 0 off, 1 Switch 1 draws, 2 native draws too
    /// Builds queued or running (GPU thread only); their graphics_cache entries are null.
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::shared_ptr<AsyncGraphicsJob>>
        async_pending;
    /// Key hashes whose build on a worker missed its snapshot: built on the GPU thread next.
    std::unordered_set<u64> async_sync_keys;
    std::mutex async_mutex;
    std::condition_variable async_idle;
    std::vector<std::shared_ptr<AsyncGraphicsJob>> async_completed; ///< Under async_mutex
    std::atomic<size_t> async_completed_count{};
    size_t async_running{}; ///< Jobs running on workers, under async_mutex
    bool async_stopping{};  ///< Under async_mutex: the cache is being destroyed
    struct AsyncStats {
        u32 queued{};
        u32 inline_built{};
        u32 waited{};
        u32 sync_built{};
        u32 fallback{};
        u64 skipped{};
        u64 snapshot_bytes{};
        std::atomic<u32> worker_built{};
        std::chrono::steady_clock::duration gpu_time{};
        // Parts of gpu_time: builds on the GPU thread, waits for builds on workers, and queueing
        // (environments and snapshots).
        std::chrono::steady_clock::duration build_time{};
        std::chrono::steady_clock::duration wait_time{};
        std::chrono::steady_clock::duration queue_time{};
        /// Part of wait_time with background builds running at the wait's start or end
        std::chrono::steady_clock::duration wait_background_time{};
        /// Background builds that waited for the workers (EDEN_NX2_BUILD_PRIORITY), and how long
        std::atomic<u32> background_yields{};
        std::atomic<u64> background_yield_ns{};
        std::chrono::steady_clock::time_point window{};
    } async_stats;
    /// Background builds (prebuild, compute warm-up) running now
    std::atomic<u32> background_building{};
    /// EDEN_NX2_BUILD_PRIORITY: background builds wait while the workers have builds to do.
    bool build_priority{};
    /// BuildPriority::YieldToForeground on the workers when build_priority is set; `stop` ends
    /// the wait.
    void YieldBackgroundBuild(const std::function<bool()>& stop);

    /// EDEN_NX2_SHADER_REUSE (on by default): a run-time build takes the translated shaders of
    /// an earlier pipeline that differs only in state they don't read (vk_shader_reuse.h).
    bool use_shader_reuse{};
    ShaderReuseCache shader_reuse;

    /// The title's warm-compute.bin (ShaderWarm::ComputeWarmPath, or EDEN_NX2_COMPUTE_WARM=<file>):
    /// compute programs prepared from the game files, converted with the missing reads guessed
    /// and built on low priority threads, then thrown away. They only fill the driver's shader
    /// cache, so a dispatch that later converts to the same SPIR-V finds the build ready instead
    /// of waiting seconds for it.
    void StartComputeWarm(u64 title_id);
    void StopComputeWarm();
    std::vector<std::jthread> compute_warm_threads;
    /// Set as soon as emulation is asked to stop (the loading stop token).
    std::atomic<bool> warm_cancel{false};
    std::unique_ptr<std::stop_callback<std::function<void()>>> warm_stop_callback;

    /// Builds the native programs PrecompileNativePrograms found and drops them; title_dir gets
    /// the list of those built (native_precompiled.bin), which later runs skip.
    void RunNativePrecompile(const std::filesystem::path& title_dir, std::stop_token stop_loading);
    /// EDEN_NX2_PRECOMPILE_GRAPHICS: notes a cached pipeline's render setup and vertex attribute
    /// types, which the graphics precompile copies.
    void NoteRecordedPipeline(const GraphicsPipelineCacheKey& key);
    /// Other boots with a precompile switch on: reads native_precompiled.bin, so that the
    /// session's end logs how many of its new pipelines Prepare Shaders built (GPU thread).
    void StartNativePrecompileHits(const std::filesystem::path& title_dir);
    void NoteNativePrecompileHit(const GraphicsPipelineCacheKey& key);
    void NoteNativePrecompileHit(const ComputePipelineCacheKey& key);
    void ReportNativePrecompileHits();
    std::unique_ptr<NativePrecompileState> native_precompile;

    /// EDEN_NX2_SHADER_CACHE_V2: writes the title's log, in place of the disk cache files. Declared
    /// before the threads that queue records to it.
    std::optional<VideoCommon::ShaderCacheV2::Writer> cache_v2;

    Common::ThreadWorker workers;
    /// EDEN_NX2_PARALLEL_STAGES (off by default): normal priority threads that translate the
    /// other programs of a new graphics pipeline while the GPU thread translates one.
    std::optional<Common::ThreadWorker> stage_helpers;
    /// EDEN_NX2_COMPUTE_FAST_FIRST: optimized rebuilds of compute pipelines built unoptimized.
    Common::ThreadWorker optimize_workers;
    Common::ThreadWorker serialization_thread;
    DynamicFeatures dynamic_features;

    /// Cached graphics pipelines boot left out are built on background threads once the game
    /// runs (EDEN_NX2_BACKGROUND_PIPELINES), so a draw that needs one takes it built instead of
    /// translating it on the GPU thread.
    void StartBackgroundPipelines(
        std::vector<std::pair<GraphicsPipelineCacheKey, std::vector<VideoCommon::FileEnvironment>>>
            pipelines);
    std::unique_ptr<GraphicsPipeline> TakeBackgroundPipeline(const GraphicsPipelineCacheKey& key);
    size_t DropBackgroundPipelines();
    void StopBackgroundPipelines();
    std::mutex background_mutex;
    /// Key hashes of cached graphics pipelines whose records lack a value this build's
    /// translation reads (EDEN_NX2_CACHE_REPAIR): the game's build of one is stored again.
    std::unordered_set<u64> incomplete_records; ///< Under background_mutex
    bool cache_repair{};
    /// Built and not asked for by a draw yet.
    std::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> background_ready;
    /// Keys the GPU thread built itself meanwhile: their background build is skipped or dropped.
    std::unordered_set<GraphicsPipelineCacheKey> background_claimed;
    size_t background_built{};    ///< Under background_mutex
    size_t background_taken{};    ///< Under background_mutex
    size_t background_left_out{}; ///< Under background_mutex: not built, memory was low
    std::atomic_bool background_active{};
    std::atomic_bool background_stop{};
    std::atomic_bool background_low_memory{};
    std::atomic<size_t> background_left{};
    std::chrono::steady_clock::time_point background_start{};
    /// Last: destroyed (its threads joined) before the state above.
    std::unique_ptr<Common::ThreadWorker> background_workers;
};

} // namespace Vulkan
