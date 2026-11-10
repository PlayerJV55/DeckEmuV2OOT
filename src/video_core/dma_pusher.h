// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <queue>

#include "common/bit_field.h"
#include "common/common_types.h"
#include "common/scratch_buffer.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/puller.h"

namespace Core {
class System;
}

namespace Tegra {

namespace Control {
struct ChannelState;
}

class GPU;
class MemoryManager;

enum class SubmissionMode : u32 {
    IncreasingOld = 0,
    Increasing = 1,
    NonIncreasingOld = 2,
    NonIncreasing = 3,
    Inline = 4,
    IncreaseOnce = 5
};

// Note that, traditionally, methods are treated as 4-byte addressable locations, and hence
// their numbers are written down multiplied by 4 in Docs. Here we are not multiply by 4.
// So the values you see in docs might be multiplied by 4.
// Register documentation:
// https://github.com/NVIDIA/open-gpu-doc/blob/ab27fc22db5de0d02a4cabe08e555663b62db4d4/classes/host/cla26f.h
//
// Register Description (approx):
// https://github.com/NVIDIA/open-gpu-doc/blob/ab27fc22db5de0d02a4cabe08e555663b62db4d4/manuals/volta/gv100/dev_pbdma.ref.txt
enum class BufferMethods : u32 {
    BindObject = 0x0,
    Illegal = 0x1,
    Nop = 0x2,
    SemaphoreAddressHigh = 0x4,
    SemaphoreAddressLow = 0x5,
    SemaphoreSequencePayload = 0x6,
    SemaphoreOperation = 0x7,
    NonStallInterrupt = 0x8,
    WrcacheFlush = 0x9,
    MemOpA = 0xA,
    MemOpB = 0xB,
    MemOpC = 0xC,
    MemOpD = 0xD,
    RefCnt = 0x14,
    SemaphoreAcquire = 0x1A,
    SemaphoreRelease = 0x1B,
    SyncpointPayload = 0x1C,
    SyncpointOperation = 0x1D,
    WaitForIdle = 0x1E,
    CRCCheck = 0x1F,
    Yield = 0x20,
    NonPullerMethods = 0x40,
};

struct CommandListHeader {
    union {
        u64 raw;
        BitField<0, 40, GPUVAddr> addr;
        BitField<40, 1, u64> allow_flush;
        BitField<41, 1, u64> is_push_buffer;
        BitField<42, 21, u64> size;
        BitField<63, 1, u64> sync;
    };
};
static_assert(sizeof(CommandListHeader) == sizeof(u64), "CommandListHeader is incorrect size");

union CommandHeader {
    u32 argument;
    BitField<0, 13, u32> method;
    BitField<0, 24, u32> method_count_;
    BitField<13, 3, u32> subchannel;
    BitField<16, 13, u32> arg_count;
    BitField<16, 13, u32> method_count;
    BitField<29, 3, SubmissionMode> mode;
};
static_assert(std::is_standard_layout_v<CommandHeader>, "CommandHeader is not standard layout");
static_assert(sizeof(CommandHeader) == sizeof(u32), "CommandHeader has incorrect size!");

inline CommandHeader BuildCommandHeader(BufferMethods method, u32 arg_count, SubmissionMode mode) {
    CommandHeader result{};
    result.method.Assign(static_cast<u32>(method));
    result.arg_count.Assign(arg_count);
    result.mode.Assign(mode);
    return result;
}

struct CommandList final {
    CommandList() = default;
    explicit CommandList(std::size_t size) : command_lists(size) {}
    explicit CommandList(boost::container::small_vector<CommandHeader, 512>&& prefetch_command_list_)
        : prefetch_command_list{std::move(prefetch_command_list_)} {}

