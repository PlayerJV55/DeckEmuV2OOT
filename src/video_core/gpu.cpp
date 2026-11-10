// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <list>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/logging.h"
#include "common/nx2_title_defaults.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "core/perf_stats.h"
#include "video_core/cdma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/gpu_thread.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_async_readback.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/nx2_profile.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"

namespace Tegra {

namespace {
constexpr u64 GpuClockMultiplier(Settings::GpuClock clock) {
    switch (clock) {
    case Settings::GpuClock::Boost:
        return 256;
    case Settings::GpuClock::Overclock:
        return 512;
    default:
        return 1;
    }
}

/// How much faster Boost and Overclock make Switch 2 GPU work look.
constexpr u64 AmpereClockDivisor(Settings::GpuClock clock) {
    switch (clock) {
    case Settings::GpuClock::Boost:
        return 4;
    case Settings::GpuClock::Overclock:
        return 16;
    default:
        return 1;
    }
}

// Switch 2 report timestamps in nanoseconds (Lichii #84, 01ee235c17) raise Ocarina of Time's
// dynamic resolution, but Mario Kart World paces by them and ran at 52.6 ms / 60% speed with them
// against 27.1 ms / 99% with GPU ticks. So only listed titles get them, as in Satsuma (a507e8a0dd)
// (title defaults in nx2_title_defaults.h). EDEN_NX2_GPU_TIMESTAMP=ns gives every Switch 2 title
// nanoseconds, =ticks none; any other value keeps the title's default.
bool Nx2NanosecondTimestamps() {
    static Common::Nx2::SessionValue<bool> nanoseconds{[] {
        constexpr const char* Name = "EDEN_NX2_GPU_TIMESTAMP";
        std::string_view mode = Common::Nx2::Setting(Name, "ticks");
        if (!mode.starts_with('n') && !mode.starts_with('t')) {
            mode = Common::Nx2::Default(Name, "ticks");
        }
        return mode.starts_with('n');
    }};
    return nanoseconds.Get();
}

// A guest thread scanning GPU-written buffers traps on every page, each a wait for the GPU
// thread. A trap that starts within ReadAheadStep past where the last one's flush ended doubles a
// window, up to ReadAheadMax, that its flush also covers past the page; any other trap starts
// over. EDEN_NX2_READ_AHEAD=1 opts in; ordinary reads flush the page alone.
// EDEN_NX2_READ_AHEAD=2: every trap also flushes the GPU writes in the ReadAheadMax-aligned block
// of the cached buffer around it. Mario Kart World reads single pages scattered over a few
// 1-4 MiB GPU-written regions, out of order, so the window never grew; this makes one GPU wait
// per block instead of one per page.
constexpr u64 ReadAheadStep = 64ULL << 10;
constexpr u64 ReadAheadMax = 1ULL << 20;

u32 ReadAheadMode() {
    static const u32 mode = [] {
        const char* value = std::getenv("EDEN_NX2_READ_AHEAD");
        if (value == nullptr) {
            return 0U;
        }
        return value[0] == '1' ? 1U : value[0] == '2' ? 2U : 0U;
    }();
    return mode;
}

// Guest CPU writes to GPU-written memory wait in the dirty-memory managers until the GPU thread
// applies them. They are applied before every submitted list, copying only the written bytes
// into the buffer cache. EDEN_NX2_HELD_WRITES=0 applies them only at fences, marking whole pages
// CPU-modified (and re-uploading them over GPU writes never copied back), as before.
bool HeldWritesPerList() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_HELD_WRITES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// EDEN_NX2_HELD_WRITE_BATCH=1 applies the held writes of one InvalidateGPUCache as one batch, each
// cache locked once for all of them instead of once per region (each unlock can wake a guest CPU
// thread waiting for that cache), after joining touching and overlapping writes within one 4 KiB
// page. =2 batches without joining. Off by default until measured.
constexpr u32 HeldWriteJoinPageBits = 12;

int HeldWriteBatchMode() {
    static const int mode = [] {
        const char* value = std::getenv("EDEN_NX2_HELD_WRITE_BATCH");
        if (value == nullptr) {
            return 0;
        }
        return value[0] == '1' ? 1 : value[0] == '2' ? 2 : 0;
    }();
    return mode;
}

// EDEN_NX2_READ_FLUSH_HELD_WRITES=1 applies the held writes before a guest CPU read's flush, so
// the download can't put the GPU's older copy over them. Off by default until compared on the
// titles it can change (Mario Kart World, Donkey Kong Bananza, Pokopia).
bool HeldWritesBeforeReadFlush() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_READ_FLUSH_HELD_WRITES");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct ReadAheadState {
    DAddr last_end{};
    u64 window{};
};
thread_local ReadAheadState t_read_ahead;
} // Anonymous namespace

struct GPU::Impl {
    explicit Impl(Core::System& system_, bool is_async_, bool use_nvdec_)
        : system{system_}
        , gpu_thread{system_}
        , use_nvdec{use_nvdec_}
        , shader_notify()
        , is_async{is_async_}
    {}

