// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

// Splits the GPU thread in two (NX2_GPU_THREAD_SPLIT.md). The GPU thread (the front) takes the
// CPU's commands, parses command lists and runs the puller; engine method calls and everything
// behind them (register state, macros, draws, the rasterizer and its caches) run in order on a
// second thread (the back), fed through a single-producer single-consumer ring. Work the front
// would do on back-owned state is deferred into the same ring; the front waits for the back to
// catch up (Drain) only where it needs a result from that state, e.g. a semaphore it acquires.
// On for Switch 2 titles with the 3D and compute engines' macros on the front, against copies of
// the registers they read (Maxwell3D and KeplerCompute Front*), which sends the writes they make.
// EDEN_NX2_SPLIT=1 keeps the macros on the back; EDEN_NX2_SPLIT=0 runs everything on one thread.
class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// How often the split started: state the front copied during an earlier start is stale.
    [[nodiscard]] u32 Starts() const {
        return starts;
    }
    /// Whether the front runs the engines' macros (EDEN_NX2_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }
    /// Whether the calling thread is the back.
    [[nodiscard]] static bool OnBackThread() {
        return on_back_thread;
    }
    /// Any thread: whether the back thread runs (false before it starts and once it stopped).
    [[nodiscard]] bool BackRunning() const {
        return back_running.load(std::memory_order_acquire);
    }
    /// Called on the back before it sleeps waiting for work and when another thread asks
    /// (RequestIdleHook): the rasterizer submits the work Scheduler::DeferFlush held back. Set
    /// only while the back thread is not running; without a hook the back runs as before.
    void SetIdleHook(void (*hook)(void*), void* argument) {
        idle_hook = hook;
        idle_argument = argument;
    }
    /// Any thread: has the back call the idle hook soon, between entries or at once if it sleeps.
    void RequestIdleHook() {
        hook_requested.store(true, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
    /// Front, while it only polls suspended channels: a back that ran everything runs its idle
    /// hook (the deferred submit) and sleeps now, as after a poll's drain, instead of spinning for
    /// more work first. Nothing waits for it.
    void SettleIdleBack() {
        if (hook_requested.load(std::memory_order_relaxed) || BackQuiet() ||
            consumed.load(std::memory_order_acquire) != write_position) {
            return;
        }
        RequestIdleHook();
    }
    /// Run on the back when a thread asks for it (RequestTick): the GPU's sync operations. Set
    /// only while the back thread is not running.
    void SetTickHook(void (*hook)(void*), void* argument) {
        tick_hook = hook;
        tick_argument = argument;
    }
    /// EDEN_NX2_BACK_TICK (default on): RequestTick has the back run the tick hook.
    [[nodiscard]] bool BackTicks() const {
        return back_tick.load(std::memory_order_relaxed);
    }
    /// Any thread, after queueing a sync operation it waits for: has the back run the tick hook
    /// after the entry it is running. Otherwise the operation runs after the back's next draw,
    /// or after the front sent everything it had queued before it.
    void RequestTick() {
        if (back_tick.load(std::memory_order_relaxed) &&
            back_running.load(std::memory_order_acquire)) {
            tick_requested.store(true, std::memory_order_release);
        }
    }

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// commands read without cache flushes). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words);
        NoteMethodWrite(engine, method);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1);
        NoteMethodWrite(engine, method);
    }

    /// Defer flag: the call doesn't end fast draw reuse (nx2_fast_draw.h).
    static constexpr u64 FastDrawNeutral = 1 << 7;

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    /// `writes_memory`: false only for work that can't write guest memory in command order (draws),
    /// which split_overlap reads then don't wait for.
    /// `flags`: FastDrawNeutral when `func` can't change what a draw resolves to.
    template <typename Func>
    void Defer(const Func& func, bool writes_memory = true, u64 flags = 0) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | flags | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
        if (writes_memory) {
            last_untracked_write_position = write_position;
        }
    }
    /// Defer, calling `func` with a copy of `words` (a std::span<const u32>).
    template <typename Func>
    void DeferWords(const Func& func, std::span<const u32> words, bool writes_memory = true) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t function_words = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        const size_t payload = function_words + 1 + (words.size() + 1) / 2;
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallWordsThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        entry[2 + function_words] = words.size();
        if (!words.empty()) {
            entry[1 + payload] = 0; // Padding after an odd count
            std::memcpy(entry + 3 + function_words, words.data(), words.size_bytes());
        }
        Commit(2 + payload);
        if (writes_memory) {
            last_untracked_write_position = write_position;
        }
    }
    void DeferFunction(std::function<void()>&& func);

    /// Nx2Perf split_overlap (EDEN_NX2_PERF_ON=split_overlap, or Selevet's EDEN_NX2_DMA_RANGES=1),
    /// while the split runs: front macro memory reads of pages no GPU work or query has written
    /// wait only for the work queued before them that can write memory, instead of everything.
    [[nodiscard]] static bool OverlapEnabled() {
        return overlap_enabled;
    }
    /// EDEN_NX2_DMA_SNAPSHOT: 0 off, 1 on, 2 on and every snapshot read checked against the
    /// drained read (Frizti's 9 and 10 mean 1 and 2).
    [[nodiscard]] static u32 DmaSnapshotMode() {
        static const u32 mode = [] {
            const char* const value = std::getenv("EDEN_NX2_DMA_SNAPSHOT");
            const int number = value != nullptr ? std::atoi(value) : 0;
            return number >= 10 ? 2U : number == 9 ? 1U : static_cast<u32>(std::clamp(number, 0, 2));
        }();
        return mode;
    }
    /// The ring position of everything sent so far.
    [[nodiscard]] u64 SentPosition() const {
        return write_position;
    }
    /// Whether work sent after position `since` may write [begin, end): a memory writer without
    /// a range, or a range-noted write that overlaps it (needs RangesEnabled).
    [[nodiscard]] bool MayBeWrittenSince(u64 since, GPUVAddr begin, GPUVAddr end) const {
        if (last_untracked_write_position > since) {
            return true;
        }
        for (size_t i = 0; i < range_count; ++i) {
            const RangeWrite& range =
                range_writes[(range_head + MaxRangeWrites - 1 - i) % MaxRangeWrites];
            if (range.position <= since) {
                return false;
            }
            if (range.begin < end && begin < range.end) {
                return true;
            }
        }
        return false;
    }
    /// Nx2Perf forward_reads, while the split runs: ForwardRead.
    [[nodiscard]] static bool ForwardEnabled() {
        return forward_enabled;
    }
    /// Whether the front tells the split the ranges of its memory writes (SetNextWriteRange), for
    /// split_overlap or forward_reads.
    [[nodiscard]] static bool RangesEnabled() {
        return ranges_enabled;
    }
    /// The memory the next memory-writing method sent writes, in GPU addresses (constant buffer
    /// and inline uploads, reports): reads that don't overlap it don't wait for it. `data`, when
    /// known, holds the words it writes there in the address space `space` (ForwardRead); it is
    /// read when the method is sent.
    void SetNextWriteRange(GPUVAddr begin, GPUVAddr end, const u32* data = nullptr,
                           size_t space = 0) {
        next_range_begin = begin;
        next_range_end = end;
        next_range_data = data;
        next_range_space = space;
        next_range_set = true;
    }
    /// Nx2Perf forward_reads: for a front macro memory read of `words` words at `begin` in the
    /// address space `space`. When the newest memory write sent is a constant buffer upload the
    /// back has not run yet and it covers the words, copies them from it to `out` and returns
    /// true: guest memory will hold them once the back runs it, and nothing sent after it but
    /// draws may write memory.
    bool ForwardRead(size_t space, GPUVAddr begin, u32 words, u32* out);

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Makes what was sent visible to the back.
    void Publish();
    /// Why the front waits for the back (EDEN_NX2_GPU_PROFILE counts each site).
    enum class DrainSite : u32 {
        Other,
        Commands,      ///< A command list on a page the GPU writes (DmaPusher)
        Acquire,       ///< A semaphore acquire (Puller)
        ResumeAcquire, ///< A suspended channel's acquire checked again
        SemExecute,    ///< An Ampere SEM_EXECUTE acquire
        DmaRead,       ///< An MME DMA read (Maxwell3D::FrontDmaRead)
        QueuedGmmu,    ///< A command list in the range of a queued GMMU update (Scheduler::Push)
        Parameters,    ///< Small macro or inline parameter blocks flushed (EDEN_NX2_PBFLUSH)
        SyncFence,     ///< A GP entry's sync bit (sync_memory_operations)
        SemaphoreRead, ///< A semaphore word flushed on the back before the front reads it
        ReleaseFences, ///< Fences released on the back for an acquire (Puller)
        FrontStart,    ///< The front takes an engine's state and macros over
        FrontVerify,   ///< EDEN_NX2_SPLIT_VERIFY
        RingFull,      ///< The ring to the back is full: the back is behind (WaitForSpace)
        AcquireWriters, ///< An acquire waits for the noted writers of its word only
        BackRegister,   ///< A register a macro the back ran may have written (Maxwell3D)
        BackCode,       ///< The macro code changes under the macros the back has yet to run
        Count,
    };
    /// Waits until the back executed everything sent so far.
    void Drain(DrainSite site = DrainSite::Other);
    /// split_overlap: waits until the back ran the work sent so far that can write
    /// [begin, end): the methods of memory_write_mask, every deferred call that may write memory,
    /// and the range-noted writes that overlap it. Returns whether it had to wait.
    bool WaitForMemoryWritersIn(GPUVAddr begin, GPUVAddr end, DrainSite site);
    /// Waits until the back executed the entries sent before `position` (a SentPosition()).
    /// Returns whether any of them was still queued.
    bool WaitUntil(u64 position, DrainSite site) {
        return WaitFor((std::min)(position, write_position), site);
    }

    /// EDEN_NX2_ACQUIRE_QUEUED while the split runs: 0 off, 1 for acquire-gequals, 2 for every
    /// acquire (front thread only).
    [[nodiscard]] u32 AcquireQueued() const {
        return acquire_queued;
    }
    /// Semaphore releases the front sent that the back has not run yet, by device address
    /// (front thread only). An acquire the last one to its word meets needs no drain: the back
    /// runs the release, then the work after the acquire, in order, and the host queue keeps
    /// that order. Call right after sending the release.
    void NoteQueuedRelease(u64 device_address, u32 payload);
    /// The front sent another kind of write to the word (a counter report, a reduction).
    void ForgetQueuedRelease(u64 device_address) {
        queued_releases.erase(device_address);
    }
    /// The front sent a write it can't place (through a mapping a queued GMMU update changes).
    void ForgetQueuedReleases() {
        queued_releases.clear();
    }
    /// The payload of the last release noted for `device_address`, while the back has not run it.
    [[nodiscard]] std::optional<u32> QueuedRelease(u64 device_address);

    /// EDEN_NX2_RELEASE_DRAIN_SKIP while the split runs (front thread only).
    [[nodiscard]] bool ReleaseDrainSkip() const {
        return release_drain_skip;
    }
    /// EDEN_NX2_RELEASE_DRAIN_SKIP: asked on the back once it has run everything published (and
    /// its idle hook), whether fences released there would only invalidate the GPU cache;
    /// `epoch` gets the fence work epoch it saw. Set only while the back thread is not running.
    using FenceQuietCheck = bool (*)(void* argument, u64* epoch);
    void SetFenceQuietCheck(FenceQuietCheck check, void* argument) {
        fence_quiet_check = check;
        fence_quiet_argument = argument;
    }
    enum class FenceQuiet : u32 {
        Quiet,     ///< The back found nothing to release with everything sent run
        Behind,    ///< Work sent is still queued
        Busy,      ///< The back found fence work with everything sent run
        Unchecked, ///< The back has not checked at this position
    };
    /// Front: what the back's last check found, if it had run everything sent. `epoch` gets the
    /// fence work epoch the back saw when Quiet.
    [[nodiscard]] FenceQuiet FencesQuietNow(u64& epoch) const {
        if (consumed.load(std::memory_order_acquire) != write_position) {
            return FenceQuiet::Behind;
        }
        if (fence_quiet_position.load(std::memory_order_acquire) == write_position) {
            epoch = fence_quiet_epoch.load(std::memory_order_relaxed);
            return FenceQuiet::Quiet;
        }
        return fence_busy_position.load(std::memory_order_relaxed) == write_position
                   ? FenceQuiet::Busy
                   : FenceQuiet::Unchecked;
    }

    /// Compute or copy work was just sent: what can write GPU-written command lists.
    void NoteWriter() {
        last_writer_position = write_position;
    }
    /// A 3D method that may draw into command lists was just sent (Tegra::Nx2DrawListCopies).
    void NoteDrawWriter() {
        last_draw_writer_position = write_position;
    }
    /// Waits until the back executed the compute and copy work sent so far (not what came after),
    /// and the draws too with `include_draws`.
    void WaitForWriters(bool include_draws = false);
    /// EDEN_NX2_ACQUIRE_WRITER_WAIT: waits until the back executed the queued work the front
    /// knows may write the u32 at `device_address`: compute and copies (NoteWriter) and the last
    /// release noted for it (NoteQueuedRelease); not its idle hook. Returns whether it had to wait.
    bool WaitForWordWriters(u64 device_address, DrainSite site) {
        u64 target = last_writer_position;
        if (const auto it = queued_releases.find(device_address); it != queued_releases.end()) {
            target = (std::max)(target, it->second.sent);
        }
        return WaitFor((std::min)(target, write_position), site);
    }
    /// EDEN_NX2_ACQUIRE_WRITER_WAIT while the split runs: 0 off, 1 or 2 (front thread only).
    [[nodiscard]] u32 AcquireWriterWait() const {
        return acquire_writer_wait;
    }
    /// Whether the front notes the releases it sends (NoteQueuedRelease), for
    /// EDEN_NX2_ACQUIRE_QUEUED or EDEN_NX2_ACQUIRE_WRITER_WAIT.
    [[nodiscard]] bool NotesQueuedReleases() const {
        return acquire_queued != 0 || acquire_writer_wait != 0;
    }

