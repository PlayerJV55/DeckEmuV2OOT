// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <vector>

#ifdef __ANDROID__
#include <android/api-level.h>
#endif

#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "common/nx2_title_defaults.h"
#include "common/video_activity.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "vulkan/vulkan_core.h"

namespace Vulkan {

namespace {

VkSurfaceFormatKHR ChooseSwapSurfaceFormat(vk::Span<VkSurfaceFormatKHR> formats) {
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        VkSurfaceFormatKHR format;
        format.format = VK_FORMAT_B8G8R8A8_UNORM;
        format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return format;
    }
    const auto& found = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    return found != formats.end() ? *found : formats[0];
}

#ifndef __ANDROID__
// EDEN_NX2_FG_KEEP_FIFO=1 keeps a FIFO VSync mode with frame generation, as before.
bool FrameGenKeepsFifo() {
    static const bool keep = [] {
        const char* value = std::getenv("EDEN_NX2_FG_KEEP_FIFO");
        return value != nullptr && value[0] == '1';
    }();
    return keep;
}
#endif

static VkPresentModeKHR ChooseSwapPresentMode(bool has_imm, bool has_mailbox,
                                              bool has_fifo_relaxed) {
    // Mailbox doesn't lock the application like FIFO (vsync)
    // FIFO present mode locks the framerate to the monitor's refresh rate
    Settings::VSyncMode setting = [has_imm, has_mailbox]() {
        // Choose Mailbox or Immediate if unlocked and those modes are supported
        const auto mode = Settings::values.vsync_mode.GetValue();
        if (Settings::values.frame_gen.GetValue()) {
#ifndef __ANDROID__
            // Frame generation always runs the present thread, which spaces generated frames
            // itself. Immediate would tear them; Mailbox keeps that spacing without tearing or
            // blocking the thread on vblank.
            if (mode == Settings::VSyncMode::Immediate) {
                return has_mailbox ? Settings::VSyncMode::Mailbox : Settings::VSyncMode::Fifo;
            }
            // Under FIFO every generated frame waits in line for a vblank of its own, and the
            // real frame queued behind them waits for all of them. Without Mailbox the user's mode
            // stays: FIFO relaxed would still queue them and only tear the frames that come late.
            if (!FrameGenKeepsFifo() && has_mailbox &&
                (mode == Settings::VSyncMode::Fifo || mode == Settings::VSyncMode::FifoRelaxed)) {
                return Settings::VSyncMode::Mailbox;
            }
            return mode;
#else
            // Without the present thread generated frames go out back to back, and FIFO gives
            // each its own refresh. Android keeps FIFO as before.
            return mode == Settings::VSyncMode::FifoRelaxed ? mode : Settings::VSyncMode::Fifo;
#endif
        }
        if (Settings::values.use_speed_limit.GetValue() &&
            Settings::values.current_speed_mode.GetValue() != Settings::SpeedMode::Turbo) {
            return mode;
        }
        switch (mode) {
        case Settings::VSyncMode::Fifo:
        case Settings::VSyncMode::FifoRelaxed:
            if (has_mailbox) {
                return Settings::VSyncMode::Mailbox;
            } else if (has_imm) {
                return Settings::VSyncMode::Immediate;
            }
            [[fallthrough]];
        default:
            return mode;
        }
    }();
    if (setting == Settings::VSyncMode::Immediate && !has_imm) {
        setting = Settings::VSyncMode::Mailbox;
    }
    if ((setting == Settings::VSyncMode::Mailbox && !has_mailbox) ||
        (setting == Settings::VSyncMode::FifoRelaxed && !has_fifo_relaxed)) {
        setting = Settings::VSyncMode::Fifo;
    }

    switch (setting) {
    case Settings::VSyncMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case Settings::VSyncMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case Settings::VSyncMode::Fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    case Settings::VSyncMode::FifoRelaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, u32 width, u32 height) {
    constexpr auto undefined_size{(std::numeric_limits<u32>::max)()};
    if (capabilities.currentExtent.width != undefined_size) {
        return capabilities.currentExtent;
    }
    VkExtent2D extent;
    extent.width = (std::max)(capabilities.minImageExtent.width,
                            (std::min)(capabilities.maxImageExtent.width, width));
    extent.height = (std::max)(capabilities.minImageExtent.height,
                             (std::min)(capabilities.maxImageExtent.height, height));
    return extent;
}

VkCompositeAlphaFlagBitsKHR ChooseAlphaFlags(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown composite alpha flags value {:#x}",
                  capabilities.supportedCompositeAlpha);
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
}

} // Anonymous namespace

Swapchain::Swapchain(
    VkSurfaceKHR_T* surface_,
    const Device& device_,
    Scheduler& scheduler_,
    u32 width_,
    u32 height_)
    : surface(surface_)
    , device{device_}
    , scheduler{scheduler_}
{
    Create(surface, width_, height_);
}

Swapchain::~Swapchain() = default;

void Swapchain::Create(
    VkSurfaceKHR_T* surface_,
    u32 width_,
    u32 height_)
{
    is_outdated = false;
    is_suboptimal = false;
    width = width_;
    height = height_;
    surface = surface_;

    const auto physical_device = device.GetPhysical();
    const auto capabilities{physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface))};
    if (capabilities.maxImageExtent.width == 0 || capabilities.maxImageExtent.height == 0) {
        return;
    }

    // Submitted work may still present from the old images and wait on their render semaphores;
    // destroying them under it is out of spec and loses the device on MoltenVK.
    if (swapchain) {
        std::scoped_lock lock{scheduler.submit_mutex};
        const VkResult result = device.GetLogical().WaitIdle();
        if (result == VK_ERROR_DEVICE_LOST) {
            device.ReportLoss();
        }
        vk::Check(result);
    }
    Destroy();

    CreateSwapchain(capabilities);
    CreateSemaphores();

    resource_ticks.clear();
    resource_ticks.resize(image_count);
}