    ~Impl() {
        NotifyShutdown();
        // Rasterizer teardown flushes commands and touches the bound engine's dirty flags: keep
        // the channels, their scheduler and the graphics context alive until it is done.
        renderer.reset();
    }

    std::shared_ptr<Control::ChannelState> CreateChannel(s32 channel_id) {
        auto channel_state = std::make_shared<Tegra::Control::ChannelState>(channel_id);
        channels.emplace(channel_id, channel_state);
        scheduler.DeclareChannel(channel_state);
        return channel_state;
    }

    void BindChannel(s32 channel_id) {
        if (bound_channel != channel_id) {
            auto it = channels.find(channel_id);
            ASSERT(it != channels.end());
            bound_channel = channel_id;
            current_channel = it->second.get();
            renderer->ReadRasterizer()->BindChannel(*current_channel);
        }
    }

    std::shared_ptr<Control::ChannelState> AllocateChannel() {
        return CreateChannel(new_channel_id++);
    }

    void InitChannel(Control::ChannelState& to_init, u64 program_id) {
        to_init.Init(system, program_id);
        to_init.BindRasterizer(renderer->ReadRasterizer());
        renderer->ReadRasterizer()->InitializeChannel(to_init);
    }

    void InitAddressSpace(Tegra::MemoryManager& memory_manager) {
        memory_manager.BindRasterizer(renderer->ReadRasterizer());
    }

    void ReleaseChannel(Control::ChannelState& to_release) {
        UNIMPLEMENTED();
    }

    /// Binds a renderer to the GPU.
    void BindRenderer(std::unique_ptr<VideoCore::RendererBase> renderer_) {
        renderer = std::move(renderer_);
        system.Host1x().memory_manager.BindInterface(renderer->ReadRasterizer());
        system.Host1x().gmmu_manager.BindRasterizer(renderer->ReadRasterizer());
    }

    /// Flush all current written commands into the host GPU for execution.
    void FlushCommands() {
        renderer->ReadRasterizer()->FlushCommands();
    }

    /// Synchronizes CPU writes with Host GPU memory.
    void InvalidateGPUCache() {
        if (HeldWriteBatchMode() != 0 && HeldWritesPerList()) {
            ApplyHeldWritesBatched();
            return;
        }
        std::function<void(PAddr, size_t)> callback_writes([this](PAddr address, size_t size) {
            if (HeldWritesPerList()) {
                renderer->ReadRasterizer()->OnCachedWrite(address, size);
            } else {
                renderer->ReadRasterizer()->OnCacheInvalidation(address, size);
            }
        });
        system.GatherGPUDirtyMemory(callback_writes);
    }

    /// InvalidateGPUCache with EDEN_NX2_HELD_WRITE_BATCH.
    void ApplyHeldWritesBatched() {
        // Per thread: fences apply held writes too.
        static thread_local std::vector<std::pair<DAddr, std::size_t>> writes;
        writes.clear();
        // EDEN_NX2_ASYNC_READBACK: gathered writes are no longer pending (ForEachPendingCpuWrite)
        // once Gather returns, before OnCachedWrites applies them. A read download landing
        // meanwhile on a reading thread would write the GPU's older bytes over them, so they are
        // listed until then. The lock is held only around `writes`: a landing asks for the
        // pending writes with the buffer cache's lock held, which Gather's other callers take.
        const bool async_read = VideoCore::AsyncReadback::Enabled();
        if (async_read) {
            std::scoped_lock lock{applying_writes_mutex};
            applying_writes.push_back(&writes);
        }
        // Also if OnCachedWrites throws: ForEachPendingCpuWrite must not reach `writes` then.
        SCOPE_EXIT {
            if (async_read) {
                std::scoped_lock lock{applying_writes_mutex};
                std::erase(applying_writes, &writes);
            }
        };
        std::function<void(PAddr, size_t)> callback_writes(
            [this, async_read](PAddr address, size_t size) {
                if (address != 0 && size != 0) {
                    std::unique_lock lock{applying_writes_mutex, std::defer_lock};
                    if (async_read) {
                        lock.lock();
                    }
                    writes.emplace_back(static_cast<DAddr>(address), size);
                }
            });
        system.GatherGPUDirtyMemory(callback_writes);
        if (writes.empty()) {
            return;
        }
        // An exact union (the same bytes, copied from guest memory) that never joins across a
        // page, so it does not make a range span two buffer cache buffers.
        if (HeldWriteBatchMode() == 1 && writes.size() > 1) {
            std::unique_lock lock{applying_writes_mutex, std::defer_lock};
            if (async_read) {
                lock.lock();
            }
            std::ranges::sort(writes);
            size_t out = 0;
            for (size_t in = 1; in < writes.size(); ++in) {
                auto& last = writes[out];
                const auto [address, size] = writes[in];
                const DAddr last_end = last.first + last.second;
                if ((address >> HeldWriteJoinPageBits) == (last.first >> HeldWriteJoinPageBits) &&
                    address <= last_end) {
                    last.second = static_cast<std::size_t>((std::max)(last_end, address + size) -
                                                           last.first);
                } else {
                    writes[++out] = writes[in];
                }
            }
            writes.resize(out + 1);
        }
        renderer->ReadRasterizer()->OnCachedWrites(writes);
    }

