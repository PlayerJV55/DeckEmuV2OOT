// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdio>

#include "common/logging.h"
#include "common/nx2_title_defaults.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

[[maybe_unused]] std::atomic<u32> g_back_thread_id{};
std::atomic<bool> g_back_busy{};

// EDEN_NX2_ACQUIRE_QUEUED=1 (or 2): every acquire a release still queued for the back meets goes
// on without draining it (GpuSplit::NoteQueuedRelease), as =1 does in Selevet and Orchid;
// =gequal: acquire-gequals only. Returns 1 for gequal, 2 for every acquire.
u32 AcquireQueuedMode() {
    const std::string_view text = Common::Nx2::Setting("EDEN_NX2_ACQUIRE_QUEUED", "0");
    if (text == "gequal") {
        return 1U;
    }
    return text == "1" || text == "2" ? 2U : 0U;
}

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    if (!g_back_busy.load(std::memory_order_acquire)) {
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::fprintf(stderr, "GPU split owner check: %s on thread %lu while the back runs\n", where,
                 thread);
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        std::fprintf(stderr, "  frame %s+%#llx\n", base_name ? base_name + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(module)));
    }
    std::fflush(stderr);
#else
    (void)where;
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    static Common::Nx2::SessionValue<char> session_mode{[] {
        const std::string_view value = Common::Nx2::Setting("EDEN_NX2_SPLIT", "2");
        return value.empty() ? '\0' : value[0];
    }};
    const char mode = session_mode.Get();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand the engines back to the front (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop();
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need the caches on the front");
        return;
    }
    if (active || !allowed) {
        return;
    }
    system = &system_;
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    overlap_enabled = Nx2Perf::On(Nx2Perf::Opt::SplitOverlap);
    forward_enabled = Nx2Perf::On(Nx2Perf::Opt::ForwardReads);
    ranges_enabled = overlap_enabled || forward_enabled || DmaSnapshotMode() != 0;
    front_sleep = Nx2Perf::On(Nx2Perf::Opt::FrontSleep);
    release_drain_skip = Common::Nx2::Setting("EDEN_NX2_RELEASE_DRAIN_SKIP", "0") == "1";
    const std::string_view writer_wait = Common::Nx2::Setting("EDEN_NX2_ACQUIRE_WRITER_WAIT", "0");
    acquire_writer_wait = writer_wait == "1" ? 1U : writer_wait == "2" ? 2U : 0U;
    // EDEN_NX2_PUBLISH_AFTER_WAIT=0: a back that ran out of work gets the front's next words only
    // after publish_every of them, as before.
    publish_after_wait = Common::Nx2::Setting("EDEN_NX2_PUBLISH_AFTER_WAIT", "1") != "0";
    // EDEN_NX2_BACK_TICK=0: a guest read's flush waits for the back's next draw or for the tick
    // sent through the GPU thread's queue, as before.
    back_tick.store(Common::Nx2::Setting("EDEN_NX2_BACK_TICK", "1") != "0",
                    std::memory_order_relaxed);
    ring = std::make_unique<u64[]>(RingWords);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
    ++starts;