bool Swapchain::AcquireNextImage(bool may_flush, bool pace) {
    const VkResult result = device.GetLogical().AcquireNextImageKHR(
        *swapchain, (std::numeric_limits<u64>::max)(), *present_semaphores[frame_index],
        VK_NULL_HANDLE, &image_index);
    switch (result) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        is_suboptimal = true;
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));
        break;
    }

    WaitAcquired(resource_ticks[image_index], may_flush, pace);

    // The present thread (may_flush false) presents frames the render thread already flushed. Its
    // current tick is one the render thread is still recording, and waiting on it would spin.
    resource_ticks[image_index] = may_flush ? scheduler.CurrentTick() : scheduler.CurrentTick() - 1;

    return is_suboptimal || is_outdated;
}

Swapchain::AcquireResult Swapchain::AcquireNextImage(VkSemaphore semaphore, bool may_flush,
                                                     bool pace) {
    // Far longer than a vblank wait. Between waits the caller checks for a minimized window or a
    // stopping thread instead of holding WaitPresent and teardown.
    constexpr u64 acquire_timeout_ns = 100'000'000;
    const VkResult result = device.GetLogical().AcquireNextImageKHR(
        *swapchain, acquire_timeout_ns, semaphore, VK_NULL_HANDLE, &image_index);
    AcquireResult acquired = AcquireResult::Acquired;
    switch (result) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        // The image is acquired and the semaphore will be signaled, so the frame still goes out.
        is_suboptimal = true;
        acquired = AcquireResult::Suboptimal;
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        return AcquireResult::OutOfDate;
    case VK_TIMEOUT:
    case VK_NOT_READY:
        return AcquireResult::TimedOut;
    case VK_ERROR_SURFACE_LOST_KHR:
        throw vk::Exception(result);
    default:
        LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));
        return AcquireResult::Unavailable;
    }

    if (may_flush) {
        WaitAcquired(resource_ticks[image_index], true, pace);
        resource_ticks[image_index] = scheduler.CurrentTick();
    } else {
        // The copy waits for the frame on the GPU (render_ready), so the present thread only
        // keeps the frame rate target here instead of waiting for the GPU to reach a tick.
        WaitAcquired(0, false, pace);
    }
    return acquired;
}