    /// GPU::ForEachPendingCpuWrite.
    void ForEachPendingCpuWrite(const std::function<void(DAddr, size_t)>& callback) {
        if (!VideoCore::AsyncReadback::Enabled()) {
            for (auto& manager : system.GetGPUDirtyMemoryManager()) {
                manager.ForEachPending(callback);
            }
            return;
        }
        std::scoped_lock lock{applying_writes_mutex};
        for (auto& manager : system.GetGPUDirtyMemoryManager()) {
            manager.ForEachPending(callback);
        }
        for (const auto* writes : applying_writes) {
            for (const auto& [address, size] : *writes) {
                callback(address, size);
            }
        }
    }

    /// Signal the ending of command list.
    void OnCommandListEnd() {
        renderer->ReadRasterizer()->ReleaseFences(false);
        Settings::UpdateGPUAccuracy();
    }

    /// Request a host GPU memory flush from the CPU.
    template <typename Func>
    [[nodiscard]] u64 RequestSyncOperation(Func&& action) {
        std::unique_lock lck{sync_request_mutex};
        const u64 fence = ++last_sync_fence;
        sync_requests.emplace_back(std::forward<Func>(action));
        requested_sync_fence.store(fence, std::memory_order_release);
        return fence;
    }

    /// Obtains current flush request fence id.
    [[nodiscard]] u64 CurrentSyncRequestFence() const {
        return current_sync_fence.load(std::memory_order_relaxed);
    }

    void WaitForSyncOperation(const u64 fence) {
        std::unique_lock lck{sync_request_mutex};
        // No thread runs the operation after the GPU threads stop, and a core waiting here would
        // keep the emulation from stopping.
        sync_request_cv.wait(lck, [this, fence] {
            return CurrentSyncRequestFence() >= fence ||
                   shutting_down.load(std::memory_order_relaxed);
        });
    }

    void WaitForIdle() {
        const u64 fence = RequestSyncOperation([] {});
        gpu_thread.TickGPU(is_async);
        WaitForSyncOperation(fence);
    }

