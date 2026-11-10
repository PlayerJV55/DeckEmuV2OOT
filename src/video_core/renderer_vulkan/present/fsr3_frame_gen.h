// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// AMD FidelityFX Super Resolution 3 (FSR 3) Frame Generation pipeline.
/// Uses compute-based optical flow and bilateral frame interpolation
/// to synthesize intermediate frames without requiring engine motion vectors.
///
/// Optimizations:
///  - Half-resolution optical flow (4x fewer compute invocations)
///  - Shared-memory tile caching in both shaders
///  - Early-out for static pixels (skip diamond search)
///  - Precise pipeline barriers instead of full-pipeline TransitionImageLayout
class Fsr3FrameGen {
public:
    explicit Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                          VkExtent2D extent, VkFormat format);
    ~Fsr3FrameGen();

    /// The interpolation pass stores to swapchain-format frames without a format qualifier.
    [[nodiscard]] static bool IsSupported(const Device& device);

    /// Records the copy of a new real frame and the optical flow against the previous one.
    void Process(Scheduler& scheduler, VkImage current_frame);

    /// Records an interpolated frame between the previous and the current real frame.
    /// The descriptor set of destination_index is rewritten, so that frame must be idle.
    void Generate(const Device& device, Scheduler& scheduler, VkImage destination,
                  VkImageView destination_storage_view, u32 destination_index,
                  float blend_factor);

    /// True once two real frames were processed and generated frames are meaningful.
    [[nodiscard]] bool HasFlow() const {
        return m_flow_valid;
    }

private:
    static constexpr size_t FRAME_SLOTS = 2;

    void CreateResources(const Device& device);
    void CreateShaders(const Device& device);
    void CreatePipelines(const Device& device);
    void WriteFlowDescriptorSets(const Device& device);

    MemoryAllocator& m_allocator;
    VkExtent2D m_extent;
    VkExtent2D m_flow_extent;   // half-res for optical flow
    VkFormat m_format;

    vk::Sampler m_linear_sampler;
    vk::ShaderModule m_flow_shader;
    vk::ShaderModule m_interp_shader;

    vk::DescriptorSetLayout m_flow_desc_layout;
    vk::PipelineLayout m_flow_pipe_layout;
    vk::Pipeline m_flow_pipeline;

    vk::DescriptorSetLayout m_interp_desc_layout;
    vk::PipelineLayout m_interp_pipe_layout;
    vk::Pipeline m_interp_pipeline;

    vk::DescriptorPool m_desc_pool;
    // One flow set per slot holding the current frame; never rewritten
    vk::DescriptorSets m_flow_desc_sets;
    // One interpolation set per present frame index
    vk::DescriptorSets m_interp_desc_sets;

    // Copies of the last two real frames, ping-ponged so the previous one survives
    std::array<vk::Image, FRAME_SLOTS> m_frame_images;
    std::array<vk::ImageView, FRAME_SLOTS> m_frame_views;
    size_t m_current_slot{};

    vk::Image m_motion_image;
    vk::ImageView m_motion_view;

    bool m_has_history{};
    bool m_flow_valid{};
};

} // namespace Vulkan