void Swapchain::WaitAcquired(u64 tick, bool may_flush, bool pace) {
    const auto wait_with_frame_pacing = [this, tick, may_flush, pace] {
    // While a video decodes, the game runs at its own rate: a lower target slowed Ocarina of
    // Time's 60 fps intro movie to half speed against its audio. EDEN_NX2_PACING_VIDEO=0 keeps
    // the target then.
    static Common::Nx2::SessionValue<bool> keep_for_video{[] {
        return Common::Nx2::Setting("EDEN_NX2_PACING_VIDEO", "1").starts_with('0');
    }};
    if (!pace || (!keep_for_video.Get() && Common::VideoFramesRecent())) {
        scheduler.Wait(tick, 0.0, may_flush);
        return;
    }
    switch (Settings::values.frame_pacing_mode.GetValue()) {
    case Settings::FramePacingMode::Target_Auto:
        scheduler.Wait(tick, 0.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_30:
        scheduler.Wait(tick, 30.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_60:
        scheduler.Wait(tick, 60.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_90:
        scheduler.Wait(tick, 90.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_120:
        scheduler.Wait(tick, 120.0, may_flush);
        break;
    }
    };

#ifdef __ANDROID__
    if (android_get_device_api_level() >= 30) {
        scheduler.Wait(tick, 0.0, may_flush);
    } else {
        wait_with_frame_pacing();
    }
#else
    wait_with_frame_pacing();
#endif
}

u64 Swapchain::Present(VkSemaphore render_semaphore) {
    const auto present_queue{device.GetPresentQueue()};
    // Ids for vkWaitForPresentKHR (the host frame pacer). They only have to increase per
    // swapchain, so one counter serves every swapchain this object creates.
    const u64 present_id = device.IsKhrPresentWaitSupported() ? ++next_present_id : 0;
    const VkPresentIdKHR present_id_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .pNext = nullptr,
        .swapchainCount = 1,
        .pPresentIds = &present_id,
    };
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = present_id != 0 ? &present_id_info : nullptr,
        .waitSemaphoreCount = render_semaphore ? 1U : 0U,
        .pWaitSemaphores = &render_semaphore,
        .swapchainCount = 1,
        .pSwapchains = swapchain.address(),
        .pImageIndices = &image_index,
        .pResults = nullptr,
    };
    // Two threads must not use a queue at once. On the graphics queue that takes the submit lock,
    // which a present waiting for vblank would hold all that time.
    std::scoped_lock lock{device.HasSeparatePresentQueue() ? present_mutex
                                                           : scheduler.submit_mutex};
    bool queued = false;
    switch (const VkResult result = present_queue.Present(present_info)) {
    case VK_SUCCESS:
        queued = true;
        break;
    case VK_SUBOPTIMAL_KHR:
        LOG_DEBUG(Render_Vulkan, "Suboptimal swapchain");
        queued = true;
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));
        break;
    }
    ++frame_index;
    if (frame_index >= image_count) {
        frame_index = 0;
    }
    return queued ? present_id : 0;
}