#ifdef ARCHITECTURE_x86_64
    // Front macros run compiled (macro_turing_jit.h) only.
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
#endif
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}{}{}",
             front_macros ? ", macros on the first" : "",
             overlap_enabled ? ", macro memory reads wait for writers only" : "",
             forward_enabled ? ", macro memory reads take queued uploads" : "");
    // The 3D engine's releases are only seen on the front with its macros there: without them,
    // trust queued releases for acquire-gequals only.
    acquire_queued = front_macros ? AcquireQueuedMode() : (std::min)(AcquireQueuedMode(), 1U);
    if (acquire_queued != 0) {
        LOG_INFO(HW_GPU, "GPU thread split: {} a queued release meets skip the drain",
                 acquire_queued == 1 ? "acquire-gequals" : "acquires");
    }
    if (release_drain_skip) {
        LOG_INFO(HW_GPU, "GPU thread split: fence releases with nothing to release skip the drain");
    }
    if (acquire_writer_wait != 0) {
        LOG_INFO(HW_GPU,
                 "GPU thread split: acquire-gequals wait for the noted writers of their word, {} "
                 "when unmet there",
                 acquire_writer_wait == 1 ? "drained" : "suspended");
    }
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    // At shutdown the front stops without a drain: free the functions DeferFunction allocated
    // for entries the back never ran, without running them.
    for (u64 position = consumed.load(std::memory_order_acquire); position < write_position;) {
        const size_t index = position & RingMask;
        const u64* const entry = &ring[index];
        if (static_cast<Kind>(entry[0] & 0xf) == Kind::HeapCall) {
            delete reinterpret_cast<std::function<void()>*>(entry[1]);
        }
        const size_t size = EntrySize(entry);
        position += size != 0 ? size : RingWords - index;
    }
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    write_position = 0;
    last_writer_position = 0;
    last_draw_writer_position = 0;
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    publish_after_wait = false;
    back_tick.store(false, std::memory_order_relaxed);
    tick_requested.store(false, std::memory_order_relaxed);
    front_engine = nullptr;
    back_engine = nullptr;
    front_macros = false;
    acquire_queued = 0;
    release_drain_skip = false;
    acquire_writer_wait = 0;
    fence_quiet_position.store(~u64{0}, std::memory_order_relaxed);
    fence_busy_position.store(~u64{0}, std::memory_order_relaxed);
    queued_releases.clear();
    queued_releases_prune = QueuedReleasesPrune;
    last_untracked_write_position = 0;
    range_count = 0;
    range_head = 0;
    next_range_set = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    wait_target.store(~u64{0}, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
    back_idle_at.store(~u64{0}, std::memory_order_relaxed);
    hook_requested.store(false, std::memory_order_relaxed);
    back_quiet.store(true, std::memory_order_relaxed);
    front_waiting.store(false, std::memory_order_relaxed);
}

template <typename Done>
void GpuSplit::FrontBackoff(u32 spins, u32 spin_limit, const Done& done) {
    if (spins < spin_limit) {
        Pause();
        return;
    }
    if (!front_sleep) {
        std::this_thread::yield();
        return;
    }
    // The back wakes a waiting front after each store a wait looks at (StoreConsumed,
    // SetBackQuiet) and when it stops; it reads front_waiting after the store, the front
    // looks again after setting it (port of #161's front_wake).
    front_waiting.store(true, std::memory_order_seq_cst);
    const u32 seen = front_wake.load(std::memory_order_seq_cst);
    if (back_running.load(std::memory_order_seq_cst) && !done()) {
        front_wake.wait(seen, std::memory_order_seq_cst);
    }
    front_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    constexpr auto site_index = static_cast<size_t>(DrainSite::RingFull);
    Nx2Profile::CountDrain(site_index);
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (write_position + words - consumed_cache <= RingWords) {
            if (spins != 0 && Nx2Profile::Enabled()) {
                Nx2Profile::AddDrainWait(site_index, Nx2Profile::Now() - start);
            }
            return;
        }
        FrontBackoff(spins, 4096, [&] {
            return write_position + words - consumed.load(std::memory_order_seq_cst) <=
                   RingWords;
        });
    }
}

void GpuSplit::Publish() {
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

static_assert(Nx2Profile::drain_site_names.size() ==
              static_cast<size_t>(GpuSplit::DrainSite::Count));

void GpuSplit::Drain(DrainSite site) {
    const auto site_index = static_cast<size_t>(site);
    if (active) {
        Nx2Profile::CountDrain(site_index);
    }
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position && BackQuiet()) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
    // Everything consumed is not enough with deferred submissions: the back submits them from
    // its idle hook afterwards, and the caller may use the scheduler once this returns.
    bool hook_asked = false;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache == write_position && BackQuiet()) {
            publish_every = 8;
            if (Nx2Profile::Enabled()) {
                Nx2Profile::AddDrainWait(site_index, Nx2Profile::Now() - start);
            }
            return;
        }
        if (consumed_cache == write_position && !hook_asked) {
            RequestIdleHook();
            hook_asked = true;
        }
        FrontBackoff(spins, 16384, [&] {
            if (consumed.load(std::memory_order_seq_cst) != write_position) {
                return false;
            }
            // Asked for above once everything is consumed; the wait ends when the hook ran.
            return !hook_asked || idle_hook == nullptr ||
                   back_quiet.load(std::memory_order_seq_cst);
        });
    }
}

void GpuSplit::NoteQueuedRelease(u64 device_address, u32 payload) {
    // Entries leave when an acquire finds them run; words no front acquire reads would stay, so
    // once there are many, drop the ones the back has run.
    if (queued_releases.size() >= queued_releases_prune) {
        const u64 done = consumed.load(std::memory_order_acquire);
        std::erase_if(queued_releases, [done](const auto& item) { return item.second.sent <= done; });
        queued_releases_prune = (std::max)(QueuedReleasesPrune, queued_releases.size() * 2);
    }
    queued_releases.insert_or_assign(device_address, QueuedReleaseEntry{payload, write_position});
}