    /// Tick pending requests within the GPU.
    void TickWork() {
        // Runs after every draw: skip the mutex while no request is waiting (requests are rare).
        if (requested_sync_fence.load(std::memory_order_acquire) ==
            current_sync_fence.load(std::memory_order_acquire)) {
            return;
        }
        std::unique_lock lck{sync_request_mutex};
        while (!sync_requests.empty()) {
            auto request = std::move(sync_requests.front());
            sync_requests.pop_front();
            sync_request_mutex.unlock();
            VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::TickWork);
            {
                VideoCommon::Nx2Profile::Scope request_scope{
                    VideoCommon::Nx2Profile::Zone::SyncRequests};
                request();
            }
            current_sync_fence.fetch_add(1, std::memory_order_release);
            sync_request_mutex.lock();
            sync_request_cv.notify_all();
        }
    }

    void ForwardSyncRequests() {
        const u64 requested = requested_sync_fence.load(std::memory_order_acquire);
        if (requested == forwarded_sync_fence ||
            requested == current_sync_fence.load(std::memory_order_acquire)) {
            return;
        }
        forwarded_sync_fence = requested;
        auto& split = VideoCommon::GpuSplit::Get();
        split.Defer([this] { TickWork(); });
        split.Publish();
    }

    [[nodiscard]] u64 GetTicks() const {
        const Settings::GpuClock clock = Settings::values.gpu_clock.GetValue();
        if (!g_nx2_ampere.load(std::memory_order_relaxed) || !Nx2NanosecondTimestamps()) {
            return system.CoreTiming().GetGPUTicks() / GpuClockMultiplier(clock);
        }
        // Titles calibrate nanosecond timestamps against CPU time, so Boost and Overclock shrink
        // only the time since the last present: GPU durations within a frame look shorter, while
        // a timestamp never runs ahead of the CPU clock. It catches up at each present. The clock
        // is the one latched with that present: a GPU Clocks change mid-frame would otherwise
        // move timestamps backward.
        const u64 now = static_cast<u64>(system.CoreTiming().GetGlobalTimeNs().count());
        const u64 anchor = present_anchor.load(std::memory_order_relaxed);
        const u64 last_present = anchor >> 2;
        u64 time = now;
        if (last_present != 0 && now > last_present) {
            const auto latched_clock = static_cast<Settings::GpuClock>(anchor & 3);
            time = last_present + (now - last_present) / AmpereClockDivisor(latched_clock);
        }
        return Nx2ReportSubmitTime() ? report_clock.Next(time) : time;
    }

    void RendererFrameEndNotify() {
        present_anchor.store(
            static_cast<u64>(system.CoreTiming().GetGlobalTimeNs().count()) << 2 |
                static_cast<u64>(Settings::values.gpu_clock.GetValue()),
            std::memory_order_relaxed);
        system.GetPerfStats().EndGameFrame();
    }

    void RendererFramesPresented(u32 count) {
        system.GetPerfStats().AddPresentedFrames(count);
    }

    /// Performs any additional setup necessary in order to begin GPU emulation.
    /// This can be used to launch any necessary threads and register any necessary
    /// core timing events.
    void Start() {
        Settings::UpdateGPUAccuracy();
        // With the GPU thread split, its back runs a sync operation a guest thread waits for
        // after its current entry (OnCPURead).
        VideoCommon::GpuSplit::Get().SetTickHook(
            [](void* argument) { static_cast<Impl*>(argument)->TickWork(); }, this);
        gpu_thread.StartThread(*renderer, renderer->Context(), scheduler);
    }

    void NotifyShutdown() {
        gpu_thread.NotifyShutdown();
        VideoCommon::GpuSplit::Get().SetTickHook(nullptr, nullptr);
        std::unique_lock lk{sync_mutex};
        shutting_down.store(true, std::memory_order::relaxed);
        sync_cv.notify_all();
        lk.unlock();
        // Taking the lock first makes a thread between its check and its wait get this wake.
        {
            std::scoped_lock request_lock{sync_request_mutex};
            // Nothing runs them anymore, and they may refer to the frames of the waiters this
            // releases (GetAppletCaptureBuffer's request captures by reference).
            sync_requests.clear();
        }
        sync_request_cv.notify_all();
    }

    /// Obtain the CPU Context
    void ObtainContext() {
        if (!cpu_context) {
            cpu_context = renderer->GetRenderWindow().CreateSharedContext();
        }
        cpu_context->MakeCurrent();
    }

    /// Release the CPU Context
    void ReleaseContext() {
        cpu_context->DoneCurrent();
    }

    /// Push GPU command entries to be processed
    void PushGPUEntries(s32 channel, Tegra::CommandList&& entries) {
        gpu_thread.SubmitList(channel, std::move(entries), is_async);
    }

    void RunInOrder(std::function<void()>&& func, bool before_later_reads = false) {
        gpu_thread.Run(std::move(func), is_async, before_later_reads);
    }

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    void FlushRegion(DAddr addr, u64 size) {
        gpu_thread.FlushRegion(addr, size, is_async);
    }

    VideoCore::RasterizerDownloadArea OnCPURead(DAddr addr, u64 size) {
        const auto read_begin = std::chrono::steady_clock::now();
        auto raster_area = renderer->ReadRasterizer()->GetFlushArea(addr, size);
        if (raster_area.preemtive) {
            return raster_area;
        }
        raster_area.preemtive = true;
        // A guest CPU read of GPU-written memory (reactive flushing): count them, they decide
        // whether buffer data must be copied back at every fence (EDEN_NX2_BUFFER_DOWNLOADS).
        if (static std::atomic<u64> reads{}; (reads.fetch_add(1, std::memory_order_relaxed) & 63) == 0) {
            LOG_INFO(HW_GPU, "CPU read of GPU-written memory [{:#x}, {:#x}) ({} so far)",
                     raster_area.start_address, raster_area.end_address,
                     reads.load(std::memory_order_relaxed));
        }
        // EDEN_NX2_ASYNC_READBACK: an earlier read's downloads cover the area, only the wait for
        // them is left.
        if (raster_area.in_flight) {
            FinishAsyncRead(raster_area, read_begin);
            CountCpuRead(std::chrono::steady_clock::now() - read_begin,
                         raster_area.end_address - raster_area.start_address);
            return raster_area;
        }
        const bool async_read = VideoCore::AsyncReadback::Enabled();
        u64 ahead_window = 0;
        const bool read_region = ReadAheadMode() == 2;
        if (ReadAheadMode() == 1) {
            auto& ahead = t_read_ahead;
            const bool sequential = ahead.last_end != 0 &&
                                    raster_area.start_address >= ahead.last_end &&
                                    raster_area.start_address - ahead.last_end <= ReadAheadStep;
            ahead.window = sequential ? (std::min)(ahead.window == 0 ? ReadAheadStep
                                                                     : ahead.window * 2,
                                                   ReadAheadMax)
                                      : 0;
            ahead_window = ahead.window;
            // The pages ahead get a preemptive answer and do not come here, whatever gaps the
            // window has: the sequence goes on past it.
            ahead.last_end = raster_area.end_address + ahead_window;
        }
        // A copy: a wait that ends at shutdown leaves this operation in the queue. The pages ahead
        // are flushed and marked within it, so no other thread sees them as flushed before.
        const u64 fence = RequestSyncOperation([this, raster_area, ahead_window, read_region,
                                                async_read]() {
            if (HeldWritesBeforeReadFlush()) {
                InvalidateGPUCache();
            }
            const auto flush_begin = std::chrono::steady_clock::now();
            auto* const rasterizer = renderer->ReadRasterizer();
            if (async_read) {
                // Submits the buffer downloads without waiting for the host GPU: FinishRead below.
                rasterizer->FlushRegionForRead(raster_area.start_address,
                                               raster_area.end_address - raster_area.start_address);
            } else {
                rasterizer->FlushRegion(raster_area.start_address,
                                        raster_area.end_address - raster_area.start_address);
            }
            if (ahead_window != 0) {
                rasterizer->FlushAhead(raster_area.end_address, ahead_window);
            }
            if (read_region) {
                rasterizer->FlushReadRegion(raster_area.start_address, ReadAheadMax);
            }
            const u64 flush_ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - flush_begin)
                    .count());
            cpu_read_flush_ns.fetch_add(flush_ns, std::memory_order_relaxed);
            if (async_read) {
                VideoCore::AsyncReadback::Add(VideoCore::AsyncReadback::g_counters.flush_ns,
                                              flush_ns);
            }
        });
        // The split's back runs it after the entry it is on. The tick sent through the GPU
        // thread's queue still runs it without the split, or while the back waits for work.
        VideoCommon::GpuSplit::Get().RequestTick();
        gpu_thread.TickGPU(is_async);
        WaitForSyncOperation(fence);
        if (async_read) {
            FinishAsyncRead(raster_area, read_begin);
        }
        CountCpuRead(std::chrono::steady_clock::now() - read_begin,
                     raster_area.end_address - raster_area.start_address);
        return raster_area;
    }

    /// EDEN_NX2_ASYNC_READBACK: the GPU thread submitted the read's downloads without waiting for
    /// the host GPU. This thread waits for them and writes them to guest memory while the GPU
    /// thread goes on.
    void FinishAsyncRead(const VideoCore::RasterizerDownloadArea& area,
                         std::chrono::steady_clock::time_point read_begin) {
        namespace AsyncReadback = VideoCore::AsyncReadback;
        AsyncReadback::t_reading = true;
        renderer->ReadRasterizer()->FinishRead(area.start_address,
                                               area.end_address - area.start_address);
        AsyncReadback::t_reading = false;
        AsyncReadback::CountRead(
            area.in_flight ? AsyncReadback::Read::InFlight : AsyncReadback::Read::Async,
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - read_begin)
                                 .count()));
        if (VideoCommon::Nx2Profile::Enabled()) {
            AsyncReadback::MaybeReport();
        }
    }

    /// The guest CPU's waits for those flushes, from the trap to the flush done, logged by the
    /// first one 10 s after the last line, over the time since, with how much of them the flush
    /// itself took on the GPU thread.
    void CountCpuRead(std::chrono::steady_clock::duration waited, u64 area_bytes) {
        std::scoped_lock lk{cpu_read_mutex};
        const auto now = std::chrono::steady_clock::now();
        if (cpu_read_window == std::chrono::steady_clock::time_point{}) {
            cpu_read_window = now;
        }
        ++cpu_read_waits;
        cpu_read_total += waited;
        cpu_read_longest = (std::max)(cpu_read_longest, waited);
        cpu_read_bytes += area_bytes;
        if (now - cpu_read_window < std::chrono::seconds{10}) {
            return;
        }
        const auto to_ms = [](std::chrono::steady_clock::duration d) {
            return std::chrono::duration<double, std::milli>(d).count();
        };
        const double flush_ms =
            static_cast<double>(cpu_read_flush_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
        LOG_INFO(HW_GPU,
                 "CPU reads of GPU-written memory in the last {:.1f} s: {} waits, {:.1f} ms total "
                 "({:.1f} ms in the flush itself), {:.1f} ms longest, {} KiB",
                 std::chrono::duration<double>(now - cpu_read_window).count(), cpu_read_waits,
                 to_ms(cpu_read_total), flush_ms, to_ms(cpu_read_longest), cpu_read_bytes >> 10);
        cpu_read_window = now;
        cpu_read_total = {};
        cpu_read_longest = {};
        cpu_read_waits = 0;
        cpu_read_bytes = 0;
    }

    /// Notify rasterizer that any caches of the specified region should be invalidated
    void InvalidateRegion(DAddr addr, u64 size, bool preserve_gpu_writes) {
        if (preserve_gpu_writes) {
            VideoCore::RasterizerInterface* rasterizer = renderer->ReadRasterizer();
            if (!rasterizer->TryCacheInvalidation(addr, size)) {
                const u64 fence = RequestSyncOperation([rasterizer, addr, size] {
                    rasterizer->FlushRegion(addr, size, VideoCommon::CacheType::BufferCache);
                    rasterizer->OnCacheInvalidation(addr, size);
                });
                gpu_thread.TickGPU(is_async);
                WaitForSyncOperation(fence);
            }
            return;
        }
        gpu_thread.InvalidateRegion(addr, size);
    }

    bool OnCPUWrite(DAddr addr, u64 size) {
        return renderer->ReadRasterizer()->OnCPUWrite(addr, size);
    }

    /// Notify rasterizer that any caches of the specified region should be flushed and invalidated
    void FlushAndInvalidateRegion(DAddr addr, u64 size) {
        gpu_thread.FlushAndInvalidateRegion(addr, size, is_async);
    }

    void RequestComposite(std::vector<Tegra::FramebufferConfig>&& layers, std::vector<Service::Nvidia::NvFence>&& fences) {
        const size_t num_fences{fences.size()};
        size_t current_request_counter{};
        if (num_fences != 0) {
            std::unique_lock<std::mutex> lk(request_swap_mutex);
            if (free_swap_counters.empty()) {
                current_request_counter = request_swap_counters.size();
                request_swap_counters.emplace_back(num_fences);
            } else {
                current_request_counter = free_swap_counters.front();
                request_swap_counters[current_request_counter] = num_fences;
                free_swap_counters.pop_front();
            }
        }
        pending_composite_fence = RequestSyncOperation(
            [this, current_request_counter, num_fences, composite_layers = std::move(layers),
             composite_fences = std::move(fences)] {
                if (num_fences == 0) {
                    std::scoped_lock lk{composite_mutex};
                    renderer->Composite(composite_layers);
                    return;
                }
                auto& syncpoint_manager = system.Host1x().GetSyncpointManager();
                const auto executer = [this, current_request_counter, composite_layers]() {
                    {
                        std::unique_lock<std::mutex> lk(request_swap_mutex);
                        if (--request_swap_counters[current_request_counter] != 0) {
                            return;
                        }
                        free_swap_counters.push_back(current_request_counter);
                    }
                    // Since syncpoint actions run outside the syncpoint guard, two can composite
                    // at once from different threads, and the renderer isn't thread safe.
                    std::scoped_lock lk{composite_mutex};
                    renderer->Composite(composite_layers);
                };
                for (size_t i = 0; i < num_fences; i++) {
                    syncpoint_manager.RegisterGuestAction(composite_fences[i].id,
                                                          composite_fences[i].value, executer);
                }
            });
        gpu_thread.TickGPU(is_async);
    }

    void WaitForComposite() {
        const u64 fence = pending_composite_fence;
        if (fence == 0) {
            return;
        }
        pending_composite_fence = 0;
        if (shutting_down.load(std::memory_order_relaxed)) {
            return;
        }
        WaitForSyncOperation(fence);
    }

    std::vector<u8> GetAppletCaptureBuffer() {
        std::vector<u8> out;

        const auto wait_fence =
            RequestSyncOperation([&] { out = renderer->GetAppletCaptureBuffer(); });
        gpu_thread.TickGPU(is_async);
        WaitForSyncOperation(wait_fence);

        return out;
    }

    Core::System& system;

    // Destruction of thread must be done before all (non trivial)
    // previous members has been destroyed
    VideoCommon::GPUThread::ThreadManager gpu_thread;

    // Declared before the renderer so they are destroyed after it: tearing the renderer down still
    // submits work, and that marks the bound channel's Maxwell3D state dirty.
    Tegra::Control::Scheduler scheduler;
    ::Common::unordered_map<s32, std::shared_ptr<Tegra::Control::ChannelState>> channels;
    Tegra::Control::ChannelState* current_channel;
    s32 bound_channel{-1};

    std::unique_ptr<VideoCore::RendererBase> renderer;
    const bool use_nvdec;

    s32 new_channel_id{1};
    /// Shader build notifier
    VideoCore::ShaderNotify shader_notify;
    /// When true, we are about to shut down emulation session, so terminate outstanding tasks
    std::atomic_bool shutting_down{};

    std::array<std::atomic<u32>, Service::Nvidia::MaxSyncPoints> syncpoints{};

    std::array<std::list<u32>, Service::Nvidia::MaxSyncPoints> syncpt_interrupts;

    std::mutex sync_mutex;
    std::mutex device_mutex;

    std::condition_variable sync_cv;

    std::list<std::function<void()>> sync_requests;
    std::atomic<u64> current_sync_fence{};
    /// For GetTicks: emulated time of the last present shifted left by 2, ORed with the GPU clock
    /// at that present.
    std::atomic<u64> present_anchor{};
    mutable Nx2ReportClock report_clock;

    u64 last_sync_fence{};
    std::atomic<u64> requested_sync_fence{}; ///< last_sync_fence, readable without the mutex
    u64 forwarded_sync_fence{}; ///< requested_sync_fence the split's front last queued a tick for
    std::mutex sync_request_mutex;
    std::condition_variable sync_request_cv;

    /// EDEN_NX2_ASYNC_READBACK: the writes ApplyHeldWritesBatched gathered and is applying, on
    /// each thread doing so.
    std::vector<const std::vector<std::pair<DAddr, std::size_t>>*> applying_writes;
    std::mutex applying_writes_mutex;

    const bool is_async;

    std::unique_ptr<Core::Frontend::GraphicsContext> cpu_context;

    std::deque<size_t> free_swap_counters;
    std::deque<size_t> request_swap_counters;
    std::mutex request_swap_mutex;
    std::mutex composite_mutex; ///< Composite runs on the GPU thread or in syncpoint actions
    u64 pending_composite_fence{};

    std::mutex cpu_read_mutex; ///< CountCpuRead's window
    std::chrono::steady_clock::time_point cpu_read_window{};
    std::chrono::steady_clock::duration cpu_read_total{};
    std::chrono::steady_clock::duration cpu_read_longest{};
    u64 cpu_read_waits{};
    u64 cpu_read_bytes{};
    std::atomic<u64> cpu_read_flush_ns{}; ///< Their FlushRegion on the GPU thread
};