    boost::container::small_vector<CommandListHeader, 512> command_lists;
    boost::container::small_vector<CommandHeader, 512> prefetch_command_list;
    /// EDEN_NX2_PB_CHECK: hash of each entry's words when it was submitted.
    std::vector<u64> submit_hashes;
    /// The entries' words when they were submitted (NX2), each entry's at submitted_offsets[i]:
    /// executed instead of the memory, which the game may already be refilling for a later frame.
    std::vector<CommandHeader> submitted_words;
    std::vector<u32> submitted_offsets;
    /// DmaPusher::suspensions when the words were copied (EDEN_NX2_PB_RESNAPSHOT).
    u64 snapshot_suspensions = 0;
    /// Counts the lists DmaPusher::CopySubmittedWords put in its in_transit on this channel.
    u64 submit_sequence = 0;
    /// When the words were copied at submit (EDEN_NX2_SNAPSHOT_TRACE).
    std::chrono::steady_clock::time_point submitted_at{};
    /// Guest time (ns) at which the game submitted this list (EDEN_NX2_REPORT_SUBMIT_TIME).
    u64 submit_ns{};
};

/// EDEN_NX2_REPORT_SUBMIT_TIME=1: Ampere GPU time reports show when the game submitted the work
/// being run, instead of when the host GPU thread got to it.
inline bool Nx2ReportSubmitTime() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_REPORT_SUBMIT_TIME");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

/// Submit time of the list the GPU is running (0: none yet), for GPU time reports.
inline std::atomic<u64> g_nx2_list_submit_ns;

/// The GPU time a report shows: when the game submitted the work, never later than now and never
/// behind an earlier report. Games time their GPU work with these reports, and the host's command
/// processing is no measure of it (Wonder took its frames for GPU-bound and switched its light
/// mask to a cheaper pass every other frame).
constexpr u64 Nx2ReportTimeNs(u64 now, u64 submitted, u64 last) {
    const u64 time = submitted != 0 && submitted < now ? submitted : now;
    return time > last ? time : last;
}

/// The GPU times reports show, never behind an earlier one (reports come from several threads).
class Nx2ReportClock {
public:
    u64 Next(u64 now) {
        std::scoped_lock lock{mutex};
        last = Nx2ReportTimeNs(now, g_nx2_list_submit_ns.load(std::memory_order_relaxed), last);
        return last;
    }

private:
    std::mutex mutex;
    u64 last{};
};

/// The GPU starts on list: its reports show its submit time (lists without one keep the last).
inline void Nx2NoteRunningList(const CommandList& list) {
    if (list.submit_ns != 0) {
        g_nx2_list_submit_ns.store(list.submit_ns, std::memory_order_relaxed);
    }
}

/// FNV-1a over pushbuffer words (EDEN_NX2_PB_CHECK).
inline u64 HashPushbufferWords(std::span<const u32> words) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (const u32 word : words) {
        hash = (hash ^ word) * 0x100000001b3ULL;
    }
    return hash;
}

/// @brief The DmaPusher class implements DMA submission to FIFOs, providing an area of memory that the
/// emulated app fills with commands and tells PFIFO to process. The pushbuffers are then assembled
/// into a "command stream" consisting of 32-bit words that make up "commands".
/// See https://envytools.readthedocs.io/en/latest/hw/fifo/dma-pusher.html#fifo-dma-pusher for
/// details on this implementation.
class DmaPusher final {
public:
    /// Counts suspensions on a semaphore acquire and the resumes after them, so it is odd while
    /// the channel is suspended (CommandList::snapshot_suspensions).
    std::atomic<u64> suspensions{0};

    /// EDEN_NX2_SNAPSHOT_TRACE (diagnostics): logs the command list copies taken after submit that
    /// changed words, and how long channels waited on semaphores.
    [[nodiscard]] static bool SnapshotTraceEnabled();

    explicit DmaPusher(Core::System& system_, MemoryManager& memory_manager_, Control::ChannelState& channel_state_);
    ~DmaPusher();

    void Push(CommandList&& entries);

    /// NX2, nvhost-gpu: copies the words of a list it submits (CommandList::submitted_words) and
    /// keeps the list in in_transit until Push queues it.
    void CopySubmittedWords(CommandList& entries);

    void DispatchCalls();

    /// Switch 2: true while the channel is suspended, on a semaphore acquire or on the GPU's copy
    /// of a command list it reads next.
    [[nodiscard]] bool HasPendingAcquire() const {
        return puller.HasPendingAcquire() || pending_list_tick.has_value();
    }
    [[nodiscard]] bool CanResumeAcquire();

    void BindSubchannel(Engines::EngineInterface* engine, u32 subchannel_id, Engines::EngineTypes engine_type) {
        subchannels[subchannel_id] = engine;
        subchannel_type[subchannel_id] = engine_type;
    }

