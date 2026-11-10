// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/nx2_profile.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/texture_barrier_epoch.h"

namespace Tegra::Engines {

namespace {

constexpr u32 Gf100BindClassMask = 0xffff;
constexpr u32 Gf100BindValidMask =  0x1f0000 | Gf100BindClassMask;

using DrainSite = VideoCommon::GpuSplit::DrainSite;

// With the GPU thread split (gpu_split.h) the rasterizer belongs to the back thread: work that
// only feeds it runs there in order; reads of what it wrote first wait for it (DrainBack).
template <typename Func>
void OnBack(const Func& func) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(func);
    } else {
        func();
    }
}

template <typename Func>
void OnBackAndWait(DrainSite site, const Func& func) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(func);
        split.Drain(site);
    } else {
        func();
    }
}

// The CPU may rewrite what the next draws read once it sees the release an acquire waited for:
// fast draws resolve in full from there, in order on the back.
void BreakFastDrawsAfterAcquire() {
    if (VideoCommon::Nx2FastDraw::Enabled()) {
        OnBack([] { VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Fence); });
    }
}

// A list already running can acquire a semaphore the guest CPU released after writing what the
// list reads next. Contended CPU write traps leave those writes held (GPU::DefersContendedCpuWrites):
// apply them before anything after the acquire.
void ApplyHeldWritesAfterAcquire(DmaPusher& dma_pusher) {
    if (Tegra::GPU::DefersContendedCpuWrites()) {
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); });
    }
    BreakFastDrawsAfterAcquire();
}

// Reads a semaphore word after flushing any GPU-written copy on the cache owner.
u32 ReadSemaphoreWord(DmaPusher& dma_pusher, GPUVAddr address) {
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address)) {
        OnBackAndWait(DrainSite::SemaphoreRead, [rasterizer = dma_pusher.rasterizer,
                                                 device = *device] {
            if (rasterizer->MustFlushRegion(device, sizeof(u32), VideoCommon::CacheType::BufferCache)) {
                rasterizer->FlushRegion(device, sizeof(u32), VideoCommon::CacheType::BufferCache);
            }
        });
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}

// EDEN_NX2_RELEASE_DRAIN_SKIP: fence releases skipped and drained, logged every 5 s. Front only.
struct FenceReleaseSkips {
    using FenceQuiet = VideoCommon::GpuSplit::FenceQuiet;

    std::array<u64, 4> taken{}; ///< By what the back's check found; Quiet: fence work added since
    u64 skipped{};
    std::chrono::steady_clock::time_point since{};

    void Count(FenceQuiet state, bool skip) {
        if (skip) {
            ++skipped;
        } else {
            ++taken[static_cast<size_t>(state)];
        }
        const auto now = std::chrono::steady_clock::now();
        if (since == std::chrono::steady_clock::time_point{}) {
            since = now;
            return;
        }
        const double window = std::chrono::duration<double>(now - since).count();
        if (window < 5.0) {
            return;
        }
        const auto rate = [window](u64 count) { return static_cast<double>(count) / window; };
        LOG_INFO(HW_GPU,
                 "Fence release skip (EDEN_NX2_RELEASE_DRAIN_SKIP): {:.0f} drains skipped/s, "
                 "{:.0f} taken/s: {:.0f} back behind, {:.0f} fence work pending, {:.0f} fence "
                 "work added since, {:.0f} unchecked",
                 rate(skipped), rate(taken[0] + taken[1] + taken[2] + taken[3]),
                 rate(taken[static_cast<size_t>(FenceQuiet::Behind)]),
                 rate(taken[static_cast<size_t>(FenceQuiet::Busy)]),
                 rate(taken[static_cast<size_t>(FenceQuiet::Quiet)]),
                 rate(taken[static_cast<size_t>(FenceQuiet::Unchecked)]));
        *this = {};
        since = now;
    }
};

