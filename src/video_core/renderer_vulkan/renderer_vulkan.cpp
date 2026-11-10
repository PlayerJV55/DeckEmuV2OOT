// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging.h"
#include <ranges>
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/nx2.h"
#include "core/core_timing.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/game_dlss_status.h"
#include "video_core/gpu.h"
#include "video_core/present.h"
#include "video_core/renderer_vulkan/present/util.h"
#ifdef HAS_RESHADE
#include "video_core/post_processing/fx_chain.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_debug_callback.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#ifdef HAS_NVIDIA_STREAMLINE
#include "video_core/vulkan_common/streamline.h"
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif
namespace Vulkan {
namespace {

constexpr VkExtent2D CaptureImageSize{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
};

#ifdef HAS_LSFG
[[nodiscard]] VkExtent2D GuestExtent(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return VkExtent2D{};
    }

    const auto& framebuffer = framebuffers.front();
    if (framebuffer.crop_rect.IsEmpty()) {
        return VkExtent2D{.width = framebuffer.width, .height = framebuffer.height};
    }
    return VkExtent2D{
        .width = static_cast<u32>(framebuffer.crop_rect.GetWidth()),
        .height = static_cast<u32>(framebuffer.crop_rect.GetHeight()),
    };
}
#endif

constexpr VkExtent3D CaptureImageExtent{
    .width = VideoCore::Capture::LinearWidth,
    .height = VideoCore::Capture::LinearHeight,
    .depth = VideoCore::Capture::LinearDepth,
};

constexpr VkFormat CaptureFormat = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

// EDEN_NX2_APPLET_CAPTURE_EVERY_FRAME=1: draw the applet capture of Switch 2 frames at every
// composite, as before, instead of when it is read.
bool AppletCaptureEveryFrame() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_APPLET_CAPTURE_EVERY_FRAME");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

std::string GetDriverVersion(const Device& device) {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = device.GetDriverVersion();

    if (device.GetDriverID() == VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (device.GetDriverID() == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

std::string BuildCommaSeparatedExtensions(
    const std::set<std::string, std::less<>>& available_extensions) {
    return fmt::format("{}", fmt::join(available_extensions, ","));
}

} // Anonymous namespace

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld, VkSurfaceKHR surface) {
    const std::vector<VkPhysicalDevice> devices = instance.EnumeratePhysicalDevices();
    if (devices.empty()) {
        LOG_ERROR(Render_Vulkan, "No Vulkan devices found!");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    const auto is_cpu = [&](u32 index) {
        const auto type = vk::PhysicalDevice(devices[index], dld).GetProperties().deviceType;
        return type == VK_PHYSICAL_DEVICE_TYPE_CPU || type == VK_PHYSICAL_DEVICE_TYPE_OTHER;
    };
    // First real GPU, so a saved index never lands on a software renderer (llvmpipe) by
    // accident: the order of devices can change between runs and loaders.
    const auto first_gpu = [&]() -> std::optional<u32> {
        for (u32 i = 0; i < u32(devices.size()); ++i) {
            if (!is_cpu(i)) {
                return i;
            }
        }
        return std::nullopt;
    }();
    u32 device_index = Settings::values.vulkan_device.GetValue();
    if (device_index >= u32(devices.size())) {
        const u32 fallback = first_gpu.value_or(0);
        LOG_WARNING(Render_Vulkan, "Invalid device index {} ({} device(s) found), using device {}",
                    device_index, devices.size(), fallback);
        device_index = fallback;
    } else if (is_cpu(device_index) && first_gpu) {
        LOG_WARNING(Render_Vulkan,
                    "Device {} is a software renderer, using GPU device {} instead",
                    device_index, *first_gpu);
        device_index = *first_gpu;
    }
    const vk::PhysicalDevice physical_device(devices[device_index], dld);
    return Device(*instance, physical_device, surface, dld);
}

RendererVulkan::RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                               Tegra::MaxwellDeviceMemoryManager& device_memory_,
                               Tegra::GPU& gpu_,
                               std::unique_ptr<Core::Frontend::GraphicsContext> context_)
try
    : RendererBase(emu_window, std::move(context_))
    , device_memory(device_memory_)
    , gpu(gpu_)
    , library(OpenLibrary(context.get()))
    , dld()
#ifdef HAS_NVIDIA_STREAMLINE
    , streamline_started(Streamline::Start())
#endif
    // Create raw Vulkan instance first
    , instance(CreateInstance(*library,
                            dld,
                            VK_API_VERSION_1_1,
                            render_window.GetWindowInfo().type,
                            Settings::values.renderer_debug.GetValue()))
    // Create debug messenger if debug is enabled
    , debug_messenger(Settings::values.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                    : vk::DebugUtilsMessenger{})
    // Create surface
    , surface(CreateSurface(instance, render_window.GetWindowInfo()))
    , device(CreateDevice(instance, dld, *surface))
    , memory_allocator(device)
    , state_tracker()
    , scheduler(device, state_tracker)
    , swapchain(*surface,
                device,
                scheduler,
               render_window.GetFramebufferLayout().width,
               render_window.GetFramebufferLayout().height)
    , present_manager(instance,
                      render_window,
                      device,
                      memory_allocator,
                      scheduler,
                      swapchain,
                      surface)
    , blit_swapchain(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_capture(device_memory,
                   device,
                   memory_allocator,
                   present_manager,
                   scheduler,
                   PresentFiltersForDisplay)
    , blit_applet(device_memory,
                  device,
                  memory_allocator,
                  present_manager,
                  scheduler,
                  PresentFiltersForAppletCapture)
    , rasterizer(render_window, gpu, device_memory, device, memory_allocator, state_tracker, scheduler)
#ifdef HAS_LSFG
    , frame_gen(memory_allocator, scheduler)
#endif
{

    if (Settings::values.renderer_force_max_clock.GetValue() && device.ShouldBoostClocks()) {
        turbo_mode.emplace(instance, dld);
        scheduler.RegisterOnSubmit([this] { turbo_mode->QueueSubmitted(); });
    }

#ifdef HAS_RESHADE
    VideoCore::FxChain::Instance().LoadFromSettings();
#endif

#ifndef HAS_LSFG
    LOG_INFO(Render_Vulkan, "Frame generation is not available in this build (ENABLE_LSFG off)");
#endif

    Report();
} catch (const vk::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
    throw std::runtime_error{fmt::format("Vulkan initialization error {}", exception.what())};
}

RendererVulkan::~RendererVulkan() {
    scheduler.RegisterOnSubmit([] {});
    // The present thread may still be submitting and presenting.
    present_manager.StopPresentThread();
    void(device.GetLogical().WaitIdle());
}

void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    SCOPE_EXIT {
        render_window.OnFrameDisplayed();
    };

    RenderAppletCaptureLayer(framebuffers);

    if (!render_window.IsShown()) {
        return;
    }

    RenderScreenshot(framebuffers);
#if defined(HAS_LSFG) && !defined(__ANDROID__)
    if (!Settings::values.frame_gen.GetValue()) {
        present_manager.StopFrameGenPresentThread();
    }
#endif
    Frame* frame = present_manager.GetRenderFrame();

    scheduler.RequestOutsideRenderPassOperationContext();
    blit_swapchain.DrawToFrame(device, rasterizer, frame, framebuffers,
                               render_window.GetFramebufferLayout(),
                               present_manager.SwapchainImageCount(),
                               swapchain.GetImageViewFormat());

#ifdef HAS_LSFG
    void(frame_gen.WantedGenerations(present_manager.MaxExtraFrames()));

    frame_gen.Process(device, frame, present_manager.SwapchainImageFormat(),
                      GuestExtent(framebuffers));

    size_t generated_frames = frame_gen.GeneratedFrameCount();
    if (generated_frames > 0) {
#ifndef __ANDROID__
        // Spacing generated frames on the render thread would block emulation on every vblank.
        present_manager.StartPresentThread();
#endif
        // Generated frames only go out when they can't hold back the real frame rate.
        generated_frames = present_manager.GeneratedFramesAllowed(generated_frames);
        frame_gen.LimitGenerations(generated_frames);
    }
    // Generated frames (and then the real one) are spread over a real frame interval instead of
    // being presented back to back. The present thread waits for each present_after.
    const auto present_step = frame_gen.PresentStep(generated_frames);
    auto present_time = std::chrono::steady_clock::now();
    for (size_t generation = 0; generation < generated_frames; ++generation) {
        Frame* generated = present_manager.GetRenderFrame();
        generated->generated = true;
        generated->present_after = present_time;
        present_time += present_step;
        blit_swapchain.PrepareFrame(device, generated, render_window.GetFramebufferLayout());
        frame_gen.GenerateInto(device, generated, generation);
        generated->render_tick = scheduler.Flush(*generated->render_ready);
        present_manager.Present(generated);
        // Hands the frame to the present thread now instead of after the next one is generated.
        scheduler.DispatchWork();
    }
    frame->present_after = present_time;
#endif

    frame->render_tick = scheduler.Flush(*frame->render_ready);

    present_manager.Present(frame);
#ifdef HAS_LSFG
    scheduler.DispatchWork();
#endif

    gpu.RendererFrameEndNotify();
    // Generated frames the present thread dropped are not counted.
    gpu.RendererFramesPresented(present_manager.TakePresentedFrames());
    rasterizer.TickFrame();
    NoteMissingGameDlss();
}

// NVIDIA DLSS only replaces a game's own DLSS evaluation, so in other games it changes nothing:
// say so once, after enough frames for a game that uses DLSS to have run it.
void RendererVulkan::NoteMissingGameDlss() {
    static constexpr u32 FramesBeforeNote = 1800;
    if (frames_with_nvidia_dlss == FramesBeforeNote || !rasterizer.UsesNvidiaDlss() ||
        Settings::GetCurrentProgramID() == Tegra::Nx2OcarinaOfTimeProgramId) {
        return;
    }
    if (++frames_with_nvidia_dlss == FramesBeforeNote &&
        VideoCore::GetGameDlssStatus().state == VideoCore::GameDlssState::None) {
        LOG_INFO(Render_Vulkan,
                 "NVIDIA DLSS is on, but this game has not run a DLSS evaluation in its first {} "
                 "frames. It only replaces a game's own DLSS (Fire Emblem: Fortune's Weave is the "
                 "only one known) and does not raise the resolution of other games",
                 FramesBeforeNote);
    }
}

void RendererVulkan::Report() const {
    using namespace Common::Literals;
    const std::string vendor_name{device.GetVendorName()};
    const std::string model_name{device.GetModelName()};
    const std::string driver_version = GetDriverVersion(device);
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);

    const std::string api_version = GetReadableVersion(device.ApiVersion());

    const std::string extensions = BuildCommaSeparatedExtensions(device.GetAvailableExtensions());

    const auto available_vram = static_cast<f64>(device.GetDeviceLocalMemory()) / f64{1_GiB};

    LOG_INFO(Render_Vulkan, "Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "Device: {}", model_name);
    LOG_INFO(Render_Vulkan, "Vulkan: {}", api_version);
    LOG_INFO(Render_Vulkan, "Available VRAM: {:.2f} GiB", available_vram);
}

vk::Buffer RendererVulkan::RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                                          const Layout::FramebufferLayout& layout, VkFormat format,
                                          VkDeviceSize buffer_size) {
    auto frame = [&]() {
        Frame f{};
        f.image =
            CreateWrappedImage(memory_allocator, VkExtent2D{layout.width, layout.height}, format);
        f.image_view = CreateWrappedImageView(device, f.image, format);
        f.framebuffer = blit_capture.CreateFramebuffer(device, layout, *f.image_view, format);
        return f;
    }();

    auto dst_buffer = CreateWrappedBuffer(memory_allocator, buffer_size, MemoryUsage::Download);
    blit_capture.DrawToFrame(device, rasterizer, &frame, framebuffers, layout, 1, format);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *frame.image, *dst_buffer,
                           VkExtent3D{layout.width, layout.height, 1});
    });

    // Ensure the copy is fully completed before saving the capture
    scheduler.Finish();

    // Copy backing image data to the capture buffer
    dst_buffer.Invalidate();
    return dst_buffer;
}