private:
    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
        HeapCall, ///< Word 1: a std::function<void()> DeferFunction allocated, run and deleted
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    static constexpr u32 MaxMultiWords = 8192;
    static constexpr size_t PublishEvery = 256;
    /// EDEN_NX2_PUBLISH_AFTER_WAIT: the fewest words the front publishes to a back that waits for
    /// them (Commit). A back that runs faster than the front would otherwise take one entry per
    /// publish.
    static constexpr size_t PublishAfterWait = 8;
    static constexpr size_t QueuedReleasesPrune = 4096;

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }
    template <typename Func>
    static void CallWordsThunk(const u64* payload) {
        constexpr size_t function_words = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        (*reinterpret_cast<const Func*>(payload))(
            std::span<const u32>{reinterpret_cast<const u32*>(payload + function_words + 1),
                                 static_cast<size_t>(payload[function_words])});
    }

    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words) {
        write_position += words;
        const u64 unpublished = write_position - published_local;
        // The back waits for work at the last position published (it stored it in back_idle_at
        // before it spun): it gets these words after a few, not after publish_every.
        if (unpublished >= publish_every ||
            (publish_after_wait && unpublished >= PublishAfterWait &&
             back_idle_at.load(std::memory_order_relaxed) == published_local)) {
            Publish();
        }
    }
    void WaitForSpace(size_t words);
    void NoteMethodWrite(Tegra::Engines::EngineInterface* engine, u32 method) {
        if (ranges_enabled) {
            NoteMaskedWrite(engine, method);
        }
    }
    void NoteMaskedWrite(Tegra::Engines::EngineInterface* engine, u32 method);
    void RecordRange(GPUVAddr begin, GPUVAddr end, const u32* data, size_t space);
    /// Waits until the back executed the entries before `target`.
    bool WaitFor(u64 target, DrainSite site);
    /// One more step of a front wait that has looked `spins` times: a pause up to `spin_limit`,
    /// then a yield, or with Nx2Perf front_sleep a sleep until the back stores a new position,
    /// goes quiet or stops, unless `done` (which must load with seq_cst) holds by then.
    template <typename Done>
    void FrontBackoff(u32 spins, u32 spin_limit, const Done& done);
    /// The back's stores a front_sleep wait looks at, each followed by a wake.
    void StoreConsumed(u64 position) {
        if (!front_sleep) {
            consumed.store(position, std::memory_order_release);
            return;
        }
        consumed.store(position, std::memory_order_seq_cst);
        NotifyFront();
    }
    void SetBackQuiet() {
        if (!front_sleep) {
            back_quiet.store(true, std::memory_order_release);
            return;
        }
        back_quiet.store(true, std::memory_order_seq_cst);
        NotifyFront();
    }
    void NotifyFront() {
        if (front_waiting.load(std::memory_order_seq_cst)) {
            front_wake.fetch_add(1, std::memory_order_seq_cst);
            front_wake.notify_one();
        }
    }
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);

    void BackLoop(std::stop_token stop_token);
    /// Runs the entry at `entry` (of the `words` published before the ring's end), returns its size.
    size_t Execute(const u64* entry, size_t words);
    /// Words of an entry, as Execute returns them (0: wrap to the ring's start).
    static size_t EntrySize(const u64* entry);

    Core::System* system{};
    bool active{};
    u32 starts{};
    bool front_macros{};
    std::unique_ptr<u64[]> ring;

    // Front side, in a block of its own: the front writes write_position for every entry, and
    // the back reads system and ring (above) for every entry it runs.
    alignas(128) u64 write_position{};
    u64 last_writer_position{};
    u64 last_draw_writer_position{};
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    /// EDEN_NX2_PUBLISH_AFTER_WAIT (default on): Commit publishes a few words to a back that waits.
    bool publish_after_wait{};
    Tegra::Engines::EngineInterface* front_engine{};
    u32 acquire_queued{};
    u32 acquire_writer_wait{};
    struct QueuedReleaseEntry {
        u32 payload;
        u64 sent; ///< write_position right after the release: run once consumed reaches it
    };
    std::unordered_map<u64, QueuedReleaseEntry> queued_releases;
    /// Size at which NoteQueuedRelease drops the releases the back ran.
    size_t queued_releases_prune{QueuedReleasesPrune};

    // split_overlap (front side): the write position after the last entry that may write
    // memory anywhere, and the last writes of known ranges.
    struct RangeWrite {
        GPUVAddr begin;
        GPUVAddr end;
        u64 position;
        size_t space;          ///< Address space of `data`, 0 when the words are not known
        std::vector<u32> data; ///< The words written over [begin, end), for forward_reads
    };
    static constexpr size_t MaxRangeWrites = 128;
    static inline bool ranges_enabled{};
    static inline bool overlap_enabled{};
    static inline bool forward_enabled{};
    u64 last_untracked_write_position{};
    std::array<RangeWrite, MaxRangeWrites> range_writes{};
    size_t range_count{};
    size_t range_head{};
    GPUVAddr next_range_begin{};
    GPUVAddr next_range_end{};
    const u32* next_range_data{};
    size_t next_range_space{};
    bool next_range_set{};

    // Back side, also alone: the back writes it at every engine switch, and the front reads
    // ring and active for every entry it sends.
    alignas(128) Tegra::Engines::EngineInterface* back_engine{};

    /// Whether the back is done with the Vulkan scheduler until it gets more work: set after its
    /// idle hook ran, cleared before it executes entries. Always true without an idle hook.
    [[nodiscard]] bool BackQuiet() const {
        return idle_hook == nullptr || back_quiet.load(std::memory_order_acquire) ||
               !back_running.load(std::memory_order_acquire);
    }

    static GpuSplit instance;
    static inline thread_local bool on_back_thread{};
    void (*idle_hook)(void*){};
    void* idle_argument{};
    void (*tick_hook)(void*){};
    void* tick_argument{};
    std::atomic<bool> tick_requested{};
    std::atomic<bool> back_tick{};
    /// The back, between entries: runs the tick hook if a thread asked for it. Only there, before
    /// the entries count as consumed: an idle back may share the scheduler with a front that
    /// drained it.
    void MaybeTick() {
        if (tick_requested.load(std::memory_order_relaxed) &&
            tick_requested.exchange(false, std::memory_order_acq_rel) && tick_hook != nullptr)
            [[unlikely]] {
            tick_hook(tick_argument);
        }
    }
    FenceQuietCheck fence_quiet_check{};
    void* fence_quiet_argument{};
    bool release_drain_skip{};
    /// Back: runs fence_quiet_check at `position`, everything published before it run.
    void CheckFencesQuiet(u64 position);
    /// Stored by the back before `consumed` or back_quiet: the position of its last check that
    /// found nothing to release (with the epoch it saw), or that found work; ~0 for none.
    alignas(128) std::atomic<u64> fence_quiet_position{~u64{0}};
    std::atomic<u64> fence_busy_position{~u64{0}};
    std::atomic<u64> fence_quiet_epoch{};
    alignas(128) std::atomic<bool> hook_requested{};
    std::atomic<bool> back_running{};
    std::atomic<bool> back_quiet{true};

    // 128 bytes: x86 spatial prefetchers pull 64-byte lines in pairs. back_waiting and wake are
    // always touched together by both threads, so they share a block.
    alignas(128) std::atomic<u64> published{};
    alignas(128) std::atomic<u64> consumed{};
    /// A position the front waits for (WaitFor): the back stores `consumed` once past it.
    alignas(128) std::atomic<u64> wait_target{~u64{0}};
    alignas(128) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    /// Nx2Perf front_sleep, set before the back starts: the front sleeps in FrontBackoff.
    bool front_sleep{};
    alignas(128) std::atomic<bool> front_waiting{};
    std::atomic<u32> front_wake{};
    /// The back's position when it last ran out of work, stored before it waits for more: while
    /// it equals published_local, the back waits for the words the front holds (Commit). The back
    /// writes it once per wait, the front reads it at every Commit. ~0: none since the start.
    alignas(128) std::atomic<u64> back_idle_at{~u64{0}};
    std::jthread back_thread;
};

} // namespace VideoCommon