// Releases the fences on the back for an acquire, and waits for it. EDEN_NX2_RELEASE_DRAIN_SKIP:
// not when the back, having run everything sent and its idle hook, found that it would only
// invalidate the GPU cache (FenceManager::FencesQuiet) and no fence work was added since; the
// invalidation is still queued, in order. Returns whether the fences were released.
bool ReleaseFences(DmaPusher& dma_pusher) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active() && split.ReleaseDrainSkip()) {
        static FenceReleaseSkips skips;
        u64 epoch{};
        const auto state = split.FencesQuietNow(epoch);
        const bool skip = state == VideoCommon::GpuSplit::FenceQuiet::Quiet &&
                          dma_pusher.rasterizer->FenceWorkEpoch() == epoch;
        skips.Count(state, skip);
        if (skip) {
            OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); });
            return false;
        }
    }
    OnBackAndWait(DrainSite::ReleaseFences,
                  [rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
    return true;
}

void DrainBack(DrainSite site) {
    VideoCommon::GpuSplit::Get().Drain(site);
}

// Whether an acquire-gequal is already met by its word in guest memory, before the split's back
// runs what was queued; never while a queued GMMU update may still move the word.
bool MetInMemory(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire) {
    return acquire.operation == Nx2SemaphoreWaiters::AcquireGequal &&
           VideoCommon::GpuSplit::Get().Active() &&
           !dma_pusher.memory_manager.IsUpdateQueued(acquire.address, sizeof(u32)) &&
           acquire.Matches(dma_pusher.memory_manager.Read<u32>(acquire.address));
}

// An acquire MetInMemory goes on without draining the back (Selevet Eglantine's acquire peek,
// for gequal only): the semaphores count up, so the releases still queued can only raise the
// word, and the work after the acquire still runs on the back after them. A queued release can
// move the word off an equal or mask acquire's value, so those drain as before.
// Off unless EDEN_NX2_ACQUIRE_PEEK=1: title 1 ran no faster with it on a GPU-bound machine.
bool PeekAcquires() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ACQUIRE_PEEK");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// EDEN_NX2_ACQUIRE_PASSED (on unless =0): an acquire-equal waits for a word that counts up. Once
// the word counted from below its value to past it while the channel waited, the release it
// waited for was made (or skipped by the game), and on the console the polling channel would have
// gone on; here the latch and the polls can miss a value written between two reads, and the
// channel waited forever (Ocarina of Time: channels 2, 3 and 5 at 0x626 with the word at 0x68a,
// 0 fps until restarted). A word already past the value when the channel suspended counts after
// 2 s, for values of 2 and up (not 0/1 flags).
bool EqualAcquirePassed(const Puller::PendingAcquire& acquire, u32 current) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ACQUIRE_PASSED");
        return value == nullptr || value[0] != '0';
    }();
    constexpr u32 window = 0x10000;
    if (!enabled || acquire.operation != Nx2SemaphoreWaiters::AcquireEqual) {
        return false;
    }
    const u32 to_value = acquire.value - acquire.first_word;
    const u32 to_current = current - acquire.first_word;
    if (to_value != 0 && to_value < window) {
        return to_current > to_value && to_current < window;
    }
    const u32 past = current - acquire.value;
    return acquire.value >= 2 && past != 0 && past < window &&
           acquire.first_word - acquire.value < window &&
           std::chrono::steady_clock::now() - acquire.since >= std::chrono::seconds{2};
}

// EDEN_NX2_ACQUIRE_QUEUED and EDEN_NX2_ACQUIRE_WRITER_WAIT: a release the front just sent, for
// acquires on any channel that read the same device word (GpuSplit::NoteQueuedRelease).
void NoteRelease(DmaPusher& dma_pusher, GPUVAddr address, u32 payload) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.NotesQueuedReleases()) {
        return;
    }
    // The back may write it through a mapping the front doesn't have yet.
    if (dma_pusher.memory_manager.IsUpdateQueued(address, sizeof(u32))) {
        split.ForgetQueuedReleases();
        return;
    }
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address)) {
        split.NoteQueuedRelease(*device, payload);
    }
}

// Whether the last release to the acquire's word that is still queued for the back meets it.
// Writes the front doesn't note (copy engine releases, compute releases without front macros,
// CPU writes) can come between that release and the acquire. Mode 1 trusts it for
// acquire-gequals only, on counters that only go up as for the peek, where such a write can only
// raise the word; mode 2 for every operation.
template <typename Meets>
bool MetByQueuedRelease(DmaPusher& dma_pusher, GPUVAddr address, bool gequal, const Meets& meets) {
    auto& split = VideoCommon::GpuSplit::Get();
    const u32 mode = split.AcquireQueued();
    if (mode == 0 || (mode == 1 && !gequal) ||
        dma_pusher.memory_manager.IsUpdateQueued(address, sizeof(u32))) {
        return false;
    }
    const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
    if (!device) {
        return false;
    }
    const auto payload = split.QueuedRelease(*device);
    return payload && meets(*payload);
}

bool MetByQueuedRelease(DmaPusher& dma_pusher, const Puller::PendingAcquire& acquire) {
    return MetByQueuedRelease(dma_pusher, acquire.address,
                              acquire.operation == Nx2SemaphoreWaiters::AcquireGequal,
                              [&acquire](u32 word) { return acquire.Matches(word); });
}

// Whether fence operations wait for the host GPU: high GPU accuracy under the default fences, or
// Balanced or Accurate fences. Payload releases that are not fences (16-byte ones) then wait in
// the fence manager for the next fence (QueryCacheBase::CounterReport).
bool DelayedFences() {
    return Settings::IsGPUFenceBehaviorDefault()
               ? Settings::IsGPULevelHigh()
               : Settings::IsGPUFenceBehaviorBalanced() || Settings::IsGPUFenceBehaviorAccurate();
}