GPU::GPU(Core::System& system, bool is_async, bool use_nvdec)
    : impl{std::make_unique<Impl>(system, is_async, use_nvdec)}
{}

GPU::~GPU() = default;

std::shared_ptr<Control::ChannelState> GPU::AllocateChannel() {
    return impl->AllocateChannel();
}

void GPU::InitChannel(Control::ChannelState& to_init, u64 program_id) {
    impl->InitChannel(to_init, program_id);
}

void GPU::BindChannel(s32 channel_id) {
    impl->BindChannel(channel_id);
}

void GPU::ReleaseChannel(Control::ChannelState& to_release) {
    impl->ReleaseChannel(to_release);
}

void GPU::InitAddressSpace(Tegra::MemoryManager& memory_manager) {
    impl->InitAddressSpace(memory_manager);
}

void GPU::BindRenderer(std::unique_ptr<VideoCore::RendererBase> renderer) {
    impl->BindRenderer(std::move(renderer));
}

void GPU::FlushCommands() {
    impl->FlushCommands();
}

void GPU::InvalidateGPUCache() {
    impl->InvalidateGPUCache();
}

// Guest CPU write traps that find the buffer or texture cache lock held (by the GPU thread, often
// for a whole draw or a long upload) are collected like writes to GPU-written memory and applied
// with them, instead of stopping the guest core until the lock is free: in Ocarina of Time's slow
// stretches they waited 0.3 s of every second. Needs the held writes applied per list, and on
// Switch 2 titles semaphore acquires apply them too (Puller), for lists already running.
// EDEN_NX2_CPU_WRITE_DEFER=0 waits for the lock as before.
bool GPU::DefersContendedCpuWrites() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_CPU_WRITE_DEFER");
        return (value == nullptr || value[0] != '0') && HeldWritesPerList();
    }();
    return enabled && Settings::CurrentProgramIsSwitch2();
}