void RendererVulkan::RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (!renderer_settings.screenshot_requested) {
        return;
    }

    const auto screenshot_layers = Tegra::FilterLayerStack(
        framebuffers, renderer_settings.screenshot_layer_stack, screenshot_layer_scratch);

    const auto& layout{renderer_settings.screenshot_framebuffer_layout};
    const auto dst_buffer = RenderToBuffer(screenshot_layers, layout, VK_FORMAT_B8G8R8A8_UNORM,
                                           layout.width * layout.height * 4);

    std::memcpy(renderer_settings.screenshot_bits, dst_buffer.Mapped().data(),
                dst_buffer.Mapped().size());
    renderer_settings.screenshot_complete_callback(false);
    renderer_settings.screenshot_requested = false;
}

std::vector<u8> RendererVulkan::GetAppletCaptureBuffer() {
    using namespace VideoCore::Capture;

    std::vector<u8> out(VideoCore::Capture::TiledSize);

    if (!pending_capture_layers.empty()) {
        DrawAppletCapture(pending_capture_layers);
        pending_capture_layers.clear();
    }
    if (!applet_frame.image) {
        return out;
    }

    const auto dst_buffer =
        CreateWrappedBuffer(memory_allocator, VideoCore::Capture::TiledSize, MemoryUsage::Download);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        DownloadColorImage(cmdbuf, *applet_frame.image, *dst_buffer, CaptureImageExtent);
    });

    // Ensure the copy is fully completed before writing the capture
    scheduler.Finish();

    // Swizzle image data to the capture buffer
    dst_buffer.Invalidate();
    Tegra::Texture::SwizzleTexture(out, dst_buffer.Mapped(), BytesPerPixel, LinearWidth,
                                   LinearHeight, LinearDepth, BlockHeight, BlockDepth);

    return out;
}