// EDEN_NX2_ACQUIRE_WRITER_WAIT for `acquire`: 0 when off or not for it. Acquire-gequals only: the
// semaphores count up, so a word read before a queued write to it that meets the value meets it
// after that write too, and a word read unmet falls back to the drain. Not with delayed fences,
// whose releases land once the host GPU passed them, not when the back runs them.
u32 WriterWait(const Puller::PendingAcquire& acquire) {
    const auto& split = VideoCommon::GpuSplit::Get();
    if (split.AcquireWriterWait() == 0 || !split.Active() ||
        acquire.operation != Nx2SemaphoreWaiters::AcquireGequal || DelayedFences()) {
        return 0;
    }
    return split.AcquireWriterWait();
}

// Switch 2 semaphore acquires not met at once, logged every 5 s: what draining the split's back
// for them costs the front. Front thread only.
struct AcquireDrainStats {
    using Clock = std::chrono::steady_clock;

    u64 unmet{};           ///< Acquires not met at once (nor by a queued release)
    u64 not_applied{};     ///< Of those, with EDEN_NX2_ACQUIRE_WRITER_WAIT on but not for them
    u64 writer_waits{};    ///< EDEN_NX2_ACQUIRE_WRITER_WAIT: waits for the writers of the word
    u64 writer_ns{};
    u64 met_at_writers{};
    u64 suspended_undrained{}; ///< EDEN_NX2_ACQUIRE_WRITER_WAIT=2: suspended without a drain
    u64 drained{};         ///< Drained checks, with the fence release when still unmet
    u64 drained_ns{};
    u64 met_after_drain{}; ///< Met after the drain or the fence release
    u64 polls{};           ///< Checks of suspended acquires
    u64 poll_drains{};
    u64 poll_drain_ns{};
    u64 resumed{};
    u64 resume_ns{};       ///< From the acquire to its resume
    Clock::time_point since{};

    static AcquireDrainStats& Get() {
        static AcquireDrainStats stats;
        return stats;
    }

    static u64 Nanoseconds(Clock::time_point begin) {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    }

    void Drained(Clock::time_point begin, bool met) {
        ++drained;
        drained_ns += Nanoseconds(begin);
        met_after_drain += met ? 1 : 0;
        Tick();
    }

    void PollDrained(Clock::time_point begin) {
        ++poll_drains;
        poll_drain_ns += Nanoseconds(begin);
        Tick();
    }

    void Tick() {
        const auto now = Clock::now();
        if (since == Clock::time_point{}) {
            since = now;
            return;
        }
        const double window = std::chrono::duration<double>(now - since).count();
        if (window < 5.0) {
            return;
        }
        const auto rate = [window](u64 count) { return static_cast<double>(count) / window; };
        const auto ms = [window](u64 ns) { return static_cast<double>(ns) / 1e6 / window; };
        LOG_INFO(HW_GPU,
                 "Acquire drains (EDEN_NX2_ACQUIRE_WRITER_WAIT={}): {:.0f} acquires/s not met at "
                 "once ({:.0f}/s not gequal or with delayed fences); writer waits {:.0f}/s {:.2f} "
                 "ms/s, met there {:.0f}/s; drained checks {:.0f}/s {:.2f} ms/s, met after them "
                 "{:.0f}/s; suspended without one {:.0f}/s; polls {:.0f}/s, drained {:.0f}/s "
                 "{:.2f} ms/s; resumed {:.0f}/s, {:.0f} us from acquire to resume",
                 VideoCommon::GpuSplit::Get().AcquireWriterWait(), rate(unmet),
                 rate(not_applied), rate(writer_waits), ms(writer_ns), rate(met_at_writers),
                 rate(drained), ms(drained_ns), rate(met_after_drain), rate(suspended_undrained),
                 rate(polls), rate(poll_drains), ms(poll_drain_ns), rate(resumed),
                 resumed != 0 ? static_cast<double>(resume_ns) / 1e3 / static_cast<double>(resumed)
                              : 0.0);
        *this = {};
        since = now;
    }
};

} // Anonymous namespace

bool Puller::PendingAcquire::Matches(u32 current) const {
    switch (static_cast<GpuSemaphoreOperation>(operation)) {
    case GpuSemaphoreOperation::AcquireEqual:
        return current == value;
    case GpuSemaphoreOperation::AcquireGequal:
        return current >= value;
    case GpuSemaphoreOperation::AcquireMask:
        return (current & value) != 0;
    default:
        return true;
    }
}