bool GPU::HasPendingCPUWrites(DAddr addr, u64 size) {
    for (auto& manager : impl->system.GetGPUDirtyMemoryManager()) {
        if (manager.Overlaps(addr, size)) {
            return true;
        }
    }
    return false;
}

void GPU::ForEachPendingCpuWrite(const std::function<void(DAddr, size_t)>& callback) {
    impl->ForEachPendingCpuWrite(callback);
}

void GPU::ApplyHeldWritesForSubmit() {
    if (HeldWritesPerList()) {
        impl->InvalidateGPUCache();
    }
}

void GPU::TakeHeldWrites(std::function<void(PAddr, size_t)>& callback) {
    if (HeldWritesPerList()) {
        impl->system.GatherGPUDirtyMemory(callback);
    }
}

void GPU::OnCommandListEnd() {
    impl->OnCommandListEnd();
}

u64 GPU::RequestFlush(DAddr addr, std::size_t size) {
    return impl->RequestSyncOperation([this, addr, size]() {
        impl->renderer->ReadRasterizer()->FlushRegion(addr, size);
    });
}

u64 GPU::RunOnGpuThread(std::function<void()>&& action) {
    return impl->RequestSyncOperation(std::move(action));
}

u64 GPU::CurrentSyncRequestFence() const {
    return impl->CurrentSyncRequestFence();
}

