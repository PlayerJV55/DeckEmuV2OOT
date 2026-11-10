// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>
#include <boost/container/deque.hpp>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_host_pacer.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Frontend {
class EmuWindow;
} // namespace Core::Frontend

namespace Vulkan {

class Device;
class Scheduler;
class Swapchain;

struct Frame {
    u32 width;
    u32 height;
    u32 index;
    vk::Image image;
    vk::ImageView image_view;
    vk::ImageView storage_view;
    vk::Framebuffer framebuffer;
    vk::CommandBuffer cmdbuf;
    vk::Semaphore render_ready;
    /// Signaled by the swapchain image acquire; reused only after present_done.
    vk::Semaphore image_acquired;
    vk::Fence present_done;
    /// The present thread holds the frame until this time (spaces generated frames).
    std::chrono::steady_clock::time_point present_after{};
    /// Generated frames are not counted by the swapchain's frame rate target.
    bool generated{};
    bool storage_capable{};
    u64 render_tick{};                               ///< Scheduler tick that renders the frame
    std::chrono::steady_clock::time_point queued_at; ///< When it reached the present queue
};

class PresentManager {
public:
    PresentManager(const vk::Instance& instance,
                   Core::Frontend::EmuWindow& render_window,
                   const Device& device,
                   MemoryAllocator& memory_allocator,
                   Scheduler& scheduler,
                   Swapchain& swapchain,
                   vk::SurfaceKHR& surface);
    ~PresentManager();

    /// Returns the last used presentation frame
    Frame* GetRenderFrame();

    /// Pushes a frame for presentation
    void Present(Frame* frame);

    /// Recreates the present frame to match the provided parameters
    void RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                       VkRenderPass rd, bool storage);

    [[nodiscard]] bool NeedsStorage(const Frame* frame, bool required) const;

    /// Waits for the present thread to finish presenting all queued frames.
    void WaitPresent();

    /// How many additional frames can be queued without stalling the render thread
    [[nodiscard]] size_t MaxExtraFrames() const;

    /// Starts the present thread for frame generation if frames are still presented on the
    /// render thread.
    void StartPresentThread();

    /// Goes back to presenting on the render thread once frame generation is off, if only frame
    /// generation started the present thread.
    void StopFrameGenPresentThread();

    /// Stops the present thread; frames still queued are not presented. For teardown, before
    /// the device is waited on.
    void StopPresentThread();

    /// How many of the wanted generated frames can be queued now without holding back the next
    /// real frame: none while the present thread is behind, and one free frame is always left.
    [[nodiscard]] size_t GeneratedFramesAllowed(size_t wanted);

    /// Frames that reached the swapchain since the last call
    [[nodiscard]] u32 TakePresentedFrames() {
        return presented_frames.exchange(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t SwapchainImageCount() const { return swapchain_image_count; }
    [[nodiscard]] VkFormat SwapchainImageFormat() const { return swapchain_image_format; }

private:
    void PresentThread(std::stop_token token);

    /// Host frame pacer (vk_host_pacer.h): collects when earlier frames were shown and holds
    /// `frame` until its target. Returns whether it was held.
    bool PaceFrame(Frame* frame, std::stop_token token);

    /// Waits for each present id until it is shown (VK_KHR_present_wait).
    void PresentWaitThread(std::stop_token token);

    void ReportShown(u64 id, std::chrono::steady_clock::time_point shown, bool lost);

    void DrainShown();

    /// `token` is the present thread's; the render thread passes none.
    void CopyToSwapchain(Frame* frame, std::stop_token token = {});

    /// `submitted` is set once the copy is submitted, which consumes the frame's semaphores.
    void CopyToSwapchainImpl(Frame* frame, bool& submitted, std::stop_token token);

    /// Acquires a swapchain image with the frame's semaphore, recreating an out-of-date
    /// swapchain a few times and waiting up to a second while the window is shown and `token`
    /// is not stopped. False when the frame has to be discarded.
    [[nodiscard]] bool AcquireImage(Frame* frame, std::stop_token token);

    void RecreateSwapchain(Frame* frame);

    void DiscardFrame(Frame* frame);

    void SetImageCount();

    void UpdateImageCount();

    void WaitPresentTime(const Frame* frame) const;

private:
    const vk::Instance& instance;
    Core::Frontend::EmuWindow& render_window;
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    Swapchain& swapchain;
    vk::SurfaceKHR& surface;
    vk::CommandPool cmdpool;
    std::vector<Frame> frames;
    boost::container::deque<Frame*> present_queue;
    boost::container::deque<Frame*> free_queue;
    std::condition_variable_any frame_cv;
    std::condition_variable free_cv;
    std::mutex swapchain_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    /// frames[0, activated_frames) have been put in use at least once (guarded by free_mutex)
    std::size_t activated_frames{};
    /// Frames cycling through the free and present queues (guarded by free_mutex)
    std::size_t circulating_frames{};
    /// Idle frames taken out of use after image_count dropped; they keep their resources
    std::vector<Frame*> parked_frames;
    bool blit_supported;
    bool storage_supported;
    bool use_present_thread;
    /// The present thread runs only because frame generation started it
    bool present_thread_for_frame_gen{};
    /// present_after of the frame the present thread is presenting, max() while it has none
    std::atomic<std::chrono::steady_clock::rep> presenting_after{
        std::chrono::steady_clock::time_point::max().time_since_epoch().count()};
    std::atomic<std::size_t> image_count{};
    std::atomic<std::size_t> swapchain_image_count{};
    std::atomic<VkFormat> swapchain_image_format{};
    std::atomic<u32> presented_frames{};

    // Host frame pacer, used when the device supports VK_KHR_present_wait (present thread only).
    struct PendingWait {
        u64 id;
        VkSwapchainKHR swapchain;
        std::chrono::steady_clock::time_point submitted;
    };
    struct ShownReport {
        u64 id;
        std::chrono::steady_clock::time_point shown;
        bool lost;
    };
    bool present_wait_enabled{};
    HostPacer pacer;
    u64 last_present_id{};
    std::mutex present_wait_mutex; ///< Held while waiting on a swapchain; recreation takes it
    std::mutex wait_queue_mutex;
    std::condition_variable_any wait_queue_cv;
    std::deque<PendingWait> wait_queue;
    std::mutex shown_mutex;
    std::condition_variable shown_cv;
    std::vector<ShownReport> shown_reports;
    std::jthread present_wait_thread;
};

} // namespace Vulkan