bool Puller::CanResumeAcquire(DmaPusher& dma_pusher) {
    if (!pending_acquire) {
        return true;
    }
    // EDEN_NX2_SNAPSHOT_TRACE: when the release first shows in guest memory.
    if (DmaPusher::SnapshotTraceEnabled() &&
        pending_acquire->seen_in_memory == std::chrono::steady_clock::time_point{} &&
        pending_acquire->Matches(dma_pusher.memory_manager.Read<u32>(pending_acquire->address))) {
        pending_acquire->seen_in_memory = std::chrono::steady_clock::now();
    }
    if (PeekAcquires() && MetInMemory(dma_pusher, *pending_acquire)) {
        return true;
    }
    if (MetByQueuedRelease(dma_pusher, *pending_acquire)) {
        return true;
    }
    auto& stats = AcquireDrainStats::Get();
    ++stats.polls;
    const auto poll_begin = AcquireDrainStats::Clock::now();
    // EDEN_NX2_ACQUIRE_WRITER_WAIT: a GPU release that meets the value latches the waiter when the
    // back or the fencing thread writes it, and a CPU release is in guest memory. The other
    // writers (copies, shader stores) are drained for only every 16th unmet poll or once 1 ms
    // passed: a drain also has the back submit the work it deferred, which a fence-timed release
    // may wait for (Fritchii f362e5a5fd: Mario Kart World hung without it).
    if (WriterWait(*pending_acquire) != 0) {
        if (Nx2SemaphoreWaiters::Get().Latched(this) || MetInMemory(dma_pusher, *pending_acquire)) {
            return true;
        }
        if (++pending_acquire->unmet_polls % 16 != 0 &&
            poll_begin - pending_acquire->last_drain < std::chrono::milliseconds{1}) {
            return false;
        }
        pending_acquire->last_drain = poll_begin;
    }
    // The release may still be queued for the split GPU thread's back half (and the caches
    // ReadSemaphoreWord asks are its state).
    DrainBack(DrainSite::ResumeAcquire);
    const u32 current = ReadSemaphoreWord(dma_pusher, pending_acquire->address);
    stats.PollDrained(poll_begin);
    if (Nx2SemaphoreWaiters::Get().Latched(this)) {
        if (static std::atomic<u32> rescued{};
            !pending_acquire->Matches(current) && rescued.fetch_add(1) < 64) {
            LOG_INFO(HW_GPU, "Channel {} met semaphore {:#x} value {:#x} at its release (now {:#x})",
                     dma_pusher.channel_state.bind_id, pending_acquire->address,
                     pending_acquire->value, current);
        }
        return true;
    }
    if (pending_acquire->Matches(current)) {
        return true;
    }
    if (EqualAcquirePassed(*pending_acquire, current)) {
        if (static std::atomic<u32> passed{}; passed.fetch_add(1) < 64) {
            LOG_WARNING(HW_GPU,
                        "Channel {} met semaphore {:#x} value {:#x} once the word passed it ({:#x} "
                        "when it suspended, now {:#x}, EDEN_NX2_ACQUIRE_PASSED)",
                        dma_pusher.channel_state.bind_id, pending_acquire->address,
                        pending_acquire->value, pending_acquire->first_word, current);
        }
        return true;
    }
    // With delayed fence operations, a payload release sent after the acquire released the fences
    // waits for a fence no suspended channel makes: release them again now and then while
    // waiting. Other modes write those releases at once, and fence releases make their own
    // fence, submitted right away.
    if (const auto now = std::chrono::steady_clock::now();
        DelayedFences() && now - pending_acquire->last_fences >= std::chrono::milliseconds{1}) {
        pending_acquire->last_fences = now;
        ReleaseFences(dma_pusher);
        if (Nx2SemaphoreWaiters::Get().Latched(this) ||
            pending_acquire->Matches(ReadSemaphoreWord(dma_pusher, pending_acquire->address))) {
            return true;
        }
    }
    if (!pending_acquire->reported &&
        std::chrono::steady_clock::now() - pending_acquire->since > std::chrono::seconds{2}) {
        pending_acquire->reported = true;
        const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address);
        LOG_WARNING(HW_GPU, "Channel {} waits on semaphore {:#x} (op {}, {:#x}, now {:#x}) for 2 s; "
                    "device {:#x}, buffer cache holds GPU data: {}, words {:08x} {:08x} {:08x} {:08x}",
                    dma_pusher.channel_state.bind_id, pending_acquire->address,
                    pending_acquire->operation, pending_acquire->value, current, device.value_or(0),
                    device && dma_pusher.rasterizer->MustFlushRegion(*device, 16, VideoCommon::CacheType::All),
                    dma_pusher.memory_manager.Read<u32>(pending_acquire->address & ~GPUVAddr{0xf}),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 4),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 8),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 12));
    }
    return false;
}

bool Puller::ResumeAcquire(DmaPusher& dma_pusher) {
    if (!CanResumeAcquire(dma_pusher)) {
        return false;
    }
    // Only a resume from a suspension counts: DmaPusher::Step calls this before every entry, and
    // counting those would make the count odd ("suspended") at random.
    if (pending_acquire) {
        auto& stats = AcquireDrainStats::Get();
        ++stats.resumed;
        stats.resume_ns += AcquireDrainStats::Nanoseconds(pending_acquire->since);
        last_resume_by_gpu = Nx2SemaphoreWaiters::Get().Latched(this);
        if (DmaPusher::SnapshotTraceEnabled()) {
            using Ms = std::chrono::duration<float, std::milli>;
            const auto now = std::chrono::steady_clock::now();
            const auto seen = pending_acquire->seen_in_memory;
            const float waited = Ms(now - pending_acquire->since).count();
            const float late =
                seen == std::chrono::steady_clock::time_point{} ? -1.0f : Ms(now - seen).count();
            // Up to 20 per 10 s, so a hang late in a session still gets some.
            thread_local std::chrono::steady_clock::time_point window{};
            thread_local u32 logged{};
            if (now - window >= std::chrono::seconds{10}) {
                window = now;
                logged = 0;
            }
            if ((late >= 2.0f || waited >= 50.0f) && logged < 20) {
                ++logged;
                LOG_INFO(HW_GPU,
                         "Channel {} resumed after waiting {:.1f} ms on semaphore {:#x} value "
                         "{:#x}, met by {}; the value was in guest memory {:.1f} ms before (-1: "
                         "not seen there)",
                         dma_pusher.channel_state.bind_id, waited, pending_acquire->address,
                         pending_acquire->value,
                         last_resume_by_gpu ? "a GPU release" : "guest memory", late);
            }
        }
        pending_acquire.reset();
        dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
        Nx2SemaphoreWaiters::Get().Remove(this);
        ApplyHeldWritesAfterAcquire(dma_pusher);
    }
    return true;
}

