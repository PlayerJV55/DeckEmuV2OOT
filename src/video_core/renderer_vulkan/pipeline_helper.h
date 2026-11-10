// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/thread.h"
#include "common/unique_function.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

using Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS;

[[nodiscard]] inline std::optional<PixelFormat> PixelFormatFromImageFormat(
    Shader::ImageFormat format) {
    switch (format) {
    case Shader::ImageFormat::Typeless:
        return std::nullopt;
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8_UINT;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8_SINT;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::R32G32B32A32_UINT;
    }
    return std::nullopt;
}

// EDEN_NX2_MESH_PUSH_LEGACY=1 leaves the task and mesh stages out of graphics push constants, as
// before.
[[nodiscard]] inline bool MeshPushLegacy() {
    static const bool legacy = [] {
        const char* value = std::getenv("EDEN_NX2_MESH_PUSH_LEGACY");
        return value != nullptr && value[0] == '1';
    }();
    return legacy;
}

/// Stages that see a graphics pipeline's push constants (rescaling and render area).
[[nodiscard]] inline VkShaderStageFlags GraphicsPushConstantStages(bool uses_mesh) {
    constexpr VkShaderStageFlags graphics = VK_SHADER_STAGE_ALL_GRAPHICS;
    constexpr VkShaderStageFlags mesh = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    return uses_mesh && !MeshPushLegacy() ? graphics | mesh : graphics;
}

// EDEN_NX2_BUILD_WAITED_PIPELINES=0 makes a draw or dispatch wait for its pipeline's turn on the
// builder workers, as before.
[[nodiscard]] inline bool BuildWaitedPipelines() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_BUILD_WAITED_PIPELINES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// EDEN_NX2_PIPELINE_BOOST=0 leaves a builder worker below normal priority while a draw or
// dispatch waits for its build.
[[nodiscard]] inline bool BoostWaitedPipelineBuilds() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PIPELINE_BOOST");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

/// A pipeline build queued on the builder workers. A draw or dispatch that waits for it runs it on
/// the scheduler thread instead of waiting for the queue, which can hold hundreds of builds, to
/// reach it. The queued work holds this and not the pipeline, which may be gone by then.
class QueuedPipelineBuild {
public:
    explicit QueuedPipelineBuild(Common::UniqueFunction<void> build_) : build{std::move(build_)} {}

    /// Runs the build unless the worker or a waiting draw already has.
    void Run() {
        if (claimed.exchange(true)) {
            return;
        }
        {
            std::scoped_lock lock{boost_mutex};
            builder = Common::GetCurrentThreadBoostId();
        }
        build();
        done.store(true, std::memory_order_release);
        done.notify_all();
        std::scoped_lock lock{boost_mutex};
        builder = 0;
        if (boosted) {
            Common::RestoreCurrentThreadPriority(previous_priority);
        }
    }

    /// A draw or dispatch waits for a build another thread runs: the builder workers run below
    /// normal priority, so that thread runs at the highest normal priority until it is done.
    void Boost() {
        if (!BoostWaitedPipelineBuilds()) {
            return;
        }
        std::scoped_lock lock{boost_mutex};
        if (builder != 0 && !boosted && builder != Common::GetCurrentThreadBoostId()) {
            boosted = Common::BoostThreadPriority(builder, previous_priority);
        }
    }

    /// Whether the build has run: nothing reaches the pipeline through this any more.
    [[nodiscard]] bool Done() const noexcept {
        return done.load(std::memory_order_acquire);
    }

    /// Drops the build unless a thread has started it. True when it was dropped.
    bool Cancel() {
        return !claimed.exchange(true);
    }

    /// Waits for a build a thread has started.
    void Wait() const {
        done.wait(false, std::memory_order_acquire);
    }

private:
    Common::UniqueFunction<void> build;
    std::atomic_bool claimed{false};
    std::atomic_bool done{false};
    std::mutex boost_mutex;
    u32 builder{};
    bool boosted{};
    s32 previous_priority{};
};

