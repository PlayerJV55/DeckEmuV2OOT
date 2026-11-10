// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/math_util.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// The ScalingFilter::Fsr3 upscaler. Despite its name it runs AMD FidelityFX Super
/// Resolution 1, the same two passes as the FSR filter, with a little more sharpening:
///   1. **EASU** - edge-adaptive spatial upsampling.
///   2. **RCAS** - robust contrast-adaptive sharpening on the final output.
/// There is no temporal accumulation and no optical flow.
class FSR3 {
public:
    explicit FSR3(const Device& device, MemoryAllocator& memory_allocator,
                  size_t image_count, VkExtent2D extent);
    ~FSR3();

    /// Run the full FSR 3 pipeline and return the upscaled image view.
    VkImageView Draw(const Device& device, Scheduler& scheduler, size_t image_index,
                     VkImage source_image, VkImageView source_image_view,
                     VkExtent2D input_image_extent,
                     const Common::Rectangle<f32>& crop_rect);

private:
    // ΓöÇΓöÇ resource creation helpers ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    void CreateImages(const Device& device);
    void CreateRenderPasses(const Device& device);
    void CreateSampler(const Device& device);
    void CreateShaders(const Device& device);
    void CreateDescriptorPool(const Device& device);
    void CreateDescriptorSetLayout(const Device& device);
    void CreateDescriptorSets(const Device& device);
    void CreatePipelineLayouts(const Device& device);
    void CreatePipelines(const Device& device);

    // ΓöÇΓöÇ per-frame helpers ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    void UploadImages(const Device& device, Scheduler& scheduler);
    void UpdateDescriptorSets(const Device& device, VkImageView image_view, size_t image_index);

    // ΓöÇΓöÇ members ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
    MemoryAllocator& m_memory_allocator;
    const size_t m_image_count;
    const VkExtent2D m_extent;

    enum Stage {
        Easu,     // Edge-adaptive spatial upsampling
        Rcas,     // Robust contrast-adaptive sharpening
        MaxStage,
    };

    vk::DescriptorPool m_descriptor_pool;
    vk::DescriptorSetLayout m_descriptor_set_layout;
    vk::PipelineLayout m_pipeline_layout;
    vk::ShaderModule m_vert_shader;
    vk::ShaderModule m_easu_shader;
    vk::ShaderModule m_rcas_shader;
    vk::ShaderModule m_area_shader; ///< First pass when downscaling (FsrAreaDownscale)
    vk::Pipeline m_easu_pipeline;
    vk::Pipeline m_rcas_pipeline;
    vk::Pipeline m_area_pipeline;
    vk::RenderPass m_renderpass;
    vk::Sampler m_sampler;

    struct Images {
        vk::DescriptorSets descriptor_sets;
        std::array<vk::Image, MaxStage> images;
        std::array<vk::ImageView, MaxStage> image_views;
        std::array<vk::Framebuffer, MaxStage> framebuffers;
    };
    std::vector<Images> m_dynamic_images;

    bool m_images_ready{};
    bool m_has_prev_frame{};
    u32 m_frame_counter{};
};

} // namespace Vulkan