void GPU::WaitForSyncOperation(u64 fence) {
    return impl->WaitForSyncOperation(fence);
}

void GPU::TickWork() {
    impl->TickWork();
}

bool GPU::ReadRequestsEarly() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_READ_REQUEST_EARLY");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void GPU::ForwardSyncRequests() {
    impl->ForwardSyncRequests();
}

/// Gets a mutable reference to the Host1x interface
Host1x::Host1x& GPU::Host1x() {
    return impl->system.Host1x();
}

/// Gets an immutable reference to the Host1x interface.
const Host1x::Host1x& GPU::Host1x() const {
    return impl->system.Host1x();
}

Engines::Maxwell3D& GPU::Maxwell3D() {
    return impl->current_channel->payload->maxwell_3d;
}

const Engines::Maxwell3D& GPU::Maxwell3D() const {
    return impl->current_channel->payload->maxwell_3d;
}

Engines::KeplerCompute& GPU::KeplerCompute() {
    return impl->current_channel->payload->kepler_compute;
}

const Engines::KeplerCompute& GPU::KeplerCompute() const {
    return impl->current_channel->payload->kepler_compute;
}

Tegra::DmaPusher& GPU::DmaPusher() {
    return impl->current_channel->payload->dma_pusher;
}

const Tegra::DmaPusher& GPU::DmaPusher() const {
    return impl->current_channel->payload->dma_pusher;
}