void Puller::ProcessBindMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_DEBUG(HW_GPU, "Binding subchannel {} to engine {:#x}", method_call.subchannel, method_call.argument);
    u32 engine = method_call.argument;
    if ((engine & ~Gf100BindClassMask) != 0 && (engine & ~Gf100BindValidMask) == 0) {
        engine &= Gf100BindClassMask;
    }
    // Switch 2 binds Ampere classes; run them on the Maxwell engines (same method layout for
    // most state). Class 0 unbinds the subchannel.
    switch (engine & 0xFFFF) {
    case 0xC797: // AMPERE_B
        engine = static_cast<u32>(EngineID::MAXWELL_B);
        break;
    case 0xC7C0: // AMPERE_COMPUTE_B
        engine = static_cast<u32>(EngineID::KEPLER_COMPUTE_B);
        break;
    case 0xC7B5: // AMPERE_DMA_COPY_B
        engine = static_cast<u32>(EngineID::MAXWELL_DMA_COPY_A);
        break;
    case 0x0:
        if (g_nx2_ampere) {
            LOG_DEBUG(HW_GPU, "Switch 2 unbinds subchannel {}", method_call.subchannel);
            return;
        }
        break;
    default:
        break;
    }

    const auto engine_id = static_cast<EngineID>(engine);
    bound_engines[method_call.subchannel] = engine_id;
    switch (engine_id) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->fermi_2d, method_call.subchannel, EngineTypes::Fermi2D);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_3d, method_call.subchannel, EngineTypes::Maxwell3D);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_compute, method_call.subchannel, EngineTypes::KeplerCompute);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_dma, method_call.subchannel, EngineTypes::MaxwellDMA);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_memory, method_call.subchannel, EngineTypes::KeplerMemory);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->nv01_timer, method_call.subchannel, EngineTypes::Nv01Timer);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine {:04X}", engine_id);
        // On Switch 2 an unknown class usually means misread command words (Bananza bound 0x0f39
        // after a burst of methods on an unbound subchannel): dump the entries once.
        if (static bool dumped = false; g_nx2_ampere && !dumped) {
            dumped = true;
            dma_pusher.DumpCurrentEntry("BindObject of an unknown class");
        }
        break;
    }
}

void Puller::ProcessFenceActionMethod(DmaPusher& dma_pusher) {
    switch (regs.fence_action.op) {
    case Puller::FenceOperation::Acquire:
        // UNIMPLEMENTED_MSG("Channel Scheduling pending.");
        // WaitFence(regs.fence_action.syncpoint_id, regs.fence_value);
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
        break;
    case Puller::FenceOperation::Increment:
        OnBack([rasterizer = dma_pusher.rasterizer, id = static_cast<u32>(regs.fence_action.syncpoint_id.Value())] {
            rasterizer->SignalSyncPoint(id);
        });
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented operation {}", regs.fence_action.op.Value());
        break;
    }
}