    void BindRasterizer(VideoCore::RasterizerInterface* rasterizer);

private:
    static constexpr u32 non_puller_methods = 0x40;
    static constexpr u32 max_subchannels = 8;
    bool Step();
    /// Returns how many words were processed: fewer than given when a semaphore acquire
    /// suspended the channel.
    size_t ProcessCommands(std::span<const CommandHeader> commands, size_t word_offset = 0);
    size_t command_resume_offset{}; ///< Words of the current entry done before a suspension
    u64 list_acquires{}; ///< The puller's acquire count when the current command list started
    u64 copy_acquires{}; ///< Same, or when the rest of the list was last copied again
    std::optional<u64> snapshot_acquires; ///< Set while an entry runs from its submitted words
    /// EDEN_NX2_PB_RESNAPSHOT: copies the list's words from memory again, from word `first_word`
    /// of entry `first_entry` on; `current` for the list being parsed, not one queued behind it.
    void RefreshSnapshot(CommandList& list, size_t first_entry, size_t first_word, bool current,
                         const char* reason);
    /// EDEN_NX2_SNAPSHOT_TRACE: logs a copy taken after submit that changed words, and sums them.
    void NoteRecopy(const CommandList& list, const char* reason, u64 old_count, u64 now,
                    u32 changed_entries, u64 changed_words, size_t first_changed);
    std::vector<CommandHeader> recopy_scratch; ///< EDEN_NX2_SNAPSHOT_TRACE
    /// Whether every page of the command words has a mapping to read them through.
    [[nodiscard]] bool CommandsMapped(GPUVAddr address, size_t size) const;
    /// At a resume: copies the lists in in_transit again, as of suspension count `now` (reread
    /// false: their copies stand as of `now`).
    void CopyInTransitAgain(u64 now, bool reread);
    /// Push: takes the copy a resume took of the list on its way, if one did.
    void TakeInTransitCopy(CommandList& list);

    /// Lists CopySubmittedWords copied that Push has not queued yet, in submission order, and the
    /// copy a resume took of each since (CopyInTransitAgain), for Push to take.
    struct InTransitList {
        u64 submit_sequence = 0;
        std::vector<CommandListHeader> entries;
        std::vector<u32> offsets;
        size_t words = 0;
        bool copied = false;  ///< newer holds a copy a resume took
        bool current = false; ///< Its copy (newer, else the one at submit) stands as of suspensions
        std::vector<CommandHeader> newer;
        u64 suspensions = 0; ///< DmaPusher::suspensions its copy stands as of
    };
    std::mutex in_transit_mutex;
    std::deque<InTransitList> in_transit;
    u64 last_submit_sequence = 0; ///< Guarded by in_transit_mutex

    void SetState(const CommandHeader& command_header);

    void CallMethod(u32 argument);
    void CallMultiMethod(const u32* base_start, u32 num_methods);
    [[nodiscard]] bool WantsSegment() const;
    void ReportUnboundSubchannel();
    void WatchArgument(u32 method, u32 argument) const;

public:
    /// Logs the GPFIFO entry being processed (and the one before it), read through the GPU MMU
    /// and straight from guest memory, to find pushbuffer desyncs. Debug aid for Switch 2.
    void DumpCurrentEntry(const char* reason);
    void CheckSubmitHash(const CommandListHeader& header, u64 submit_hash, bool after_acquire);
    /// EDEN_NX2_PB_CHECK: keeps the entry's hash and word source for DumpCurrentEntry, and logs
    /// the first words of an entry a split method goes on in although it starts elsewhere.
    void RecordParsedEntry(const CommandListHeader& header, std::span<const CommandHeader> commands,
                           bool from_snapshot, bool continues_split);
    /// Reads a command list on pages the GPU wrote into command_headers, from the copies the
    /// buffer cache made after the work that wrote them or from guest memory for pages flushed
    /// since. False when not all are current (or the list isn't GPU-written): the caller drains,
    /// flushes and reads guest memory. With `pending`, a copy the GPU has not finished sets
    /// `*pending` and pending_list_tick instead of waiting (the result is false then).
    bool ReadGpuWrittenCommands(GPUVAddr address, u32 count, bool* pending = nullptr);
    /// Brings command list words the GPU wrote (still only in host buffers) to guest memory.
    void FlushGpuWrittenCommands(GPUVAddr address, size_t size);
    /// Whether the entry holds GPU-written arguments a compute dispatch reads from GPU memory
    /// instead (EDEN_NX2_INDIRECT_DISPATCH): it is then parsed from guest memory as it is.
    bool TakeIndirectGrid(GPUVAddr address, size_t size);
    /// EDEN_NX2_CMDPAGE_DIAG: what wrote a slow-path entry's pages and what its flush changed.
    void RecordGpuWrittenCommandsDiag(GPUVAddr address, size_t size, bool flushed,
                                      std::span<const u32> before,
                                      std::chrono::steady_clock::time_point start);
    /// Whether the GPU has written a page of these command words (NX2, see above).
    bool IsGpuWrittenCommands(GPUVAddr address, size_t size) const;

private:

public:
    Common::ScratchBuffer<CommandHeader> command_headers; ///< Buffer for list of commands fetched at once