VideoCore::RendererBase& GPU::Renderer() {
    return *impl->renderer;
}

const VideoCore::RendererBase& GPU::Renderer() const {
    return *impl->renderer;
}

VideoCore::ShaderNotify& GPU::ShaderNotify() {
    return impl->shader_notify;
}

const VideoCore::ShaderNotify& GPU::ShaderNotify() const {
    return impl->shader_notify;
}

void GPU::RequestComposite(std::vector<Tegra::FramebufferConfig>&& layers,
                           std::vector<Service::Nvidia::NvFence>&& fences) {
    impl->RequestComposite(std::move(layers), std::move(fences));
}

void GPU::WaitForComposite() {
    impl->WaitForComposite();
}

std::vector<u8> GPU::GetAppletCaptureBuffer() {
    return impl->GetAppletCaptureBuffer();
}

u64 GPU::GetTicks() const {
    return impl->GetTicks();
}

bool GPU::IsAsync() const {
    return impl->is_async;
}

bool GPU::HasSuspendedChannels() const {
    return impl->scheduler.HasPendingChannelsAnyThread();
}

bool GPU::UseNvdec() const {
    return impl->use_nvdec;
}

void GPU::RendererFrameEndNotify() {
    impl->RendererFrameEndNotify();
}

void GPU::RendererFramesPresented(u32 count) {
    impl->RendererFramesPresented(count);
}

void GPU::Start() {
    impl->Start();
}

void GPU::NotifyShutdown() {
    impl->NotifyShutdown();
}

void GPU::WaitForIdle() {
    impl->WaitForIdle();
}

void GPU::ObtainContext() {
    impl->ObtainContext();
}

void GPU::ReleaseContext() {
    impl->ReleaseContext();
}

void GPU::PushGPUEntries(s32 channel, Tegra::CommandList&& entries) {
    impl->PushGPUEntries(channel, std::move(entries));
}

VideoCore::RasterizerDownloadArea GPU::OnCPURead(PAddr addr, u64 size) {
    return impl->OnCPURead(addr, size);
}

void GPU::FlushRegion(DAddr addr, u64 size) {
    impl->FlushRegion(addr, size);
}

namespace {
struct HeldRuns {
    u32 depth{};
    GPU* gpu{};
    std::vector<std::pair<std::function<void()>, bool>> funcs;
};
thread_local HeldRuns held_runs;

// With the GPU split, the front reads command lists, macro DMA and other GPU memory while the
// back runs what was queued before. A GMMU map deferred to the back left the front reading the
// lists submitted after it through addresses not mapped yet (zeros or the old mapping). Maps run
// on the front instead, after draining the back (EDEN_NX2_MAP_IN_ORDER=1). Off by default: queued
// maps already keep the front from reading their ranges (BeginQueuedUpdate), and draining the
// back at every map costs the titles whose back thread is their bottleneck (Mario Kart World).
bool MapsBeforeLaterReads() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_MAP_IN_ORDER");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}
} // namespace

GPU::RunInOrderBatch::RunInOrderBatch() {
    ++held_runs.depth;
}

GPU::RunInOrderBatch::~RunInOrderBatch() {
    if (--held_runs.depth != 0 || held_runs.funcs.empty()) {
        return;
    }
    auto funcs = std::move(held_runs.funcs);
    held_runs.funcs.clear();
    for (auto& [func, before_later_reads] : funcs) {
        held_runs.gpu->impl->RunInOrder(std::move(func), before_later_reads);
    }
}

void GPU::RunInOrder(std::function<void()>&& func) {
    if (held_runs.depth != 0) {
        held_runs.gpu = this;
        held_runs.funcs.emplace_back(std::move(func), false);
        return;
    }
    impl->RunInOrder(std::move(func));
}

void GPU::MapInOrder(std::function<void()>&& func) {
    const bool before_later_reads = MapsBeforeLaterReads();
    if (held_runs.depth != 0) {
        held_runs.gpu = this;
        held_runs.funcs.emplace_back(std::move(func), before_later_reads);
        return;
    }
    impl->RunInOrder(std::move(func), before_later_reads);
}

void GPU::InvalidateRegion(DAddr addr, u64 size, bool preserve_gpu_writes) {
    impl->InvalidateRegion(addr, size, preserve_gpu_writes);
}

bool GPU::OnCPUWrite(DAddr addr, u64 size) {
    return impl->OnCPUWrite(addr, size);
}

void GPU::FlushAndInvalidateRegion(DAddr addr, u64 size) {
    impl->FlushAndInvalidateRegion(addr, size);
}

} // namespace Tegra

namespace VideoCore {

bool MayHoldGpuWrites(DAddr addr, u64 size) {
    return !Tegra::g_nx2_ampere.load(std::memory_order_relaxed) ||
           Tegra::Nx2MayBeGpuWritten(addr, size);
}

} // namespace VideoCore