void Swapchain::CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities) {
    const auto physical_device{device.GetPhysical()};
    const auto formats{physical_device.GetSurfaceFormatsKHR(VkSurfaceKHR(surface))};
    const auto present_modes = physical_device.GetSurfacePresentModesKHR(VkSurfaceKHR(surface));

    has_mailbox = std::find(present_modes.begin(), present_modes.end(), VK_PRESENT_MODE_MAILBOX_KHR)
                  != present_modes.end();
    has_imm = std::find(present_modes.begin(), present_modes.end(),
                        VK_PRESENT_MODE_IMMEDIATE_KHR) != present_modes.end();
    has_fifo_relaxed = std::find(present_modes.begin(), present_modes.end(),
                                 VK_PRESENT_MODE_FIFO_RELAXED_KHR) != present_modes.end();

    const VkCompositeAlphaFlagBitsKHR alpha_flags{ChooseAlphaFlags(capabilities)};
    surface_format = ChooseSwapSurfaceFormat(formats);
    present_mode = ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed);

    u32 requested_image_count{capabilities.minImageCount + 1};
    // Ensure Triple buffering if possible.
    if (capabilities.maxImageCount > 0) {
        if (requested_image_count > capabilities.maxImageCount) {
            requested_image_count = capabilities.maxImageCount;
        } else {
            requested_image_count =
                (std::max)(requested_image_count, (std::min)(3U, capabilities.maxImageCount));
        }
    } else {
        requested_image_count = (std::max)(requested_image_count, 3U);
    }
    // EDEN_NX2_SWAPCHAIN_IMAGES=<n> (opt-in): that many images instead, within what the surface
    // allows. Under FIFO each image past the one on screen can hold a finished frame back a
    // vblank: 2 keeps at most one waiting, a frame less of input lag than triple buffering.
    static const u32 forced_image_count = [] {
        const char* const value = std::getenv("EDEN_NX2_SWAPCHAIN_IMAGES");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
    }();
    if (forced_image_count != 0) {
        requested_image_count = (std::max)(forced_image_count, capabilities.minImageCount);
        if (capabilities.maxImageCount > 0) {
            requested_image_count = (std::min)(requested_image_count, capabilities.maxImageCount);
        }
    }
    VkSwapchainCreateInfoKHR swapchain_ci{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = VkSurfaceKHR(surface),
        .minImageCount = requested_image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = {},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
#ifdef __ANDROID__
        // On Android, do not allow surface rotation to deviate from the frontend.
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
#else
        .preTransform = capabilities.currentTransform,
#endif
        .compositeAlpha = alpha_flags,
        .presentMode = present_mode,
        .clipped = VK_FALSE,
        .oldSwapchain = VkSwapchainKHR{},
    };
    const u32 graphics_family{device.GetGraphicsFamily()};
    const u32 present_family{device.GetPresentFamily()};
    const std::array<u32, 2> queue_indices{graphics_family, present_family};
    if (graphics_family != present_family) {
        swapchain_ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_ci.queueFamilyIndexCount = static_cast<u32>(queue_indices.size());
        swapchain_ci.pQueueFamilyIndices = queue_indices.data();
    }
    // According to Vulkan spec, when using VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR,
    // the base format (imageFormat) MUST be included in pViewFormats
    const std::array view_formats{
        swapchain_ci.imageFormat,  // Base format MUST be first
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
#ifdef __ANDROID__
        VK_FORMAT_R8G8B8A8_UNORM,  // Android may use RGBA
        VK_FORMAT_R8G8B8A8_SRGB,
#endif
    };
    VkImageFormatListCreateInfo format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (device.IsKhrSwapchainMutableFormatEnabled()) {
        format_list.pNext = std::exchange(swapchain_ci.pNext, &format_list);
        swapchain_ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    }
    // Request the size again to reduce the possibility of a TOCTOU race condition.
    const auto updated_capabilities = physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface));
    swapchain_ci.imageExtent = ChooseSwapExtent(updated_capabilities, width, height);
    // Don't add code within this and the swapchain creation.
    swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);

    extent = swapchain_ci.imageExtent;

    images = swapchain.GetImages();
    image_count = static_cast<u32>(images.size());

    if (present_mode != logged_present_mode) {
        LOG_INFO(Render_Vulkan, "Swapchain: {}x{}, {} images, present mode {}", extent.width,
                 extent.height, image_count, string_VkPresentModeKHR(present_mode));
        logged_present_mode = present_mode;
    }
#ifdef __ANDROID__
    // Android is already ordered the same as Switch.
    image_view_format = VK_FORMAT_R8G8B8A8_UNORM;
#else
    image_view_format = VK_FORMAT_B8G8R8A8_UNORM;
#endif
}

void Swapchain::CreateSemaphores() {
    present_semaphores.resize(image_count);
    std::ranges::generate(present_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
    render_semaphores.resize(image_count);
    std::ranges::generate(render_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
}

void Swapchain::Destroy() {
    frame_index = 0;
    present_semaphores.clear();
    render_semaphores.clear();
    swapchain.reset();
}

bool Swapchain::NeedsPresentModeUpdate() const {
    const auto requested_mode = ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed);
    return present_mode != requested_mode;
}

} // namespace Vulkan
