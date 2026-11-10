// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/evicted_pointer_slots.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_library.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

class BackSplit;
class Device;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

/// Graphics pipeline libraries (EDEN_NX2_GPL): run-time builds pass the library cache, which
/// links the pipeline from shader libraries, the SPIR-V hashes of the stages emitted before the
/// pipeline is made, and the worker optimized links run on (EDEN_NX2_GPL_OPTIMIZE). Declared
/// outside the class for GCC, which rejects a nested struct with member initializers as a default
/// argument inside its class (Lichii 140c579aaa).
struct GraphicsPipelineLibraryParams {
    GraphicsPipelineLibraryCache* library_cache{};
    std::array<u128, Tegra::Engines::Maxwell3D::Regs::MaxShaderStage> spirv_hashes{};
    Common::ThreadWorker* optimize_worker{};
};

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    /// Emits the stages' SPIR-V on the build's thread, writing the hashes of what it emitted when
    /// the pipeline is linked from libraries.
    using EmitStagesFunc = std::function<std::array<vk::ShaderModule, NUM_STAGES>(
        std::array<u128, NUM_STAGES>& spirv_hashes)>;

    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue,
        DescriptorBufferRing& descriptor_buffer_ring, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        const GraphicsPipelineCacheKey& key, std::array<vk::ShaderModule, NUM_STAGES> stages,
        const std::array<const Shader::Info*, NUM_STAGES>& infos, EmitStagesFunc emit_stages = {},
        GraphicsPipelineLibraryParams library_params = {});
    ~GraphicsPipeline();

    bool HasDynamicVertexInput() const noexcept { return key.state.dynamic_vertex_input; }
    const std::array<u64, 6>& ShaderHashes() const noexcept {
        return key.unique_hashes;
    }
    bool SupportsAlphaToCoverage() const noexcept {
        return fragment_has_color0_output;
    }

    bool SupportsAlphaToOne() const noexcept {
        return fragment_has_color0_output;
    }

    bool UsesExtendedDynamicState() const noexcept {
        return key.state.extended_dynamic_state != 0;
    }

    /// Switch 2 task/mesh pipeline: drawn with vkCmdDrawMeshTasksEXT.
    [[nodiscard]] bool IsMeshPipeline() const noexcept {
        return stage_infos[0].is_task || stage_infos[0].is_mesh;
    }
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(const GraphicsPipelineCacheKey& transition_key,
                       GraphicsPipeline* transition);

    /// `back_split`: the draw's EDEN_NX2_BACK_SPLIT pool, or nullptr.
    bool Configure(bool is_indexed, BackSplit* back_split) {
        return configure_func(this, is_indexed, back_split);
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        const auto it{std::find(transition_keys.begin(), transition_keys.end(), current_key)};
        return it != transition_keys.end() ? transitions[std::distance(transition_keys.begin(), it)]
                                           : nullptr;
    }

    /// The frame the pipeline cache last handed this pipeline out (evicts long unused ones).
    void MarkUsed(u64 frame) noexcept {
        last_use_frame = frame;
        used = true;
    }

    [[nodiscard]] u64 LastUse() const noexcept {
        return last_use_frame;
    }

    /// Whether the cache has handed this pipeline out at all (frame 0 included), as opposed to a
    /// pipeline prebuilt from the disk cache that no draw has used yet.
    [[nodiscard]] bool WasUsed() const noexcept {
        return used;
    }

    /// Bit i: the vertex stage reads generic attribute i (attributes it doesn't read are unused).
    [[nodiscard]] u32 GenericAttributesRead() const noexcept {
        u32 mask = 0;
        for (size_t index = 0; index < 32; ++index) {
            mask |= stage_infos[0].loads.Generic(index) ? 1U << index : 0U;
        }
        return mask;
    }

    /// The constant buffer slots each program binds as storage buffer pointers
    [[nodiscard]] const VideoCommon::EvictedPointerSlots::Slots& MappedPointers() const noexcept {
        return mapped_pointers;
    }

    void SetMappedPointers(VideoCommon::EvictedPointerSlots::Slots slots) noexcept {
        mapped_pointers = std::move(slots);
    }

    /// Forgets the transitions, which may point to pipelines the cache evicted.
    void ClearTransitions() noexcept {
        transition_keys.clear();
        transitions.clear();
    }

    /// Acquire: a caller that sees it built also sees the pipeline the builder made.
    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::acquire);
    }

    /// Some stage writes storage buffers, images or global memory: a skipped draw would lose data.
    [[nodiscard]] bool WritesStorage() const noexcept {
        return writes_storage;
    }

    /// Writes storage images or image buffers (their images are marked modified when resolved).
    [[nodiscard]] bool WritesImages() const noexcept {
        return writes_images;
    }

    /// Why TakesDeltaDraws is false (diagnostics).
    enum class DeltaBlocker : u8 {
        None,
        WritesImages,
        Mesh,
        ComputedHandles,
        NotConfigured,
        Count,
    };

    /// Fast draw (nx2_fast_draw.h): whether draws can keep the image views and samplers the last
    /// captured Configure resolved. Handles computed at draw time are resolved on every draw.
    [[nodiscard]] bool TakesDeltaDraws() const noexcept {
        return delta_blocker == DeltaBlocker::None && delta_views_valid;
    }

    [[nodiscard]] DeltaBlocker DeltaDrawBlocker() const noexcept {
        return delta_blocker != DeltaBlocker::None ? delta_blocker
               : delta_views_valid                 ? DeltaBlocker::None
                                                   : DeltaBlocker::NotConfigured;
    }

    /// Constant buffer slots per stage the pipeline reads texture and image handles from.
    [[nodiscard]] const std::array<u32, NUM_STAGES>& HandleConstantBuffers() const noexcept {
        return handle_constant_buffers;
    }

    /// Resolves and binds every buffer as Configure does (storage buffer addresses are read from
    /// the constant buffers again) and records the draw's descriptors with the image views,
    /// samplers and texture buffers the last captured Configure resolved. False when the draw
    /// can't be recorded, as for Configure.
    bool ConfigureDelta(bool is_indexed);

    /// The next Configure keeps the views and samplers it resolves for delta draws. With verify,
    /// it first compares them with the ones kept before; DeltaMismatch reports it.
    void CaptureNextDelta(bool verify) noexcept {
        capture_delta = true;
        verify_delta = verify;
    }

    [[nodiscard]] bool DeltaMismatch() const noexcept {
        return delta_mismatch;
    }

    /// Whether an image the kept views sample has an upload pending or a newer alias to copy in:
    /// ConfigureDelta prepares none, Configure does.
    [[nodiscard]] bool DeltaImagesChange() const noexcept;

    [[nodiscard]] bool UsesTransformFeedback() const noexcept {
        return key.state.xfb_enabled != 0;
    }

    /// Waits up to timeout for the build to finish; true when it is built.
    [[nodiscard]] bool WaitForBuild(std::chrono::microseconds timeout) {
        std::unique_lock lock{build_mutex};
        return build_condvar.wait_for(lock, timeout,
                                      [this] { return is_built.load(std::memory_order::relaxed); });
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed, BackSplit* back_split) {
            return pl->ConfigureImpl<Spec>(is_indexed, back_split);
        };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