std::optional<u32> GpuSplit::QueuedRelease(u64 device_address) {
    const auto it = queued_releases.find(device_address);
    if (it == queued_releases.end()) {
        return std::nullopt;
    }
    // Run by the back: guest memory, or the fence that writes it, has the value now.
    if (consumed.load(std::memory_order_acquire) >= it->second.sent) {
        queued_releases.erase(it);
        return std::nullopt;
    }
    return it->second.payload;
}

void GpuSplit::WaitForWriters(bool include_draws) {
    const u64 writers = include_draws
                            ? (std::max)(last_writer_position, last_draw_writer_position)
                            : last_writer_position;
    WaitFor((std::min)(writers, write_position), DrainSite::Commands);
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment) {
    SelectEngine(engine);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
    NoteMethodWrite(engine, method);
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    // An entry kind of its own, so Stop can free the ones left in the ring.
    auto* const heap = new std::function<void()>(std::move(func));
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::HeapCall);
    entry[1] = reinterpret_cast<u64>(heap);
    Commit(2);
    last_untracked_write_position = write_position;
}

void GpuSplit::NoteMaskedWrite(Tegra::Engines::EngineInterface* engine, u32 method) {
    const bool range = std::exchange(next_range_set, false);
    if (!engine->memory_write_mask[method]) {
        return;
    }
    if (range) {
        RecordRange(next_range_begin, next_range_end, next_range_data, next_range_space);
    } else {
        last_untracked_write_position = write_position;
    }
}

void GpuSplit::RecordRange(GPUVAddr begin, GPUVAddr end, const u32* data, size_t space) {
    const bool words_known = forward_enabled && data != nullptr && space != 0 &&
                             (end - begin) % sizeof(u32) == 0;
    const size_t words = static_cast<size_t>(end - begin) / sizeof(u32);
    // Word-by-word constant buffer uploads extend the last range.
    if (range_count != 0) {
        RangeWrite& last = range_writes[(range_head + MaxRangeWrites - 1) % MaxRangeWrites];
        if (last.end == begin || (begin >= last.begin && end <= last.end)) {
            // Its words stay known while every write to it is known, in the same address space.
            const bool full = last.space != 0 &&
                              last.data.size() * sizeof(u32) == last.end - last.begin;
            if (words_known && full && last.space == space &&
                (begin - last.begin) % sizeof(u32) == 0) {
                const size_t offset = static_cast<size_t>(begin - last.begin) / sizeof(u32);
                if (offset + words > last.data.size()) {
                    last.data.resize(offset + words);
                }
                std::memcpy(last.data.data() + offset, data, words * sizeof(u32));
            } else {
                last.space = 0;
                last.data.clear();
            }
            last.end = (std::max)(last.end, end);
            last.position = write_position;
            return;
        }
    }
    if (range_count == MaxRangeWrites) {
        // The oldest range falls out: if it is still queued, any read waits for it.
        const RangeWrite& oldest = range_writes[range_head];
        last_untracked_write_position = (std::max)(last_untracked_write_position, oldest.position);
    } else {
        ++range_count;
    }
    RangeWrite& range = range_writes[range_head];
    range.begin = begin;
    range.end = end;
    range.position = write_position;
    range.space = words_known ? space : 0;
    if (words_known) {
        range.data.assign(data, data + words);
    } else {
        range.data.clear();
    }
    range_head = (range_head + 1) % MaxRangeWrites;
}

bool GpuSplit::ForwardRead(size_t space, GPUVAddr begin, u32 words, u32* out) {
    if (range_count == 0 || words == 0 || space == 0) {
        return false;
    }
    const RangeWrite& newest = range_writes[(range_head + MaxRangeWrites - 1) % MaxRangeWrites];
    const GPUVAddr end = begin + u64{words} * sizeof(u32);
    // Run by the back already: guest memory has it. A write sent after it that may land anywhere
    // (GPU addresses only tell writes through one mapping apart) may change the words again.
    if (newest.position <= consumed.load(std::memory_order_acquire) ||
        last_untracked_write_position > newest.position || newest.space != space ||
        begin < newest.begin || end > newest.end || (begin - newest.begin) % sizeof(u32) != 0 ||
        newest.data.size() * sizeof(u32) != newest.end - newest.begin) {
        return false;
    }
    std::memcpy(out, newest.data.data() + (begin - newest.begin) / sizeof(u32),
                size_t{words} * sizeof(u32));
    return true;
}