void Puller::ProcessSemaphoreTriggerMethod(DmaPusher& dma_pusher) {
    const auto semaphoreOperationMask = 0xF;
    const auto op = GpuSemaphoreOperation(regs.semaphore_trigger & semaphoreOperationMask);
    // EDEN_NX2_SEM_WATCH=<gpu address>: log every release/acquire on the 16 bytes there.
    static const GPUVAddr watch = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
        return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
    }();
    if (watch != 0 && (regs.semaphore_address.SemaphoreAddress() & ~GPUVAddr{0xf}) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} as {} device {:#x} op {} {:#x} value {:#x} (now {:#x})",
                 dma_pusher.channel_state.bind_id, dma_pusher.memory_manager.GetID(),
                 dma_pusher.memory_manager.GpuToCpuAddress(regs.semaphore_address.SemaphoreAddress()).value_or(0),
                 static_cast<u32>(op),
                 regs.semaphore_address.SemaphoreAddress(), regs.semaphore_sequence,
                 dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress()));
    }
    if (op == GpuSemaphoreOperation::WriteLong) {
        const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
        const u32 payload = regs.semaphore_sequence;
        // RELEASE_SIZE (bit 24) set means a 4-byte release without the timestamp; writing 16
        // bytes there clobbers the words next to it (Switch 2 packs semaphores tightly).
        const auto properties = g_nx2_ampere && (regs.semaphore_trigger & (1U << 24)) != 0
                                    ? VideoCommon::QueryPropertiesFlags::IsAFence
                                    : VideoCommon::QueryPropertiesFlags::HasTimeout;
        const GPUVAddr mirror = g_nx2_ampere ? Nx2SemaphoreMirror(sequence_address) : 0;
        if (mirror != 0) {
            LOG_DEBUG(HW_GPU, "Switch 2 semaphore {:#x}={:#x} mirrored to {:#x}", sequence_address, payload, mirror);
        }
        OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, mirror, properties, payload] {
            rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload, properties, payload, 0);
            if (mirror != 0) {
                rasterizer->Query(mirror, VideoCommon::QueryType::Payload, properties, payload, 0);
            }
        });
        // The mirror too: the queue progress acquires read it.
        NoteRelease(dma_pusher, sequence_address, payload);
        if (mirror != 0) {
            NoteRelease(dma_pusher, mirror, payload);
        }
    } else if (g_nx2_ampere) {
        // Switch 2 waits on semaphores the CPU (or another channel) releases, e.g. once it has
        // written the commands this channel runs next: suspend the channel until the value is
        // met instead of reading ahead (approach from changes.patch).
        PendingAcquire acquire{
            .address = regs.semaphore_address.SemaphoreAddress(),
            .value = regs.semaphore_sequence,
            .operation = static_cast<u32>(op),
            .since = std::chrono::steady_clock::now(),
            .last_fences = std::chrono::steady_clock::now(),
            .reported = false,
        };
        if (op != GpuSemaphoreOperation::AcquireEqual && op != GpuSemaphoreOperation::AcquireGequal &&
            op != GpuSemaphoreOperation::AcquireMask) {
            LOG_ERROR(HW_GPU, "Invalid semaphore operation {}", static_cast<u32>(op));
            return;
        }
        namespace Profile = VideoCommon::Nx2Profile;
        Profile::Add(Profile::Counter::AcquireChecks, 1);
        // Counted even when met at once: an entry read from its submit-time copy stops at every
        // acquire and reads the rest from memory (DmaPusher::Step).
        ++nx2_acquires;
        // Read while profiling even with the peek off, so both sides of an A/B show the count.
        if ((PeekAcquires() || Profile::Enabled()) && MetInMemory(dma_pusher, acquire)) {
            Profile::Add(Profile::Counter::AcquiresMetInMemory, 1);
            if (PeekAcquires()) {
                ApplyHeldWritesAfterAcquire(dma_pusher);
                return;
            }
        }
        if (MetByQueuedRelease(dma_pusher, acquire)) {
            Profile::Add(Profile::Counter::AcquiresMetQueued, 1);
            ApplyHeldWritesAfterAcquire(dma_pusher);
            return;
        }
        auto& stats = AcquireDrainStats::Get();
        ++stats.unmet;
        const u32 writer_wait = WriterWait(acquire);
        if (writer_wait != 0) {
            const auto wait_begin = AcquireDrainStats::Clock::now();
            const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address);
            if (VideoCommon::GpuSplit::Get().WaitForWordWriters(device.value_or(0),
                                                                DrainSite::AcquireWriters)) {
                ++stats.writer_waits;
                stats.writer_ns += AcquireDrainStats::Nanoseconds(wait_begin);
            }
            if (MetInMemory(dma_pusher, acquire)) {
                ++stats.met_at_writers;
                stats.Tick();
                ApplyHeldWritesAfterAcquire(dma_pusher);
                return;
            }
        } else if (VideoCommon::GpuSplit::Get().AcquireWriterWait() != 0) {
            ++stats.not_applied;
        }
        if (writer_wait == 2) {
            // Registered before the word is read again, so a release that lands in between counts.
            // The fences are released on the back in order, without waiting for it.
            auto& waiters = Nx2SemaphoreWaiters::Get();
            if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
                waiters.Add(this, *device, acquire.value, acquire.operation);
            }
            OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
            if (waiters.Latched(this) || MetInMemory(dma_pusher, acquire)) {
                ++stats.met_at_writers;
                stats.Tick();
                waiters.Remove(this);
                ApplyHeldWritesAfterAcquire(dma_pusher);
                return;
            }
            pending_acquire = acquire;
            pending_acquire->first_word = dma_pusher.memory_manager.Read<u32>(acquire.address);
            pending_acquire->last_drain = AcquireDrainStats::Clock::now();
            dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
            Profile::Add(Profile::Counter::AcquireWaits, 1);
            ++stats.suspended_undrained;
            stats.Tick();
            return;
        }
        const auto drain_begin = AcquireDrainStats::Clock::now();
        DrainBack(DrainSite::Acquire);
        acquire.first_word = ReadSemaphoreWord(dma_pusher, acquire.address);
        if (acquire.Matches(acquire.first_word)) {
            Profile::Add(Profile::Counter::AcquiresMetAfterDrain, 1);
            stats.Drained(drain_begin, true);
            ApplyHeldWritesAfterAcquire(dma_pusher);
            return;
        }
        // Registered before the fences run, so a release among them that meets the value counts
        // even if a later one overwrites it.
        auto& waiters = Nx2SemaphoreWaiters::Get();
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
            waiters.Add(this, *device, acquire.value, acquire.operation);
        }
        // Not released, the back ran nothing since the read above that could write the word:
        // guest memory holds it.
        const bool released = ReleaseFences(dma_pusher);
        if (!waiters.Latched(this) &&
            !acquire.Matches(released ? ReadSemaphoreWord(dma_pusher, acquire.address)
                                      : dma_pusher.memory_manager.Read<u32>(acquire.address))) {
            pending_acquire = acquire;
            pending_acquire->last_drain = drain_begin;
            dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
            Profile::Add(Profile::Counter::AcquireWaits, 1);
            stats.Drained(drain_begin, false);
        } else {
            Profile::Add(Profile::Counter::AcquiresMetAfterFences, 1);
            stats.Drained(drain_begin, true);
            waiters.Remove(this);
            ApplyHeldWritesAfterAcquire(dma_pusher);
        }
    } else {
        DrainBack(DrainSite::Acquire);
        BreakFastDrawsAfterAcquire();
        do {
            const u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
            regs.acquire_source = true;
            regs.acquire_value = regs.semaphore_sequence;
            if (op == GpuSemaphoreOperation::AcquireEqual) {
                regs.acquire_active = true;
                regs.acquire_mode = false;
                if (word != regs.acquire_value) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireGequal) {
                regs.acquire_active = true;
                regs.acquire_mode = true;
                if (word < regs.acquire_value) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireMask) {
                if (word && regs.semaphore_sequence == 0) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else {
                LOG_ERROR(HW_GPU, "Invalid semaphore operation");
            }
        } while (false);
    }
}