// Draws and dispatches that wait for the driver compile hold up the scheduler thread, which the
// pipeline cache's GPU thread timings don't see. Only the scheduler's worker calls this.
inline void ReportPipelineBuildWait(bool is_compute, std::chrono::steady_clock::duration elapsed,
                                    u64 key_hash, bool is_mesh) {
    static std::chrono::steady_clock::duration wait_total{};
    static std::chrono::steady_clock::duration wait_longest{};
    static u32 graphics_waits{}, compute_waits{}, logged_waits{};
    static auto wait_window = std::chrono::steady_clock::now();
    const auto to_ms = [](std::chrono::steady_clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    wait_total += elapsed;
    wait_longest = (std::max)(wait_longest, elapsed);
    ++(is_compute ? compute_waits : graphics_waits);
    if (elapsed >= std::chrono::milliseconds{20} && logged_waits < 200) {
        ++logged_waits;
        if (is_compute) {
            LOG_INFO(Render_Vulkan, "Dispatch waited {:.1f} ms for its compute pipeline build",
                     to_ms(elapsed));
        } else {
            LOG_INFO(Render_Vulkan, "Draw waited {:.1f} ms for graphics pipeline {:016x}{}",
                     to_ms(elapsed), key_hash, is_mesh ? " (task/mesh)" : "");
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - wait_window > std::chrono::seconds{10}) {
        // The first wait after 10 s logs it, so it can cover more.
        LOG_INFO(Render_Vulkan,
                 "Draws waiting for a pipeline build in the last {:.1f} s: {} graphics / {} "
                 "compute, {:.1f} ms total, {:.1f} ms longest",
                 std::chrono::duration<double>(now - wait_window).count(), graphics_waits,
                 compute_waits, to_ms(wait_total), to_ms(wait_longest));
        wait_total = {};
        wait_longest = {};
        graphics_waits = 0;
        compute_waits = 0;
        wait_window = now;
    }
}

[[nodiscard]] inline VkDeviceSize DescriptorSizeForType(const Device& device,
                                                        VkDescriptorType type) {
    const auto& props = device.DescriptorBufferProperties();
    const bool robust = device.IsRobustBufferAccessEnabled();
    switch (type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        return robust ? props.robustUniformBufferDescriptorSize : props.uniformBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        return robust ? props.robustStorageBufferDescriptorSize : props.storageBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        return robust ? props.robustUniformTexelBufferDescriptorSize
                      : props.uniformTexelBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return robust ? props.robustStorageTexelBufferDescriptorSize
                      : props.storageTexelBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        return props.combinedImageSamplerDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        return props.storageImageDescriptorSize;
    default:
        return 0;
    }
}

struct DescriptorBufferBinding {
    VkDescriptorType type;
    u32 count;
    VkDeviceSize offset;
    VkDeviceSize stride;
};

struct DescriptorBufferLayout {
    VkDeviceSize size{};
    boost::container::small_vector<DescriptorBufferBinding, 32> bindings;

    [[nodiscard]] bool Empty() const noexcept {
        return bindings.empty();
    }
};

inline void WriteDescriptorBuffer(const Device& device, const DescriptorBufferLayout& layout,
                                  const DescriptorUpdateEntry* payload, u8* host) {
    const vk::Device& dev = device.GetLogical();
    for (const DescriptorBufferBinding& binding : layout.bindings) {
        for (u32 index = 0; index < binding.count; ++index) {
            const DescriptorUpdateEntry& entry = *(payload++);
            const VkDescriptorAddressInfoEXT address_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
                .pNext = nullptr,
                .address = entry.address.address,
                .range = entry.address.range,
                .format = entry.address.format,
            };
            VkDescriptorGetInfoEXT get_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                .pNext = nullptr,
                .type = binding.type,
                .data{},
            };
            switch (binding.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                get_info.data.pUniformBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                get_info.data.pStorageBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                get_info.data.pUniformTexelBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                get_info.data.pStorageTexelBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                get_info.data.pCombinedImageSampler = &entry.image;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                get_info.data.pStorageImage = &entry.image;
                break;
            default:
                continue;
            }
            dev.GetDescriptorEXT(get_info, binding.stride,
                                 host + binding.offset + index * binding.stride);
        }
    }
}