bool GpuSplit::WaitForMemoryWritersIn(GPUVAddr begin, GPUVAddr end, DrainSite site) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    u64 target = last_untracked_write_position;
    for (size_t i = 0; i < range_count; ++i) {
        const RangeWrite& range =
            range_writes[(range_head + MaxRangeWrites - 1 - i) % MaxRangeWrites];
        if (range.position <= consumed_cache) {
            break; // The older ones ran too: positions only grow toward the head
        }
        if (range.begin < end && begin < range.end) {
            target = (std::max)(target, range.position);
        }
    }
    return WaitFor((std::min)(target, write_position), site);
}

bool GpuSplit::WaitFor(u64 target, DrainSite site) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= target) {
        return false;
    }
    const auto site_index = static_cast<size_t>(site);
    Nx2Profile::CountDrain(site_index);
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Now() : 0;
    wait_target.store(target, std::memory_order_seq_cst);
    Publish();
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) {
            wait_target.store(~u64{0}, std::memory_order_relaxed);
            if (Nx2Profile::Enabled()) {
                Nx2Profile::AddDrainWait(site_index, Nx2Profile::Now() - start);
            }
            return true;
        }
        FrontBackoff(spins, 16384,
                     [&] { return consumed.load(std::memory_order_seq_cst) >= target; });
    }
}

size_t GpuSplit::Execute(const u64* entry, size_t words) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if (!engine->reports_fast_draw_breaks) {
            Nx2FastDraw::Break(Nx2FastDraw::Reason::OtherEngine);
        }
        if (engine->fast_maxwell3d && !Nx2Profile::Enabled()) {
            // As below without the profile zones and the virtual call (Maxwell3D is final).
            using Tegra::Engines::Maxwell3D;
            auto* const maxwell3d = static_cast<Maxwell3D*>(engine);
            constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
            if (method - cb_data < 16 && (head & HasSegment) == 0 &&
                Nx2Perf::On(Nx2Perf::Opt::CbBatch)) {
                // Single constant buffer words in a row (macros write them one by one) go up in
                // one write. They are executable: each entry would only consume the sink first.
                std::array<u32, 64> methods;
                std::array<u32, 64> arguments;
                methods[0] = method;
                arguments[0] = argument;
                u32 count = 1;
                while (count < methods.size() && count < words) {
                    const u64 next = entry[count];
                    const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
                    if (static_cast<Kind>(next & 0xf) != Kind::Method || (next & HasSegment) != 0 ||
                        next_method - cb_data >= 16) {
                        break;
                    }
                    methods[count] = next_method;
                    arguments[count] = static_cast<u32>(next >> 32);
                    ++count;
                }
                maxwell3d->ConsumeSink(*system);
                if (count == 1) {
                    maxwell3d->CallMethod(*system, method, argument, true);
                } else {
                    maxwell3d->CallConstBufferWords(*system, methods.data(), arguments.data(),
                                                    count);
                }
                return count;
            }
            if ((head & Direct) != 0) {
                maxwell3d->ConsumeSink(*system);
                maxwell3d->CallMethod(*system, method, argument, true);
            } else if (!maxwell3d->execution_mask[method]) {
                maxwell3d->method_sink.emplace_back(method, argument);
            } else {
                maxwell3d->ConsumeSink(*system);
                if ((head & HasSegment) != 0) {
                    maxwell3d->current_dma_segment = entry[1];
                }
                maxwell3d->CallMethod(*system, method, argument, (head & IsLast) != 0);
            }
            return (head & HasSegment) != 0 ? 2 : 1;
        }
        if ((head & Direct) != 0) {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        if (!engine->reports_fast_draw_breaks) {
            Nx2FastDraw::Break(Nx2FastDraw::Reason::OtherEngine);
        }
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::Call: {
        if ((head & FastDrawNeutral) == 0) {
            Nx2FastDraw::Break(Nx2FastDraw::Reason::Call);
        }
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        Nx2Profile::Scope call_scope{Nx2Profile::Zone::DeferredCall};
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    case Kind::HeapCall: {
        Nx2FastDraw::Break(Nx2FastDraw::Reason::Call);
        auto* const func = reinterpret_cast<std::function<void()>*>(entry[1]);
        Nx2Profile::Scope call_scope{Nx2Profile::Zone::DeferredCall};
        (*func)();
        delete func;
        return 2;
    }
    }
    return 1;
}