void RendererVulkan::RenderAppletCaptureLayer(
    std::span<const Tegra::FramebufferConfig> framebuffers) {
    const auto capture_layers = Tegra::FilterLayerStack(
        framebuffers, Service::Nvnflinger::LayerStackId::LastFrame, applet_capture_layers);

    if (capture_layers.empty())
        return;

    // Only applets read this capture (GetAppletCaptureBuffer), and drawing it for every frame
    // cost a framebuffer lookup under the texture cache lock, a render pass break and a
    // 1280x720 draw. Frames made only of Switch 2 layers are drawn into it when it is read.
    const bool switch2_layers = std::ranges::all_of(
        capture_layers, [](const Tegra::FramebufferConfig& layer) { return layer.native_tiling; });
    if (switch2_layers && !AppletCaptureEveryFrame()) {
        pending_capture_layers.assign(capture_layers.begin(), capture_layers.end());
        return;
    }
    pending_capture_layers.clear();
    DrawAppletCapture(capture_layers);
}

void RendererVulkan::DrawAppletCapture(std::span<const Tegra::FramebufferConfig> capture_layers) {
    if (!applet_frame.image) {
        applet_frame.image = CreateWrappedImage(memory_allocator, CaptureImageSize, CaptureFormat);
        applet_frame.image_view = CreateWrappedImageView(device, applet_frame.image, CaptureFormat);
        applet_frame.framebuffer = blit_applet.CreateFramebuffer(device,
            VideoCore::Capture::Layout, *applet_frame.image_view, CaptureFormat);
    }

    // With one set of layer resources each capture waited for the host GPU to finish the
    // previous one; rotating through three lets the GPU thread stay ahead like the swapchain blit
    // does. EDEN_NX2_CAPTURE_SETS=<1-8> picks the count (1 as before).
    static const size_t capture_resource_sets = [] {
        const char* value = std::getenv("EDEN_NX2_CAPTURE_SETS");
        const unsigned long sets = value != nullptr ? std::strtoul(value, nullptr, 10) : 0;
        return sets >= 1 && sets <= 8 ? static_cast<size_t>(sets) : size_t{3};
    }();
    scheduler.RequestOutsideRenderPassOperationContext();
    blit_applet.DrawToFrame(device, rasterizer, &applet_frame, capture_layers,
                            VideoCore::Capture::Layout, capture_resource_sets, CaptureFormat);
}

} // namespace Vulkan