[[nodiscard]] inline u32 NumDescriptorEntries(const Shader::Info& info) {
    return Shader::NumDescriptors(info.constant_buffer_descriptors) +
           Shader::NumDescriptors(info.storage_buffers_descriptors) +
           Shader::NumDescriptors(info.texture_buffer_descriptors) +
           Shader::NumDescriptors(info.image_buffer_descriptors) +
           Shader::NumDescriptors(info.texture_descriptors) +
           Shader::NumDescriptors(info.image_descriptors);
}

class DescriptorLayoutBuilder {
public:
    DescriptorLayoutBuilder(const Device& device_) : device{&device_} {}

    bool CanUsePushDescriptor() const noexcept {
        if (!device->IsKhrPushDescriptorSupported() ||
            num_descriptors > device->MaxPushDescriptors()) {
            return false;
        }
        return !device->IsExtDescriptorBufferSupported() ||
               device->DescriptorBufferProperties().bufferlessPushDescriptors;
    }

    bool CanUseDescriptorBuffer() const noexcept {
        const auto& props = device->DescriptorBufferProperties();
        if (!device->IsExtDescriptorBufferSupported() || bindings.empty() ||
            !props.combinedImageSamplerDescriptorSingleArray) {
            return false;
        }
        return !props.bufferlessPushDescriptors || !CanUsePushDescriptor();
    }

    DescriptorBufferLayout MakeDescriptorBufferLayout(VkDescriptorSetLayout layout) const {
        DescriptorBufferLayout result;
        if (!layout) {
            return result;
        }
        const vk::Device& dev = device->GetLogical();
        result.size = dev.GetDescriptorSetLayoutSizeEXT(layout);
        result.bindings.reserve(bindings.size());
        for (const VkDescriptorSetLayoutBinding& entry : bindings) {
            result.bindings.push_back(DescriptorBufferBinding{
                .type = entry.descriptorType,
                .count = entry.descriptorCount,
                .offset = dev.GetDescriptorSetLayoutBindingOffsetEXT(layout, entry.binding),
                .stride = DescriptorSizeForType(*device, entry.descriptorType),
            });
        }
        return result;
    }

    vk::DescriptorSetLayout CreateDescriptorSetLayout(bool use_push_descriptor,
                                                      bool use_descriptor_buffer = false) const {
        if (bindings.empty()) {
            return nullptr;
        }
        VkDescriptorSetLayoutCreateFlags flags = 0;
        if (use_push_descriptor) {
            flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        }
        if (use_descriptor_buffer) {
            flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        }
        boost::container::small_vector<VkDescriptorBindingFlags, 32> binding_flags;
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_ci{};
        const void* pnext = nullptr;
        if (!use_push_descriptor && device->IsDescriptorBindingPartiallyBoundSupported()) {
            binding_flags.assign(bindings.size(), VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
            binding_flags_ci = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                .pNext = nullptr,
                .bindingCount = static_cast<u32>(binding_flags.size()),
                .pBindingFlags = binding_flags.data(),
            };
            pnext = &binding_flags_ci;
        }
        return device->GetLogical().CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = pnext,
            .flags = flags,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
    }