void Puller::ProcessSemaphoreRelease(DmaPusher& dma_pusher) {
    const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
    const u32 payload = regs.semaphore_release;
    OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, payload] {
        rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload,
                          VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
    });
    NoteRelease(dma_pusher, sequence_address, payload);
}

void Puller::ProcessSemaphoreAcquire(DmaPusher& dma_pusher) {
    DrainBack(DrainSite::Acquire);
    BreakFastDrawsAfterAcquire();
    u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
    const auto value = regs.semaphore_acquire;
    while (word != value) {
        regs.acquire_active = true;
        regs.acquire_value = value;
        ReleaseFences(dma_pusher);
        word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
        // TODO(kemathe73) figure out how to do the acquire_timeout
        regs.acquire_mode = false;
        regs.acquire_source = false;
    }
}

// Ampere SEM_EXECUTE (method 0x1B): operation in bits 2:0 on the semaphore at
// SEM_ADDR_HI:SEM_ADDR_LO (methods 0x18/0x17) with payload SEM_PAYLOAD_LO (0x19).
void Puller::ProcessSemaphoreExecute(DmaPusher& dma_pusher, u32 argument) {
    const GPUVAddr address = (GPUVAddr{regs.reg_array[0x18] & 0x1FFFFFF} << 32) |
                             (regs.reg_array[0x17] & ~3U);
    const u32 payload = regs.reg_array[0x19];
    const u32 operation = argument & 7;
    if (operation == 1) { // RELEASE
        OnBack([rasterizer = dma_pusher.rasterizer, address, payload] {
            rasterizer->Query(address, VideoCommon::QueryType::Payload,
                              VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
        });
        NoteRelease(dma_pusher, address, payload);
        return;
    }
    if (operation <= 5) { // 6 is a reduction, 7 is undefined
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SemExecuteAcquires, 1);
    }
    const auto meets = [operation, payload](u32 word) {
        return operation == 0   ? word == payload
               : operation == 2 ? word >= payload
               : operation == 3 ? static_cast<s32>(word - payload) >= 0
               : operation == 4 ? (word & payload) != 0
               : operation == 5 ? (word & payload) == 0
                                : true;
    };
    if (operation <= 5 && MetByQueuedRelease(dma_pusher, address, operation == 2, meets)) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquiresMetQueued, 1);
        ApplyHeldWritesAfterAcquire(dma_pusher);
        return;
    }
    // Acquires: check once after flushing pending fences; never block the GPU thread on them.
    DrainBack(DrainSite::SemExecute);
    ReleaseFences(dma_pusher);
    const u32 word = dma_pusher.memory_manager.Read<u32>(address);
    const bool ready = meets(word);
    if (!ready) {
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1) < 8) {
            LOG_WARNING(HW_GPU, "SEM_EXECUTE op {} at {:#x}: {:#x} vs {:#x} not met, continuing",
                        operation, address, word, payload);
        }
    }
    ApplyHeldWritesAfterAcquire(dma_pusher);
}