    std::deque<CommandList> dma_pushbuffer; ///< Queue of command lists to be processed
    std::size_t dma_pushbuffer_subindex{};  ///< Index within a command list within the pushbuffer

    struct DmaState {
        u32 method;            ///< Current method
        u32 subchannel;        ///< Current subchannel
        u32 method_count;      ///< Current method count
        u32 length_pending;    ///< Large NI command length pending
        GPUVAddr dma_get;      ///< Currently read segment
        u64 dma_word_offset;   ///< Current word offset from address
        bool non_incrementing; ///< Current command's NI flag
        bool is_last_call;
    };

    Core::System& system;
    MemoryManager& memory_manager;
    Control::ChannelState& channel_state;

    DmaState dma_state{};

    // Switch 2: methods whose data continues in a later GPFIFO entry, set aside while other
    // command buffers run (NVN interleaves several, so more than one can be pending).
    struct ParkedMethod {
        u32 method;
        u32 subchannel;
        u32 method_count;
        bool non_incrementing;
        bool increment_once;
        GPUVAddr resume_address; ///< Where the entry that continues the method starts
        /// The engine holding the macro call's parameters so far, set aside with the method
        Engines::EngineInterface* macro_engine;
        Engines::EngineTypes macro_engine_type;
    };
    bool ResolveSplitMethod(const CommandListHeader& header);
    bool EntryCallsStream(const CommandListHeader& header);
    bool PlausibleHeader(CommandHeader command, u32& bound) const;
    u32 BoundSubchannels() const;
    bool ParsesCleanly(std::span<const u32> words, size_t start) const;
    size_t ResyncEntry(std::span<const CommandHeader> commands);
    void DropMisreadEntry(std::span<const CommandHeader> commands, size_t index,
                          size_t word_offset);
    enum class ParkedMacroAction { Resume, Drop };
    /// Sets the macro call of a parked method aside in its engine; returns the engine, or null
    /// when the method is no macro call.
    Engines::EngineInterface* ParkEngineMacro(u32 subchannel, u32 method, GPUVAddr key);
    void ParkedEngineMacro(const ParkedMethod& parked, ParkedMacroAction action);
    static constexpr size_t MaxParkedMethods = 16;

    // EDEN_NX2_SEM_WATCH=<gpu address>: log every method whose argument is the low word of an
    // address in the 16 bytes there (finds the engine and method that release a semaphore). Read
    // per channel, not at static initialization, which runs before the frontend loads
    // nx2_switches.txt.
    const u32 semaphore_watch = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
        return value ? static_cast<u32>(std::strtoull(value, nullptr, 16)) : 0U;
    }();

    // Debug aid: hashes of the words of the last entries as they were parsed.
    struct ParsedEntry {
        GPUVAddr address;
        u32 size;
        u64 hash;
        bool from_snapshot; ///< Parsed from the words copied at submit, not from memory
        /// A split method went on in it from elsewhere, and it doesn't carry the method's rest
        bool continues_split;
        std::array<u32, 4> first_words; ///< When continues_split
    };
    std::array<ParsedEntry, 64> parsed_entries{};
    size_t parsed_entry_count{};
    std::vector<ParkedMethod> parked_methods;
    GPUVAddr split_method_end{}; ///< End of the last entry that ended inside a method
    /// EDEN_NX2_PB_CHECK: the same, but not moved by an entry the method went on in from elsewhere,
    /// so going back to the method's own stream after a gather's piece isn't reported.
    GPUVAddr split_stream_end{};

    std::array<Engines::EngineInterface*, max_subchannels> subchannels{};
    std::array<Engines::EngineTypes, max_subchannels> subchannel_type;

    Engines::Puller puller;
    std::mutex sync_mutex;
    std::condition_variable sync_cv;

    VideoCore::RasterizerInterface* rasterizer = nullptr;
    /// Host GPU tick of the command list copy this channel waits for (ReadGpuWrittenCommands).
    std::optional<u64> pending_list_tick;
    /// The entry parsed next was suspended on pending_list_tick, which is done now.
    bool resumed_list_read{};

    /// EDEN_NX2_CMD_BATCH: GPU-written command entries that needed a flush.
    std::set<std::pair<GPUVAddr, size_t>> flushed_command_entries;

    // Not bitfields: the fence thread sets synced, and a bitfield write would rewrite the byte it
    // shares with dma_increment_once and signal_sync (#479).
    const bool ib_enable = true; ///< IB mode enabled
    bool dma_increment_once = false;
    bool signal_sync = false;
    std::atomic<bool> synced{false};
};

} // namespace Tegra