    vk::DescriptorUpdateTemplate CreateTemplate(VkDescriptorSetLayout descriptor_set_layout,
                                                VkPipelineLayout pipeline_layout,
                                                bool use_push_descriptor) const {
        if (entries.empty()) {
            return nullptr;
        }
        const VkDescriptorUpdateTemplateType type =
            use_push_descriptor ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
                                : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        return device->GetLogical().CreateDescriptorUpdateTemplate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
            .pDescriptorUpdateEntries = entries.data(),
            .templateType = type,
            .descriptorSetLayout = descriptor_set_layout,
            .pipelineBindPoint =
                is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pipelineLayout = pipeline_layout,
            .set = 0,
        });
    }

    VkPushConstantRange PushConstantRange() const {
        using Shader::Backend::SPIRV::RenderAreaLayout;
        using Shader::Backend::SPIRV::RescalingLayout;
        const u32 size_offset = is_compute ? sizeof(RescalingLayout::down_factor) : 0u;
        return VkPushConstantRange{
            .stageFlags = is_compute ? static_cast<VkShaderStageFlags>(VK_SHADER_STAGE_COMPUTE_BIT)
                                     : GraphicsPushConstantStages(uses_mesh),
            .offset = 0,
            .size = static_cast<u32>(sizeof(RescalingLayout)) - size_offset +
                    static_cast<u32>(sizeof(RenderAreaLayout)),
        };
    }

    /// What CreateDescriptorSetLayout and CreatePipelineLayout make of these bindings: layouts
    /// with the same signature are identically defined (graphics pipeline libraries).
    std::string LayoutSignature(bool use_push_descriptor, bool use_descriptor_buffer) const {
        std::string signature;
        const auto add = [&signature](u32 value) {
            signature.append(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        const VkPushConstantRange range{PushConstantRange()};
        add(range.stageFlags);
        add(range.offset);
        add(range.size);
        add(use_push_descriptor ? 1U : 0U);
        add(use_descriptor_buffer ? 1U : 0U);
        add(device->IsDescriptorBindingPartiallyBoundSupported() ? 1U : 0U);
        add(static_cast<u32>(bindings.size()));
        for (const VkDescriptorSetLayoutBinding& entry : bindings) {
            add(entry.binding);
            add(static_cast<u32>(entry.descriptorType));
            add(entry.descriptorCount);
            add(entry.stageFlags);
        }
        return signature;
    }

    vk::PipelineLayout CreatePipelineLayout(VkDescriptorSetLayout descriptor_set_layout) const {
        const VkPushConstantRange range{PushConstantRange()};
        return device->GetLogical().CreatePipelineLayout({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = descriptor_set_layout ? 1U : 0U,
            .pSetLayouts = bindings.empty() ? nullptr : &descriptor_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
    }

    void Add(const Shader::Info& info, VkShaderStageFlags stage) {
        is_compute |= (stage & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
        uses_mesh |= (stage & (VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)) != 0;

        Add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stage, info.constant_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage, info.storage_buffers_descriptors);
        Add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, stage, info.texture_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, stage, info.image_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, info.texture_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stage, info.image_descriptors);
    }

private:
    template <typename Descriptors>
    void Add(VkDescriptorType type, VkShaderStageFlags stage, const Descriptors& descriptors) {
        const size_t num{descriptors.size()};
        for (size_t i = 0; i < num; ++i) {
            bindings.push_back({
                .binding = binding,
                .descriptorType = type,
                .descriptorCount = descriptors[i].count,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = descriptors[i].count,
                .descriptorType = type,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            num_descriptors += descriptors[i].count;
            offset += sizeof(DescriptorUpdateEntry) * descriptors[i].count;
        }
    }

    const Device* device{};
    bool is_compute{};
    bool uses_mesh{};
    boost::container::small_vector<VkDescriptorSetLayoutBinding, 32> bindings;
    boost::container::small_vector<VkDescriptorUpdateTemplateEntry, 32> entries;
    u32 binding{};
    u32 num_descriptors{};
    size_t offset{};
};

class RescalingPushConstant {
public:
    explicit RescalingPushConstant() noexcept {}

    void PushTexture(bool is_rescaled) noexcept {
        *texture_ptr |= is_rescaled ? texture_bit : 0u;
        texture_bit <<= 1u;
        if (texture_bit == 0u) {
            texture_bit = 1u;
            ++texture_ptr;
        }
    }

    void PushImage(bool is_rescaled) noexcept {
        *image_ptr |= is_rescaled ? image_bit : 0u;
        image_bit <<= 1u;
        if (image_bit == 0u) {
            image_bit = 1u;
            ++image_ptr;
        }
    }

    const std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>& Data() const noexcept {
        return words;
    }

    /// The words a back split worker pushed for this draw (EDEN_NX2_BACK_SPLIT); push no more.
    void Assign(const std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>& data) noexcept {
        words = data;
    }

private:
    std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> words{};
    u32* texture_ptr{words.data()};
    u32* image_ptr{words.data() + Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS};
    u32 texture_bit{1u};
    u32 image_bit{1u};
};

class RenderAreaPushConstant {
public:
    bool uses_render_area{};
    std::array<f32, 4> words{};
};

/// Pushes a stage's sampled and storage image descriptors to `queue` (the guest descriptor queue
/// or a back split worker's ImageDescriptorSink) and passes `mark_written` each image a storage
/// image descriptor writes.
template <typename Queue, typename MarkWritten>
inline void PushImageDescriptors(TextureCache& texture_cache, Queue& queue,
                                 const Shader::Info& info, RescalingPushConstant& rescaling,
                                 const VideoCommon::SamplerId*& samplers,
                                 const VideoCommon::ImageViewInOut*& views,
                                 MarkWritten&& mark_written) {
    const u32 num_texture_buffers = Shader::NumDescriptors(info.texture_buffer_descriptors);
    const u32 num_image_buffers = Shader::NumDescriptors(info.image_buffer_descriptors);
    views += num_texture_buffers;
    views += num_image_buffers;
    for (const auto& desc : info.texture_descriptors) {
        bool is_rescaled{};
        for (u32 index = 0; index < desc.count; ++index) {
            const VideoCommon::ImageViewId image_view_id{(views++)->id};
            const VideoCommon::SamplerId sampler_id{*(samplers++)};
            ImageView& image_view{texture_cache.GetImageView(image_view_id)};
            VkImageView vk_image_view{image_view.Handle(desc.type)};
            if (vk_image_view == VK_NULL_HANDLE) {
                const VkImageView null_image_view{texture_cache.GetImageView(VideoCommon::NULL_IMAGE_VIEW_ID).Handle(desc.type)};
                if (null_image_view != VK_NULL_HANDLE) vk_image_view = null_image_view;
            }
            Sampler& sampler{texture_cache.GetSampler(sampler_id)};
            queue.AddSampledImage(vk_image_view, sampler.HandleFor(image_view, desc.is_depth));
            const bool element_rescaled{texture_cache.IsRescaling(image_view)};
            is_rescaled |= element_rescaled;
        }
        rescaling.PushTexture(is_rescaled);
    }
    for (const auto& desc : info.image_descriptors) {
        bool is_rescaled{};
        for (u32 index = 0; index < desc.count; ++index) {
            ImageView& image_view{texture_cache.GetImageView((views++)->id)};
            if (desc.is_written) {
                mark_written(image_view.image_id);
            }
            const VkImageView vk_image_view{image_view.StorageView(desc.type, desc.format)};
            queue.AddImage(vk_image_view);
            const bool element_rescaled{texture_cache.IsRescaling(image_view)};
            is_rescaled |= element_rescaled;
        }
        rescaling.PushImage(is_rescaled);
    }
}

inline void PushImageDescriptors(TextureCache& texture_cache,
                                 GuestDescriptorQueue& guest_descriptor_queue,
                                 const Shader::Info& info, RescalingPushConstant& rescaling,
                                 const VideoCommon::SamplerId*& samplers,
                                 const VideoCommon::ImageViewInOut*& views) {
    PushImageDescriptors(texture_cache, guest_descriptor_queue, info, rescaling, samplers, views,
                         [&texture_cache](VideoCommon::ImageId image_id) {
                             texture_cache.MarkModification(image_id);
                         });
}

} // namespace Vulkan