size_t GpuSplit::EntrySize(const u64* entry) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
    case Kind::HeapCall:
        return 2;
    case Kind::Method:
        return (head & HasSegment) != 0 ? 2 : 1;
    case Kind::Multi:
        return ((head & HasSegment) != 0 ? 3 : 2) + (static_cast<u32>(head >> 32) + 1) / 2;
    case Kind::Call:
        return 2 + static_cast<size_t>((head >> 8) & 0xffffff);
    }
    return 1;
}

void GpuSplit::CheckFencesQuiet(u64 position) {
    if (!release_drain_skip || fence_quiet_check == nullptr) {
        return;
    }
    u64 epoch{};
    if (fence_quiet_check(fence_quiet_argument, &epoch)) {
        fence_quiet_epoch.store(epoch, std::memory_order_relaxed);
        fence_quiet_position.store(position, std::memory_order_release);
    } else {
        fence_quiet_position.store(~u64{0}, std::memory_order_relaxed);
        fence_busy_position.store(position, std::memory_order_relaxed);
    }
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    Common::SetCurrentThreadName("GPUBack");
    on_back_thread = true;
    back_running.store(true, std::memory_order_release);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    Common::PinCurrentThread(Common::PinnedRole::GpuBack);
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    // EDEN_NX2_BACK_SPIN=<pauses>: how long the back spins for more work before it sleeps (0:
    // not at all), to weigh the spin's latency against its power on a power-capped CPU.
    static const u32 back_spin = [] {
        const char* value = std::getenv("EDEN_NX2_BACK_SPIN");
        return value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 8192U;
    }();
    u64 read_position = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            // The front publishes what it holds at its next commit (a few words) from now on.
            back_idle_at.store(read_position, std::memory_order_relaxed);
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < back_spin && available == read_position &&
                                !hook_requested.load(std::memory_order_relaxed);
                 ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position && idle_hook != nullptr) {
                hook_requested.store(false, std::memory_order_seq_cst);
                // The front may already see everything consumed: Drain() waits for back_quiet
                // too, so this submit can't overlap whatever the front does next.
                if (!back_quiet.load(std::memory_order_relaxed)) {
                    idle_hook(idle_argument);
                    CheckFencesQuiet(read_position);
                    SetBackQuiet();
                }
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once. A hook request
                // made after the hook ran runs it before sleeping.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                if (available == read_position && !stop_token.stop_requested() &&
                    !hook_requested.load(std::memory_order_seq_cst)) {
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        u64 last_store = read_position;
        if (idle_hook != nullptr) {
            // Before any entry runs, so a front that sees these entries consumed also sees this.
            back_quiet.store(false, std::memory_order_seq_cst);
        }
        g_back_busy.store(true, std::memory_order_relaxed);
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            const size_t size =
                Execute(&ring[index], (std::min)(available - read_position, RingWords - index));
            read_position += size != 0 ? size : RingWords - index;
            if (idle_hook != nullptr && hook_requested.load(std::memory_order_relaxed)) {
                hook_requested.store(false, std::memory_order_seq_cst);
                idle_hook(idle_argument);
            }
            // A guest thread waits for a flush: run it now, not after the draws and compute work
            // still queued here, or after everything the front holds (the guest synchronized with
            // the GPU work it reads before it read, so that work ran on this thread already).
            MaybeTick();
            // Hand ring space back now and then while working through a long batch, and at once
            // when the front waits for this position (WaitFor).
            if ((read_position - last_store >= 4096 ||
                 read_position >= wait_target.load(std::memory_order_relaxed)) &&
                read_position != available) {
                StoreConsumed(read_position);
                last_store = read_position;
            }
        }
        g_back_busy.store(false, std::memory_order_release);
        if (idle_hook == nullptr) {
            CheckFencesQuiet(read_position);
        }
        StoreConsumed(read_position);
    }
    if (!front_sleep) {
        back_running.store(false, std::memory_order_release);
        return;
    }
    back_running.store(false, std::memory_order_seq_cst);
    front_wake.fetch_add(1, std::memory_order_seq_cst);
    front_wake.notify_all();
}

} // namespace VideoCommon