private:
    template <typename Spec>
    bool ConfigureImpl(bool is_indexed, BackSplit* back_split);

    bool ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    struct PipelineStates;

    /// The fixed-function state of the pipeline, shared by the monolithic and library builds.
    void FillStates(PipelineStates& states) const;

    void MakePipeline(VkRenderPass render_pass);

    /// Graphics pipeline libraries (EDEN_NX2_GPL): a shader library of this pipeline's stages in
    /// `part`, and the key it is shared under.
    GplLibrary MakeShaderLibrary(GplPart part, const PipelineStates& states,
                                 VkRenderPass render_pass) const;
    std::string ShaderLibraryKey(GplPart part, const PipelineStates& states,
                                 VkRenderPass render_pass) const;

    /// Links the pipeline from its shader libraries and its own vertex input and fragment output
    /// libraries. False when the pipeline takes the monolithic route (task/mesh, transform
    /// feedback, static rasterizer discard) or a library or the link failed.
    bool MakeLibraryPipeline(VkRenderPass render_pass);

    vk::Pipeline LinkLibraries(VkRenderPass render_pass, bool optimized) const;

    void Validate();

    const GraphicsPipelineCacheKey key;
    Tegra::Engines::Maxwell3D* maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;

    bool (*configure_func)(GraphicsPipeline*, bool, BackSplit*){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;
    VideoCommon::EvictedPointerSlots::Slots mapped_pointers;
    u64 last_use_frame{};
    bool used{};

    std::array<vk::ShaderModule, NUM_STAGES> spv_modules;
    /// The stages' SPIR-V hashes, what shader libraries are keyed by (zero for absent stages).
    std::array<u128, NUM_STAGES> spirv_hashes{};

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_descriptor_entries{};
    size_t num_image_elements{};
    u32 num_textures{};
    bool fragment_has_color0_output{};
    std::array<bool, 8> fragment_writes_color{};
    /// A stage stores to memory (global memory, storage buffers or images).
    bool writes_storage{};
    bool writes_images{};

    DeltaBlocker delta_blocker{};
    bool delta_views_valid{};
    bool capture_delta{};
    bool verify_delta{};
    bool delta_mismatch{};
    std::array<bool, NUM_STAGES> configured_stages{};
    std::array<u32, NUM_STAGES> handle_constant_buffers{};
    std::vector<VideoCommon::ImageViewInOut> delta_views;
    std::vector<VideoCommon::SamplerId> delta_samplers;

    /// Graphics pipeline libraries (EDEN_NX2_GPL). The libraries are declared before the pipelines
    /// linked from them, so they are destroyed after them.
    GraphicsPipelineLibraryCache* const library_cache;
    Common::ThreadWorker* const optimize_worker;
    std::array<std::shared_ptr<const GplLibrary>, 2> shader_libraries;
    vk::Pipeline vertex_input_library;
    vk::Pipeline fragment_output_library;

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;
    /// EDEN_NX2_GPL_OPTIMIZE: the optimized link of the libraries, bound in place of the fast
    /// linked pipeline from the first draw after it is ready. Both stay alive.
    vk::Pipeline optimized_pipeline;
    std::shared_ptr<QueuedPipelineBuild> optimized_link;
    std::atomic_bool optimized_ready{false};
    bool optimized_swapped{}; ///< GPU thread only
    bool optimized_bound{};   ///< Scheduler thread only

    DescriptorBufferLayout descriptor_buffer_layout;
    std::vector<DescriptorUpdateEntry> last_descriptor_payload;
    VkDeviceSize last_descriptor_buffer_offset{};
    u32 last_descriptor_buffer_chunk{};
    u64 last_descriptor_buffer_generation{};

    std::shared_ptr<QueuedPipelineBuild> queued_build;
    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
};

} // namespace Vulkan