/// Calls a GPU puller method.
void Puller::CallPullerMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    static const bool nx2_trace = std::getenv("EDEN_NX2_PULLER_TRACE") != nullptr;
    if (nx2_trace) {
        LOG_INFO(HW_GPU, "nx2 puller sub={} method={:#x} arg={:#x}", method_call.subchannel,
                 method_call.method, method_call.argument);
    }
    regs.reg_array[method_call.method] = method_call.argument;
    const auto method = static_cast<BufferMethods>(method_call.method);

    switch (method) {
    case BufferMethods::BindObject: {
        ProcessBindMethod(dma_pusher, method_call);
        break;
    }
    case BufferMethods::Nop:
    case BufferMethods::SemaphoreAddressHigh:
    case BufferMethods::SemaphoreAddressLow:
    case BufferMethods::SemaphoreSequencePayload:
    case BufferMethods::SyncpointPayload:
    case BufferMethods::WrcacheFlush:
        break;
    case BufferMethods::RefCnt:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->SignalReference(); });
        break;
    case BufferMethods::SyncpointOperation:
        ProcessFenceActionMethod(dma_pusher);
        break;
    case BufferMethods::WaitForIdle:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->WaitForIdle(); });
        break;
    case BufferMethods::SemaphoreOperation: {
        ProcessSemaphoreTriggerMethod(dma_pusher);
        break;
    }
    case BufferMethods::NonStallInterrupt: {
        LOG_ERROR(HW_GPU, "Special puller engine method NonStallInterrupt not implemented");
        break;
    }
    case BufferMethods::MemOpA: {
        LOG_TRACE(HW_GPU, "Memory Operation A");
        break;
    }
    case BufferMethods::MemOpB: {
        // Implement this better.
        OnBack([rasterizer = dma_pusher.rasterizer] {
            VideoCommon::NoteTextureBarrier();
            rasterizer->InvalidateGPUCache();
        });
        break;
    }
    case BufferMethods::MemOpC:
    case BufferMethods::MemOpD: {
        LOG_TRACE(HW_GPU, "Memory Operation C,D");
        break;
    }
    case BufferMethods::SemaphoreAcquire: {
        // On Ampere host classes 0x17..0x1A are SEM_ADDR_LO/HI and SEM_PAYLOAD_LO/HI.
        if (!g_nx2_ampere) {
            ProcessSemaphoreAcquire(dma_pusher);
        }
        break;
    }
    case BufferMethods::SemaphoreRelease: {
        if (g_nx2_ampere) {
            ProcessSemaphoreExecute(dma_pusher, method_call.argument);
        } else {
            ProcessSemaphoreRelease(dma_pusher);
        }
        break;
    }
    case BufferMethods::Yield: {
        // TODO(Kmather73): Research and implement this method.
        LOG_ERROR(HW_GPU, "Special puller engine method Yield not implemented");
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Special puller engine method {:X} not implemented", method);
        if (static bool dumped = false; g_nx2_ampere && !dumped) {
            dumped = true;
            dma_pusher.DumpCurrentEntry("unknown host method");
        }
        break;
    }
}

/// Calls a GPU engine method.
static bool Nx2MethodTrace() {
    static const bool trace = std::getenv("EDEN_NX2_METHOD_TRACE") != nullptr;
    return trace;
}

void Puller::CallEngineMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    const EngineID engine = bound_engines[method_call.subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 method sub={} engine={:04X} method={:#x} arg={:#x}",
                 method_call.subchannel, static_cast<u32>(engine), method_call.method,
                 method_call.argument);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU engine multivalue method.
void Puller::CallEngineMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    const EngineID engine = bound_engines[subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 multimethod sub={} engine={:04X} method={:#x} amount={} first={:#x}",
                 subchannel, static_cast<u32>(engine), method, amount, base_start[0]);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU method.
void Puller::CallMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method_call.method, method_call.subchannel);
    ASSERT(method_call.subchannel < bound_engines.size());

    if (ExecuteMethodOnEngine(dma_pusher, method_call.method)) {
        CallEngineMethod(dma_pusher, method_call);
    } else {
        CallPullerMethod(dma_pusher, method_call);
    }
}

/// Calls a GPU multivalue method.
void Puller::CallMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method, subchannel);
    ASSERT(subchannel < bound_engines.size());
    if (ExecuteMethodOnEngine(dma_pusher, method)) {
        CallEngineMultiMethod(dma_pusher, method, subchannel, base_start, amount, methods_pending);
    } else {
        for (u32 i = 0; i < amount; i++) {
            CallPullerMethod(dma_pusher, MethodCall{
                method,
                base_start[i],
                subchannel,
                methods_pending - i,
            });
        }
    }
}

/// Determines where the method should be executed.
[[nodiscard]] bool Puller::ExecuteMethodOnEngine(DmaPusher& dma_pusher, u32 method) {
    const auto buffer_method = BufferMethods(method);
    return buffer_method >= BufferMethods::NonPullerMethods;
}

} // namespace Tegra::Engines
