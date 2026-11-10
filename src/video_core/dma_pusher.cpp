// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "common/nx2_title_defaults.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/nx2_profile.h"
#include "video_core/dma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_cmdpage_diag.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra {

constexpr u32 MacroRegistersStart = 0xE00;
constexpr u32 ComputeInline = 0x6D;

// A header's method address is bits 11:0: no class has a method at 0x1000 or above, so a header
// naming one is a misread word. Ocarina of Time's loading read one as a 5700-word method 0x1300
// that went on through the next frames' entries; a semaphore release in them never ran and three
// channels waited on it for good (0 fps). The rest of such an entry is dropped instead.
// EDEN_NX2_KEEP_MISREAD_HEADERS runs these methods as before.
constexpr u32 MethodAddressLimit = 0x1000;

static bool DropMisreadHeaders() {
    static const bool drop = std::getenv("EDEN_NX2_KEEP_MISREAD_HEADERS") == nullptr;
    return drop;
}

// When a semaphore acquire of a command list is met, copy the rest of the list again and go on
// with the copy instead of reading memory as the list is parsed; also copy the lists queued
// behind the acquire again when the channel resumes from it, and lists copied during or before a
// suspension when they are queued (DmaPusher::Step, Push). Read at parse time, they could
// already hold a later frame's commands (upstream 9fea7d8379: Hyrule Warriors' fight freezes,
// the same unbound-subchannel bursts as Bananza's first cave). EDEN_NX2_PB_RESNAPSHOT=1 copies
// the current list only, =0 reads memory after a met acquire as before.
enum class Resnapshot { Off, List, Queued };
static Resnapshot ResnapshotMode() {
    static const Resnapshot mode = [] {
        const char* value = std::getenv("EDEN_NX2_PB_RESNAPSHOT");
        if (value == nullptr) {
            return Resnapshot::Queued;
        }
        return value[0] == '2'   ? Resnapshot::Queued
               : value[0] == '1' ? Resnapshot::List
                                 : Resnapshot::Off;
    }();
    return mode;
}

// A list's copy goes stale only when its channel suspends on an acquire: it is copied again when
// the channel resumes, as Lichii and Fritchii do. An acquire met at once no longer sends the rest
// of the list to a read of memory at parse time, which in Ocarina of Time could find the ring
// refilled for a later frame: misread headers, lost semaphore releases and channels waiting for
// good. EDEN_NX2_PB_ACQUIRE_COPY=1 copies the rest again at every met acquire, as before.
static bool CopyAtEveryAcquire() {
    static const bool every = [] {
        const char* value = std::getenv("EDEN_NX2_PB_ACQUIRE_COPY");
        return value != nullptr && value[0] == '1';
    }();
    return every;
}

// With EDEN_NX2_PB_RESNAPSHOT=2, a resume also copies again the lists submitted but not queued
// here yet. Copied when Push queued them instead, they were copied up to half a second after the
// submit at the start of Ocarina of Time gameplay (the GPU thread building pipelines), when the
// game had refilled the memory for a later frame: misread headers, entries waiting for words that
// never came, then a freeze. Off unless EDEN_NX2_PB_RESNAPSHOT_IN_TRANSIT=1: Push copies them.
bool DmaPusher::SnapshotTraceEnabled() {
    static const bool enabled = std::getenv("EDEN_NX2_SNAPSHOT_TRACE") != nullptr;
    return enabled;
}

// EDEN_NX2_SNAPSHOT_TRACE: the words in which a copy taken again differs from the one it replaces.
static u32 ChangedWords(std::span<const CommandHeader> old_words,
                        std::span<const CommandHeader> new_words) {
    u32 changed = 0;
    for (size_t i = 0; i < old_words.size(); ++i) {
        changed += old_words[i].argument != new_words[i].argument ? 1 : 0;
    }
    return changed;
}

static bool InTransitCopies() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PB_RESNAPSHOT_IN_TRANSIT");
        return ResnapshotMode() == Resnapshot::Queued && value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// EDEN_NX2_PB_RESNAPSHOT_AFTER=cpu: a resume copies command lists again only after a release the
// game wrote. One the emulated GPU wrote comes when the GPU thread gets to it, maybe long after the
// game wrote the commands, and the game may have reused the memory since (Ocarina of Time:
// channel 2's semaphore values a frame ahead, other commands in place of channel 1's). The copies
// taken so far stand then. =all (the default) copies again after every release.
static bool CopyOnlyAfterGameReleases() {
    static Common::Nx2::SessionValue<bool> only{[] {
        return Common::Nx2::Setting("EDEN_NX2_PB_RESNAPSHOT_AFTER", "all") == "cpu";
    }};
    return only.Get();
}

namespace {
// Compute dispatches and DMA copies write the command lists ReadGpuWrittenCommands reads. The
// register writes that set them up only store a value: marking those too made every list fetch
// after one wait for all the work queued before it (draws included).
bool WritesMemory(Engines::EngineInterface* engine, Engines::EngineTypes type, u32 method,
                  u32 count) {
    if (type == Engines::EngineTypes::KeplerCompute) {
        const auto* const compute = static_cast<const Engines::KeplerCompute*>(engine);
        for (u32 i = 0; i < count; ++i) {
            if (compute->MayWriteMemory(method + i)) {
                return true;
            }
        }
        return false;
    }
    if (type == Engines::EngineTypes::MaxwellDMA) {
        for (u32 i = 0; i < count; ++i) {
            const u32 current = method + i;
            if (current >= MacroRegistersStart ||
                (current < engine->execution_mask.size() && engine->execution_mask[current])) {
                return true;
            }
        }
    }
    return false;
}

// Draws can write command lists too with EDEN_NX2_DRAW_LIST_COPIES (Tegra::Nx2DrawListCopies):
// reads of GPU-written lists then wait for them as well.
bool DrawMayWriteLists(Engines::EngineTypes type) {
    return type == Engines::EngineTypes::Maxwell3D &&
           Tegra::g_nx2_has_command_list_pages.load(std::memory_order_relaxed) &&
           Tegra::Nx2DrawListCopies();
}
} // Anonymous namespace

DmaPusher::DmaPusher(Core::System& system_, MemoryManager& memory_manager_, Control::ChannelState& channel_state_)
    : system{system_}
    , memory_manager{memory_manager_}
    , channel_state{channel_state_}
    , signal_sync{false}
    , synced{false}
{}

DmaPusher::~DmaPusher() = default;

bool DmaPusher::CanResumeAcquire() {
    if (pending_list_tick) {
        return rasterizer->IsGpuTickDone(*pending_list_tick);
    }
    return puller.CanResumeAcquire(*this);
}

void DmaPusher::Push(CommandList&& entries) {
    if (InTransitCopies() && !entries.submitted_offsets.empty()) {
        TakeInTransitCopy(entries);
    }
    // EDEN_NX2_PB_RESNAPSHOT=2: copied during a suspension, or before one that ended since, and
    // not copied again on its way: copy again now, unless the channel is still suspended (its
    // resume copies the queued lists).
    if (ResnapshotMode() == Resnapshot::Queued && !entries.submitted_offsets.empty()) {
        const u64 now = suspensions.load(std::memory_order_acquire);
        if ((now & 1) == 0 && entries.snapshot_suspensions != now) {
            RefreshSnapshot(entries, 0, 0, false, "when queued");
        }
    }
    dma_pushbuffer.push_back(std::move(entries));
}

void DmaPusher::DispatchCalls() {
    // The sub-index is kept: a channel suspended on a semaphore resumes inside its command list.
    dma_state.is_last_call = true;
    const bool forward_requests = GPU::ReadRequestsEarly() && VideoCommon::GpuSplit::Get().Active();
    while (system.IsPoweredOn()) {
        if (!Step()) {
            break;
        }
        if (forward_requests) {
            system.GPU().ForwardSyncRequests();
        }
    }
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer([gpu = &system.GPU()] {
            gpu->FlushCommands();
            gpu->OnCommandListEnd();
        });
        split.Publish();
        return;
    }
    system.GPU().FlushCommands();
    system.GPU().OnCommandListEnd();
}

namespace {

enum class LiveReadReason : u32 {
    Resumed,
    FlushedParameters,
    AcquireSinceCopy,
    SuspendedCopy,
    SuspensionsChanged,
    GpuWritten,
    SafeDirty,
    Count,
};

/// EDEN_NX2_LIVE_READ_CHECK: compares the words an entry was parsed from with its copy.
void CheckLiveRead(LiveReadReason reason, s32 channel, const CommandListHeader& header,
                   std::span<const CommandHeader> parsed, const CommandHeader* copy) {
    constexpr size_t count = static_cast<size_t>(LiveReadReason::Count);
    static std::array<std::atomic<u64>, count> checked{};
    static std::array<std::atomic<u64>, count> differing{};
    static std::atomic<u64> total_checked{};
    static constexpr std::array<const char*, count> names{
        "resumed entry",           "flushed parameters",       "acquire met since the copy",
        "copied during a suspension", "suspension count changed", "GPU-written page",
        "safe read of dirty memory"};
    const size_t r = static_cast<size_t>(reason);
    u32 first = ~0U;
    u32 words = 0;
    for (u32 i = 0; i < parsed.size(); ++i) {
        if (copy[i].argument != parsed[i].argument) {
            first = (std::min)(first, i);
            ++words;
        }
    }
    ++checked[r];
    const u64 n = total_checked.fetch_add(1) + 1;
    if (words != 0 && differing[r].fetch_add(1) < 4) {
        LOG_WARNING(HW_GPU,
                    "Channel {} entry {:#x}+{:#x} parsed from memory ({}) differs from its copy in "
                    "{} words, first at word {}",
                    channel, u64{header.addr}, u64{header.size}, names[r], words, first);
    }
    if ((n % 10000) == 0) {
        for (size_t i = 0; i < count; ++i) {
            LOG_INFO(HW_GPU, "Entries parsed from memory ({}): {} checked, {} differ from the copy",
                     names[i], checked[i].load(), differing[i].load());
        }
    }
}

} // Anonymous namespace

bool DmaPusher::Step() {
    if (!ib_enable || dma_pushbuffer.empty()) {
        return false;
    }
    // Waiting for the GPU's copy of a command list this channel reads next (ReadGpuWrittenCommands).
    if (pending_list_tick) {
        if (!rasterizer->IsGpuTickDone(*pending_list_tick)) {
            return false;
        }
        pending_list_tick.reset();
        resumed_list_read = true;
    }
    // Only for the entry that suspended: an acquire still waiting, the safe-read path or a stopped
    // split must not leave it set for a later list (which would then skip its writer wait).
    SCOPE_EXIT {
        resumed_list_read = false;
    };
    const u64 suspensions_before = suspensions.load(std::memory_order_relaxed);
    if (!puller.ResumeAcquire(*this)) {
        return false;
    }
    // EDEN_NX2_PB_RESNAPSHOT=2: the lists queued behind the acquire this channel waited on may
    // hold commands the game wrote after their submit, before it released the acquire: copy them
    // again (the current list below, with =1 too). Later than their copy at submit: with the
    // present a frame ahead the game may have refilled them for the next frame by now.
    const Resnapshot resnapshot = ResnapshotMode();
    const bool resumed = suspensions.load(std::memory_order_relaxed) != suspensions_before;
    // After a release the emulated GPU wrote, the copies stand (CopyOnlyAfterGameReleases).
    const bool reread = !(resumed && puller.LastResumeByGpu() && CopyOnlyAfterGameReleases());
    const auto keep = [this](CommandList& list) {
        if (!list.submitted_offsets.empty()) {
            list.snapshot_suspensions = suspensions.load(std::memory_order_relaxed);
        }
    };
    if (resnapshot == Resnapshot::Queued && resumed) {
        for (size_t i = 1; i < dma_pushbuffer.size(); ++i) {
            if (reread) {
                RefreshSnapshot(dma_pushbuffer[i], 0, 0, false, "after a semaphore wait (queued)");
            } else {
                keep(dma_pushbuffer[i]);
            }
        }
        // And the lists submitted but not queued here yet (InTransitCopies).
        if (InTransitCopies()) {
            CopyInTransitAgain(suspensions.load(std::memory_order_relaxed), reread);
        }
    }
    if (resnapshot != Resnapshot::Off && resumed && !CopyAtEveryAcquire()) {
        if (reread) {
            RefreshSnapshot(dma_pushbuffer.front(), dma_pushbuffer_subindex,
                            command_resume_offset, true, "after a semaphore wait");
        } else {
            keep(dma_pushbuffer.front());
        }
    }

    CommandList& command_list = dma_pushbuffer.front();
    Nx2NoteRunningList(command_list);

    const size_t prefetch_size = command_list.prefetch_command_list.size();
    const size_t command_list_size = command_list.command_lists.size();

    if (prefetch_size == 0 && command_list_size == 0) {
        dma_pushbuffer.pop_front();
        dma_pushbuffer_subindex = 0;
        return true;
    }

    if (prefetch_size > 0) {
        const std::span<const CommandHeader> commands{command_list.prefetch_command_list};
        command_resume_offset +=
            ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        if (puller.HasPendingAcquire()) {
            return false;
        }
        command_resume_offset = 0;
        dma_pushbuffer.pop_front();
        return true;
    }

    auto& current_command = command_list.command_lists[dma_pushbuffer_subindex];
    const CommandListHeader& header = current_command;
    dma_state.dma_get = header.addr;
    if (dma_pushbuffer_subindex == 0 && command_resume_offset == 0) {
        list_acquires = puller.Nx2AcquireCount();
        copy_acquires = list_acquires;
    } else if (resnapshot != Resnapshot::Off && CopyAtEveryAcquire() &&
               copy_acquires != puller.Nx2AcquireCount()) {
        // An acquire was met since the list's words were copied, at once or after a suspension:
        // what it gates may have been written after the submit. Copy the rest of the list now
        // rather than read it as it is parsed: after a long stall on the way (pipeline compiles)
        // the game may have refilled that memory with a later frame's commands.
        RefreshSnapshot(command_list, dma_pushbuffer_subindex, command_resume_offset, true,
                        "after a met acquire");
        copy_acquires = puller.Nx2AcquireCount();
    }

    if (signal_sync && !synced) {
        std::unique_lock lk(sync_mutex);
        sync_cv.wait(lk, [this]() { return synced.load(); });
        signal_sync = false;
        synced = false;
    }

    if (header.size > 0) {
        bool flushed_parameters = false;
        // EDEN_NX2_PB_CHECK: the method goes on in an entry that starts neither where the last
        // piece ended nor where its own stream goes on (split_stream_end).
        bool from_elsewhere = false;
        bool continues_split = false;
        if (command_resume_offset == 0 && g_nx2_ampere) {
            if (static u32 split_logs{}; dma_state.method_count != 0 && split_logs < 64) {
                ++split_logs;
                LOG_DEBUG(HW_GPU, "Split method {:#x} ({} left) on ch {}: next entry {:#x} size {} raw {:#x} (level bit {}, priv bit {}), expected {:#x}, parked {}",
                         dma_state.method, dma_state.method_count, channel_state.bind_id,
                         u64{header.addr}, u64{header.size}, header.raw, u64{header.is_push_buffer},
                         u64{header.allow_flush}, split_method_end, parked_methods.size());
                std::array<u32, 8> next_words{};
                std::array<u32, 4> prev_words{};
                memory_manager.ReadBlockUnsafe(header.addr, next_words.data(),
                                         std::min<size_t>(header.size, 8) * sizeof(u32));
                memory_manager.ReadBlockUnsafe(split_method_end - sizeof(prev_words), prev_words.data(),
                                         sizeof(prev_words));
                LOG_DEBUG(HW_GPU, "  previous entry ends {:08x}; next entry starts {:08x}",
                          fmt::join(prev_words, " "), fmt::join(next_words, " "));
            }
            from_elsewhere =
                dma_state.method_count != 0 && header.addr.Value() != split_stream_end;
            // Before the dirty check below, which reads the method that goes on in this entry.
            continues_split = ResolveSplitMethod(header) && from_elsewhere;
        }
        if (subchannels[dma_state.subchannel] && dma_state.method_count) {
            const auto engine = subchannel_type[dma_state.subchannel];
            const bool kepler_payload = engine == Engines::EngineTypes::KeplerCompute && dma_state.method == ComputeInline && dma_state.non_incrementing;
            const bool macro_payload = engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart;
            if (kepler_payload || macro_payload) {
                const size_t words = std::min<size_t>(dma_state.method_count, header.size);
                auto* const engine_state = subchannels[dma_state.subchannel];
                // PokeDinner flushes small GPU-written parameter blocks before reading them.
                // Only words on pages GPU work has written can be newer in the caches: with the
                // split, =1 waits for its back for those blocks only; =2 waits for every block,
                // which is what =1 did before.
                static const char flush_mode = [] {
                    const char* value = std::getenv("EDEN_NX2_PBFLUSH");
                    return value == nullptr ? '0' : value[0];
                }();
                const bool flush_parameters =
                    words <= 64 &&
                    (flush_mode == '2' ||
                     (flush_mode == '1' &&
                      (!VideoCommon::GpuSplit::Get().Active() ||
                       IsGpuWrittenCommands(dma_state.dma_get, words * sizeof(u32)))));
                if (flush_parameters) {
                    const GPUVAddr address = dma_state.dma_get;
                    const size_t bytes = words * sizeof(u32);
                    auto flush = [this, address, bytes, &flushed_parameters] {
                        if (memory_manager.IsMemoryDirty(address, bytes, VideoCommon::CacheType::BufferCache)) {
                            memory_manager.FlushRegion(address, bytes, VideoCommon::CacheType::BufferCache);
                            flushed_parameters = true;
                        }
                    };
                    auto& split = VideoCommon::GpuSplit::Get();
                    if (split.Active()) {
                        split.Defer(flush);
                        split.Drain(VideoCommon::GpuSplit::DrainSite::Parameters);
                    } else {
                        flush();
                    }
                    if (flushed_parameters) {
                        static std::atomic<u32> flush_logs{};
                        if (flush_logs.fetch_add(1) < 20) {
                            LOG_INFO(HW_GPU, "Flushed GPU-written parameters at {:#x} ({} words)",
                                     address, words);
                        }
                    }
                }
                if (flushed_parameters) {
                    engine_state->current_dirty = false;
                } else if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
                    // The caches behind IsMemoryDirty belong to the split's back thread.
                    split.Defer([engine_state, memory = &memory_manager, address = GPUVAddr{dma_state.dma_get},
                                 bytes = words * sizeof(u32)] {
                        engine_state->current_dirty = memory->IsMemoryDirty(address, bytes);
                    }, false, VideoCommon::GpuSplit::FastDrawNeutral);
                } else {
                    engine_state->current_dirty = memory_manager.IsMemoryDirty(dma_state.dma_get, words * sizeof(u32));
                }
            }
        }
        if (command_resume_offset == 0) {
            if (dma_pushbuffer_subindex < command_list.submit_hashes.size()) {
                CheckSubmitHash(header, command_list.submit_hashes[dma_pushbuffer_subindex],
                                puller.Nx2AcquireCount() != list_acquires);
            }
        }
        // A resumed entry is read again: the words after the acquire may be new by now.
        // A safe read flushes the caches, which belong to the split's back thread while it runs
        // (the split only starts with unsafe reads; the settings can change mid-game).
        const bool use_safe = !VideoCommon::GpuSplit::Get().Active() &&
                              (Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh() : Settings::IsDMALevelSafe());
        // The words as submitted, unless this is a resumed entry or the GPU has newer ones the
        // safe read below would flush (with the split, the front can't tell: it reads memory
        // unflushed anyway).
        // Words the GPU writes after the submit (title 1's culling output, drawn indirectly) are
        // newer than the copy too: they go through FlushGpuWrittenCommands or the safe read as
        // before. So are words after a semaphore acquire: NVN2 gates command buffers with one the
        // CPU releases after writing them (NX2_NOTES row 50), possibly after the submit; a met
        // acquire doesn't suspend the channel, so the copy is left there and the rest of the
        // entry, and of its command list, is read from memory (with EDEN_NX2_PB_RESNAPSHOT, from
        // the copy taken again above, resumed entries too).
        const u64 suspensions_now = suspensions.load(std::memory_order_acquire);
        const bool copy_current =
            CopyAtEveryAcquire() ? puller.Nx2AcquireCount() == copy_acquires
                                 : (command_list.snapshot_suspensions & 1) == 0 &&
                                       command_list.snapshot_suspensions == suspensions_now;
        const bool submitted = (command_resume_offset == 0 || resnapshot != Resnapshot::Off) &&
                               dma_pushbuffer_subindex < command_list.submitted_offsets.size() &&
                               !flushed_parameters && copy_current &&
                               !(use_safe && memory_manager.IsMemoryDirty(dma_state.dma_get,
                                                                          header.size * sizeof(u32))) &&
                               !IsGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
        // EDEN_NX2_LIVE_READ_CHECK: an entry parsed from memory instead of its copy is the way a
        // command ring the game already refilled gets read as this frame's commands. Counts how
        // often the words parsed differ from the copy, by the reason the copy was skipped.
        static const bool live_read_check = std::getenv("EDEN_NX2_LIVE_READ_CHECK") != nullptr;
        const CommandHeader* live_read_copy = nullptr;
        LiveReadReason live_read_reason = LiveReadReason::Count;
        if (live_read_check && !submitted &&
            dma_pushbuffer_subindex < command_list.submitted_offsets.size() &&
            command_list.submitted_offsets[dma_pushbuffer_subindex] + size_t{header.size} <=
                command_list.submitted_words.size()) {
            live_read_copy = command_list.submitted_words.data() +
                             command_list.submitted_offsets[dma_pushbuffer_subindex];
            if (command_resume_offset != 0 && resnapshot == Resnapshot::Off) {
                live_read_reason = LiveReadReason::Resumed;
            } else if (flushed_parameters) {
                live_read_reason = LiveReadReason::FlushedParameters;
            } else if (!copy_current) {
                live_read_reason = CopyAtEveryAcquire()
                                       ? LiveReadReason::AcquireSinceCopy
                                   : (command_list.snapshot_suspensions & 1) != 0
                                       ? LiveReadReason::SuspendedCopy
                                       : LiveReadReason::SuspensionsChanged;
            } else if (IsGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32))) {
                live_read_reason = LiveReadReason::GpuWritten;
            } else {
                live_read_reason = LiveReadReason::SafeDirty;
            }
        }
        if (submitted) {
            const std::span<const CommandHeader> commands{
                command_list.submitted_words.data() +
                    command_list.submitted_offsets[dma_pushbuffer_subindex],
                header.size};
            RecordParsedEntry(header, commands, true, continues_split);
            Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Submitted);
            if (CopyAtEveryAcquire()) {
                snapshot_acquires = puller.Nx2AcquireCount();
            }
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
            snapshot_acquires.reset();
        } else if (use_safe) {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::SafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands, false, continues_split);
            if (live_read_copy != nullptr) {
                CheckLiveRead(live_read_reason, channel_state.bind_id, header, commands,
                              live_read_copy);
            }
            Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Safe);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (bool pending = false,
                   indirect_grid =
                       TakeIndirectGrid(dma_state.dma_get, size_t{header.size} * sizeof(u32));
                   !indirect_grid &&
                   (ReadGpuWrittenCommands(dma_state.dma_get, static_cast<u32>(header.size),
                                           &pending) ||
                    pending)) {
            if (pending) {
                // Parsed again once the GPU copied the list; other channels run meanwhile.
                return false;
            }
            const std::span<const CommandHeader> commands{command_headers.data(), header.size};
            RecordParsedEntry(header, commands, false, continues_split);
            if (live_read_copy != nullptr) {
                CheckLiveRead(live_read_reason, channel_state.bind_id, header, commands,
                              live_read_copy);
            }
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else {
            if (!indirect_grid) {
                FlushGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
            }
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::UnsafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands, false, continues_split);
            if (live_read_copy != nullptr) {
                CheckLiveRead(live_read_reason, channel_state.bind_id, header, commands,
                              live_read_copy);
            }
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        }
        if (puller.HasPendingAcquire()) {
            return false;
        }
        if (submitted && command_resume_offset < header.size) {
            return true; // left the copy at an acquire: the rest comes from memory or a new copy
        }
        command_resume_offset = 0;
        if (dma_state.method_count != 0) {
            split_method_end = header.addr + header.size * sizeof(u32);
            // A piece the method went on in elsewhere (a gather) doesn't move its own stream.
            if (!from_elsewhere) {
                split_stream_end = split_method_end;
            }
        }
    }

    if (++dma_pushbuffer_subindex >= command_list_size) {
        // A split method resumes within the submission that split it; ring addresses repeat
        // every frame, so a stale one could otherwise swallow the headers of a later entry.
        for (const auto& parked : parked_methods) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1) < 16) {
                LOG_WARNING(HW_GPU,
                            "Submission ended with parked method {:#x} (sub {}, {} words left, "
                            "resume {:#x})",
                            parked.method, parked.subchannel, parked.method_count,
                            parked.resume_address);
            }
            ParkedEngineMacro(parked, ParkedMacroAction::Drop);
        }
        parked_methods.clear();
        dma_pushbuffer.pop_front();
        dma_pushbuffer_subindex = 0;
    } else {
        signal_sync = command_list.command_lists[dma_pushbuffer_subindex].sync && Settings::values.sync_memory_operations.GetValue();
    }

    if (signal_sync) {
        auto signal = [this] {
            rasterizer->SignalFence([this]() {
                std::scoped_lock lk(sync_mutex);
                synced = true;
                sync_cv.notify_all();
            });
        };
        auto& split = VideoCommon::GpuSplit::Get();
        if (split.Active()) {
            split.Defer(signal);
            split.Drain(VideoCommon::GpuSplit::DrainSite::SyncFence);
        } else {
            signal();
        }
    }

    return true;
}

size_t DmaPusher::ProcessCommands(std::span<const CommandHeader> commands, size_t word_offset) {
    for (size_t index = 0; index < commands.size();) {
        // Data word of methods command
        if (dma_state.method_count && dma_state.non_incrementing) {
            auto const& command_header = commands[index]; //must ref (MUltiMethod re)
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            u32 max_write = u32(std::min<std::size_t>(index + dma_state.method_count, commands.size()) - index);
            if (g_nx2_ampere && dma_state.method < non_puller_methods) {
                max_write = 1; // a semaphore acquire may suspend the channel after any word
            }
            CallMultiMethod(&command_header.argument, max_write);
            dma_state.method_count -= max_write;
            dma_state.is_last_call = true;
            index += max_write;
        } else if (dma_state.method_count) {
            auto const command_header = commands[index]; //can copy
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            dma_state.is_last_call = dma_state.method_count <= 1;
            CallMethod(command_header.argument);
            dma_state.method += !dma_state.non_incrementing ? 1 : 0;
            dma_state.non_incrementing |= dma_increment_once;
            dma_state.method_count--;
            index++;
        } else {
            if (g_nx2_ampere && word_offset + index == 0) {
                if (const size_t skip = ResyncEntry(commands); skip != 0) {
                    index += skip;
                    continue;
                }
            }
            auto const command_header = commands[index]; //can copy
            if (g_nx2_ampere && command_header.method >= MethodAddressLimit &&
                command_header.mode != SubmissionMode::IncreasingOld &&
                command_header.mode != SubmissionMode::NonIncreasingOld && DropMisreadHeaders()) {
                DropMisreadEntry(commands, index, word_offset);
                return commands.size();
            }
            // No command active - this is the first word of a new one
            switch (command_header.mode) {
            case SubmissionMode::Increasing:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = false;
                break;
            case SubmissionMode::NonIncreasing:
                SetState(command_header);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::Inline:
                dma_state.method = command_header.method;
                dma_state.subchannel = command_header.subchannel;
                dma_state.dma_word_offset = u64(-s64(dma_state.dma_get)); // negate to set address as 0
                CallMethod(command_header.arg_count);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = true;
                break;
            default:
                break;
            }
            index++;
        }
        if (puller.HasPendingAcquire() ||
            (snapshot_acquires && puller.Nx2AcquireCount() != *snapshot_acquires)) {
            return index;
        }
    }
    return commands.size();
}

// NVN on Switch 2 ends GPFIFO entries in the middle of a method's data and calls another command
// buffer (e.g. a compute dispatch) before the entry that carries the rest, which starts right
// where the first one ended. An entry starting anywhere else is such a call: run it with a clean
// method state and resume the unfinished method when the stream continues at that address.
// Two kinds of entries elsewhere do carry the rest, so the method continues into them: one exactly
// as long as the words left (title 4 ends a main entry after a macro's first word and supplies the
// other five in a 5-word subroutine entry) and GPU-written macro arguments (an indirect draw's
// record). Bananza ended an entry 28 words into a 53-word compute macro and called a 20-word 3D
// stream; feeding that stream to the macro misread every header after it.
// Continue by default, unless the entry reads as a called stream (EntryCallsStream), except for
// the Yoshi demo, which runs the heuristic: continuing there ate the first header of a called 3D
// stream and read the rest from the wrong word (a 4140-word method on unbound subchannel 7, then a
// GPU hang). EDEN_NX2_SPLIT_METHODS=continue or =auto sets the mode for every title, and =park or
// EDEN_NX2_PARK_SPLIT_METHODS always parks.
// Returns whether the unfinished method goes on in this entry although it starts elsewhere.
bool DmaPusher::ResolveSplitMethod(const CommandListHeader& header) {
    enum class Mode { Auto, Continue, Park };
    static const std::optional<Mode> forced = []() -> std::optional<Mode> {
        if (std::getenv("EDEN_NX2_PARK_SPLIT_METHODS") != nullptr) {
            return Mode::Park;
        }
        const char* const value = std::getenv("EDEN_NX2_SPLIT_METHODS");
        if (value == nullptr) {
            return std::nullopt;
        }
        const std::string_view name{value};
        return name == "auto" ? Mode::Auto : name == "park" ? Mode::Park : Mode::Continue;
    }();
    const Mode mode = forced ? *forced
                      : Settings::GetCurrentProgramID() == Nx2YoshiDemoProgramId ? Mode::Auto
                                                                                 : Mode::Continue;
    const GPUVAddr entry_address = header.addr;
    const auto continued = std::ranges::find(parked_methods, entry_address,
                                             &ParkedMethod::resume_address);
    std::optional<ParkedMethod> resume;
    if (continued != parked_methods.end()) {
        resume = *continued;
        parked_methods.erase(continued);
    }
    bool continues_elsewhere = false;
    if (dma_state.method_count != 0 && entry_address != split_method_end) {
        const u64 entry_words = header.size;
        const bool carries_rest =
            mode == Mode::Continue
                ? !EntryCallsStream(header)
                : mode == Mode::Auto &&
                      (entry_words == dma_state.method_count ||
                       IsGpuWrittenCommands(entry_address, entry_words * sizeof(u32)));
        static std::atomic<u32> continued_splits{};
        static std::atomic<u32> parked_splits{};
        const u32 count = (carries_rest ? continued_splits : parked_splits).fetch_add(1) + 1;
        if (count <= 16 || (count % 4096) == 0) {
            LOG_INFO(HW_GPU,
                     "Split method {:#x} (sub {}, {} words left) {} entry {:#x} (size {}, level "
                     "bit {}); the split entry ended at {:#x} (#{})",
                     dma_state.method, dma_state.subchannel, dma_state.method_count,
                     carries_rest ? "continues in" : "parked across", entry_address, entry_words,
                     u64{header.is_push_buffer}, split_method_end, count);
        }
        continues_elsewhere = carries_rest;
        if (!carries_rest) {
            if (parked_methods.size() == MaxParkedMethods) {
                static std::atomic<u32> reported{};
                if (reported.fetch_add(1) < 8) {
                    const auto& oldest = parked_methods.front();
                    LOG_WARNING(HW_GPU,
                                "Dropping parked method {:#x} (sub {}, {} words left, resume {:#x})",
                                oldest.method, oldest.subchannel, oldest.method_count,
                                oldest.resume_address);
                }
                ParkedEngineMacro(parked_methods.front(), ParkedMacroAction::Drop);
                parked_methods.erase(parked_methods.begin());
            }
            Engines::EngineInterface* const macro_engine =
                ParkEngineMacro(dma_state.subchannel, dma_state.method, split_method_end);
            parked_methods.push_back(ParkedMethod{
                .method = dma_state.method,
                .subchannel = dma_state.subchannel,
                .method_count = dma_state.method_count,
                .non_incrementing = dma_state.non_incrementing,
                .increment_once = dma_increment_once,
                .resume_address = split_method_end,
                .macro_engine = macro_engine,
                .macro_engine_type = subchannel_type[dma_state.subchannel],
            });
            dma_state.method_count = 0;
            dma_increment_once = false;
        }
    }
    if (resume && dma_state.method_count == 0) {
        dma_state.method = resume->method;
        dma_state.subchannel = resume->subchannel;
        dma_state.method_count = resume->method_count;
        dma_state.non_incrementing = resume->non_incrementing;
        dma_increment_once = resume->increment_once;
        ParkedEngineMacro(*resume, ParkedMacroAction::Resume);
    } else if (resume) {
        ParkedEngineMacro(*resume, ParkedMacroAction::Drop);
    }
    return continues_elsewhere;
}

// Whether an entry a split method would go on in is a called stream instead: read from its first
// word it holds only well-formed commands, read after the words the method has left it doesn't.
// Ocarina of Time and Bananza on an RX 570 (GitLab #7, #8) continued a 2- and a 1-word method
// into an entry starting with a macro call; the word after them read as a 6000-word run over the
// host methods, every later header was lost, and the game crashed or hung.
bool DmaPusher::EntryCallsStream(const CommandListHeader& header) {
    if (dma_state.method_count >= header.size) {
        return false;
    }
    std::array<u32, 64> words{};
    const size_t count = std::min<size_t>(header.size, words.size());
    memory_manager.ReadBlockUnsafe(header.addr, words.data(), count * sizeof(u32));
    const std::span<const u32> entry{words.data(), count};
    return ParsesCleanly(entry, 0) && !ParsesCleanly(entry, dma_state.method_count);
}

// Whether a word read as a command header could start a command of a real stream: no incrementing
// run past the host methods' registers, and no engine method on a subchannel nothing bound. A
// BindObject marks its subchannel bound in `bound`.
bool DmaPusher::PlausibleHeader(CommandHeader command, u32& bound) const {
    const SubmissionMode mode = command.mode;
    if (mode != SubmissionMode::Increasing && mode != SubmissionMode::NonIncreasing &&
        mode != SubmissionMode::IncreaseOnce) {
        return true;
    }
    const u32 method = command.method;
    const u32 count = command.method_count;
    if (method >= MethodAddressLimit && DropMisreadHeaders()) {
        return false;
    }
    if (method >= non_puller_methods) {
        return (bound & (1U << command.subchannel)) != 0;
    }
    const u32 registers = mode == SubmissionMode::NonIncreasing ? 1U
                          : mode == SubmissionMode::IncreaseOnce ? std::min(count, 2U)
                                                                 : count;
    if (method + registers > non_puller_methods) {
        return false;
    }
    if (method == 0 && count != 0) {
        bound |= 1U << command.subchannel;
    }
    return true;
}

u32 DmaPusher::BoundSubchannels() const {
    u32 bound = 0;
    for (u32 index = 0; index < max_subchannels; ++index) {
        bound |= subchannels[index] != nullptr ? 1U << index : 0U;
    }
    return bound;
}

// Whether the words from `start` parse as commands a real stream sends (PlausibleHeader).
bool DmaPusher::ParsesCleanly(std::span<const u32> words, size_t start) const {
    u32 bound = BoundSubchannels();
    for (size_t index = start; index < words.size();) {
        CommandHeader command;
        command.argument = words[index++];
        if (!PlausibleHeader(command, bound)) {
            return false;
        }
        const SubmissionMode mode = command.mode;
        if (mode == SubmissionMode::Increasing || mode == SubmissionMode::NonIncreasing ||
            mode == SubmissionMode::IncreaseOnce) {
            index += command.method_count;
        }
    }
    return true;
}

// An entry that starts while no method is unfinished, with a first word no stream starts with, holds
// the rest of a split method whose state was lost on the way. OoT froze collecting a blue rupee on
// an RTX 3050: an entry began with the last two arguments of a macro call, read as a 5500-word
// method on unbound subchannel 5, and the GPU hung. Skips at most 4 words, to the first offset the
// rest of the entry parses cleanly from (the macro call loses those arguments); 0 to go on as is.
size_t DmaPusher::ResyncEntry(std::span<const CommandHeader> commands) {
    u32 bound = BoundSubchannels();
    if (commands.empty() || PlausibleHeader(commands.front(), bound)) {
        return 0;
    }
    const std::span<const u32> words{reinterpret_cast<const u32*>(commands.data()),
                                     (std::min<size_t>)(commands.size(), 64)};
    for (size_t skip = 1; skip <= 4 && skip < words.size(); ++skip) {
        if (!ParsesCleanly(words, skip)) {
            continue;
        }
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1) < 16) {
            LOG_WARNING(HW_GPU,
                        "Entry at {:#x} on channel {} starts with {} words no command starts with "
                        "({:08x}): skipped them",
                        dma_state.dma_get, channel_state.bind_id, skip, words.front());
        }
        return skip;
    }
    return 0;
}

// A parked macro method has sent part of its call's parameters to the engine already. The command
// buffer the split entry calls often makes macro calls on the same engine (the Yoshi demo ends a
// 3D entry two words into a three-word macro call and calls a stream that starts with another
// one), which would join that call: the engine sets the call aside until the method resumes.
Engines::EngineInterface* DmaPusher::ParkEngineMacro(u32 subchannel, u32 method, GPUVAddr key) {
    Engines::EngineInterface* const engine = subchannels[subchannel];
    if (engine == nullptr || method < MacroRegistersStart) {
        return nullptr;
    }
    switch (subchannel_type[subchannel]) {
    case Engines::EngineTypes::Maxwell3D:
        static_cast<Engines::Maxwell3D*>(engine)->ParkMacro(system, key);
        return engine;
    case Engines::EngineTypes::KeplerCompute:
        static_cast<Engines::KeplerCompute*>(engine)->ParkMacro(key);
        return engine;
    default:
        return nullptr;
    }
}

void DmaPusher::ParkedEngineMacro(const ParkedMethod& parked, ParkedMacroAction action) {
    if (parked.macro_engine == nullptr) {
        return;
    }
    const bool resume = action == ParkedMacroAction::Resume;
    switch (parked.macro_engine_type) {
    case Engines::EngineTypes::Maxwell3D: {
        auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(parked.macro_engine);
        if (resume) {
            maxwell3d->ResumeMacro(system, parked.resume_address);
        } else {
            maxwell3d->DropParkedMacro(system, parked.resume_address);
        }
        break;
    }
    case Engines::EngineTypes::KeplerCompute: {
        auto* const compute = static_cast<Engines::KeplerCompute*>(parked.macro_engine);
        if (resume) {
            compute->ResumeMacro(parked.resume_address);
        } else {
            compute->DropParkedMacro(parked.resume_address);
        }
        break;
    }
    default:
        break;
    }
}

void DmaPusher::SetState(const CommandHeader& command_header) {
    dma_state.method = command_header.method;
    dma_state.subchannel = command_header.subchannel;
    dma_state.method_count = command_header.method_count;
}

// EDEN_NX2_PB_CHECK also keeps the last parsed entries, for DumpCurrentEntry, and counts where
// their words came from. Read on first use: nx2_switches.txt is applied in main(), after static
// initialisation.
static bool RecordParsedEntries() {
    static const bool record = std::getenv("EDEN_NX2_PB_CHECK") != nullptr;
    return record;
}
static std::atomic<u64> g_parsed_from_snapshot{};
static std::atomic<u64> g_parsed_from_memory{};

// Logs where the misread header came from: the words parsed against guest memory now. Equal words
// mean memory held them when the entry was read; different ones, that the entry was read from a
// copy memory no longer matches, or before the game finished writing it.
void DmaPusher::DropMisreadEntry(std::span<const CommandHeader> commands, size_t index,
                                 size_t word_offset) {
    static std::atomic<u32> reported{};
    const u32 count = reported.fetch_add(1) + 1;
    if (count > 16 && (count % 1024) != 0) {
        return;
    }
    std::array<u32, 4> parsed{};
    std::array<u32, 4> memory{};
    const size_t words = (std::min)(parsed.size(), commands.size() - index);
    for (size_t i = 0; i < words; ++i) {
        parsed[i] = commands[index + i].argument;
    }
    const GPUVAddr address = dma_state.dma_get + (word_offset + index) * sizeof(u32);
    memory_manager.ReadBlockUnsafe(address, memory.data(), words * sizeof(u32));
    LOG_WARNING(HW_GPU,
                "Entry at {:#x} on channel {}, word {}: header {:08x} names method {:#x}, which no "
                "class has; dropped the {} words left in the entry. Parsed {:08x}, memory now "
                "{:08x} ({}) (#{})",
                dma_state.dma_get, channel_state.bind_id, word_offset + index,
                commands[index].argument, commands[index].method.Value(), commands.size() - index,
                fmt::join(std::span{parsed.data(), words}, " "),
                fmt::join(std::span{memory.data(), words}, " "),
                parsed == memory ? "the same" : "different", count);
    if (count == 1 && RecordParsedEntries()) {
        DumpCurrentEntry("misread header");
    }
}

void DmaPusher::WatchArgument(u32 method, u32 argument) const {
    const u32 watch = semaphore_watch;
    if ((argument & ~0xfU) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} subchannel {} method {:#x} argument {:#x}",
                 channel_state.bind_id, dma_state.subchannel, method, argument);
    }
}

bool DmaPusher::WantsSegment() const {
    // Only macro parameters and compute uploads read the engine's current_dma_segment.
    const auto engine = subchannel_type[dma_state.subchannel];
    return engine == Engines::EngineTypes::KeplerCompute ||
           (engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart);
}

void DmaPusher::CallMethod(u32 argument) {
    if (semaphore_watch != 0) [[unlikely]] {
        WatchArgument(dma_state.method, argument);
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (dma_state.method < non_puller_methods) {
        puller.CallPullerMethod(*this, Engines::Puller::MethodCall{
            dma_state.method,
            argument,
            dma_state.subchannel,
            dma_state.method_count,
        });
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            ReportUnboundSubchannel();
            return;
        }
        const bool writer = split.Active() &&
                            WritesMemory(subchannel, subchannel_type[dma_state.subchannel],
                                         dma_state.method, 1);
        const bool draw_writer =
            split.Active() && DrawMayWriteLists(subchannel_type[dma_state.subchannel]);
        SCOPE_EXIT {
            if (writer) {
                split.NoteWriter();
            }
            if (draw_writer) {
                split.NoteDrawWriter();
            }
        };
        if (split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMethod(system, split, dma_state.method, argument,
                                                   dma_state.is_last_call);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMethod(system, split, dma_state.method, argument,
                                                 dma_state.is_last_call, &segment);
                        return;
                    }
                }
            }
            split.Method(subchannel, dma_state.method, argument, dma_state.is_last_call,
                         WantsSegment() ? &segment : nullptr);
            return;
        }
        if (!subchannel->execution_mask[dma_state.method]) {
            subchannel->method_sink.emplace_back(dma_state.method, argument);
        } else {
            VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
            subchannel->ConsumeSink(system);
            subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
            subchannel->CallMethod(system, dma_state.method, argument, dma_state.is_last_call);
        }
    }
}

void DmaPusher::CallMultiMethod(const u32* base_start, u32 num_methods) {
    if (semaphore_watch != 0) [[unlikely]] {
        for (u32 i = 0; i < num_methods; ++i) {
            WatchArgument(dma_state.method + (dma_state.non_incrementing ? 0 : i), base_start[i]);
        }
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (dma_state.method < non_puller_methods) {
        puller.CallMultiMethod(*this, dma_state.method, dma_state.subchannel, base_start, num_methods, dma_state.method_count);
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            ReportUnboundSubchannel();
            return;
        }
        const bool writer = split.Active() &&
                            WritesMemory(subchannel, subchannel_type[dma_state.subchannel],
                                         dma_state.method, dma_state.non_incrementing ? 1 : num_methods);
        const bool draw_writer =
            split.Active() && DrawMayWriteLists(subchannel_type[dma_state.subchannel]);
        SCOPE_EXIT {
            if (writer) {
                split.NoteWriter();
            }
            if (draw_writer) {
                split.NoteDrawWriter();
            }
        };
        if (split.Active()) {
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                        num_methods, dma_state.method_count);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                      num_methods, dma_state.method_count, &segment);
                        return;
                    }
                }
            }
            split.MultiMethod(subchannel, dma_state.method, base_start, num_methods,
                              dma_state.method_count, WantsSegment() ? &segment : nullptr);
            return;
        }
        VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
        subchannel->ConsumeSink(system);
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
        subchannel->CallMultiMethod(system, dma_state.method, base_start, num_methods, dma_state.method_count);
    }
}

void DmaPusher::RefreshSnapshot(CommandList& list, size_t first_entry, size_t first_word,
                                bool current, const char* reason) {
    const size_t entries = (std::min)(list.command_lists.size(), list.submitted_offsets.size());
    if (first_entry >= entries) {
        return;
    }
    // Read through a GMMU update still queued for the GPU thread, the words would come from the
    // old mapping, and an unmapped page (the game unmapped a list still queued) reads zeros. The
    // current list then reads memory as it is parsed, as it would without the switch; a queued
    // list keeps the words it has, copied at submit with no update queued.
    for (size_t i = first_entry; i < entries; ++i) {
        const CommandListHeader& entry = list.command_lists[i];
        if (memory_manager.IsUpdateQueued(entry.addr, entry.size * sizeof(u32))) {
            if (current) {
                list.submitted_offsets.clear();
            }
            return;
        }
    }
    const u64 old_count = list.snapshot_suspensions;
    list.snapshot_suspensions = suspensions.load(std::memory_order_relaxed);
    const bool trace = SnapshotTraceEnabled();
    u32 changed_entries = 0;
    u64 changed_words = 0;
    size_t first_changed = list.command_lists.size();
    for (size_t i = first_entry; i < entries; ++i) {
        const CommandListHeader& entry = list.command_lists[i];
        const size_t skip = i == first_entry ? (std::min)(first_word, size_t{entry.size}) : 0;
        const size_t offset = list.submitted_offsets[i] + skip;
        const size_t words = size_t{entry.size} - skip;
        if (words == 0 || offset + words > list.submitted_words.size()) {
            continue;
        }
        const GPUVAddr address = entry.addr + skip * sizeof(u32);
        if (!CommandsMapped(address, words * sizeof(u32))) {
            if (current) {
                list.submitted_offsets.clear();
                return;
            }
            continue;
        }
        CommandHeader* const destination = list.submitted_words.data() + offset;
        if (!trace) {
            memory_manager.ReadBlockUnsafe(address, destination, words * sizeof(u32));
            continue;
        }
        recopy_scratch.resize(words);
        memory_manager.ReadBlockUnsafe(address, recopy_scratch.data(), words * sizeof(u32));
        if (const u32 changed =
                ChangedWords(std::span<const CommandHeader>{destination, words}, recopy_scratch);
            changed != 0) {
            ++changed_entries;
            changed_words += changed;
            first_changed = (std::min)(first_changed, i);
            std::memcpy(destination, recopy_scratch.data(), words * sizeof(u32));
        }
    }
    if (trace) {
        NoteRecopy(list, reason, old_count, list.snapshot_suspensions, changed_entries,
                   changed_words, first_changed);
    }
}

void DmaPusher::NoteRecopy(const CommandList& list, const char* reason, u64 old_count, u64 now,
                           u32 changed_entries, u64 changed_words, size_t first_changed) {
    const auto time = std::chrono::steady_clock::now();
    const float age_ms =
        list.submitted_at == std::chrono::steady_clock::time_point{}
            ? -1.0f
            : std::chrono::duration<float, std::milli>(time - list.submitted_at).count();
    // Per 10 s window (GPU thread): how many copies were taken again, how late, how many changed,
    // and up to 20 of those that changed in full, so a hang late in a session still gets some.
    struct Stats {
        u64 copies;
        u64 changed;
        u64 changed_entries;
        float max_age_ms;
        float max_changed_age_ms;
        u32 logged;
        std::chrono::steady_clock::time_point since;
    };
    thread_local Stats stats{};
    if (stats.since == std::chrono::steady_clock::time_point{}) {
        stats.since = time;
    }
    if (changed_entries != 0 && stats.logged < 20) {
        ++stats.logged;
        LOG_INFO(HW_GPU,
                 "Channel {} copied a command list again {}, {:.1f} ms after it was submitted "
                 "(suspension count {} -> {}): {} of {} entries ({} words) changed, the first "
                 "entry {} at {:#x}",
                 channel_state.bind_id, reason, age_ms, old_count, now, changed_entries,
                 list.command_lists.size(), changed_words, first_changed,
                 u64{list.command_lists[first_changed].addr});
    }
    ++stats.copies;
    stats.max_age_ms = (std::max)(stats.max_age_ms, age_ms);
    if (changed_entries != 0) {
        ++stats.changed;
        stats.changed_entries += changed_entries;
        stats.max_changed_age_ms = (std::max)(stats.max_changed_age_ms, age_ms);
    }
    const float seconds = std::chrono::duration<float>(time - stats.since).count();
    if (seconds >= 10.0f) {
        LOG_INFO(HW_GPU,
                 "Command lists copied again in {:.0f} s: {} (latest {:.1f} ms after submit), {} "
                 "changed {} entries (latest {:.1f} ms after submit)",
                 seconds, stats.copies, stats.max_age_ms, stats.changed, stats.changed_entries,
                 stats.max_changed_age_ms);
        stats = Stats{};
        stats.since = time;
    }
}

bool DmaPusher::CommandsMapped(GPUVAddr address, size_t size) const {
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        if (!memory_manager.GpuToCpuAddress(page)) {
            return false;
        }
    }
    return true;
}

void DmaPusher::CopySubmittedWords(CommandList& entries) {
    size_t words = 0;
    for (const auto& entry : entries.command_lists) {
        words += entry.size;
    }
    // Under the lock a resume copies the lists in transit again with: this copy is either after
    // the resume or in in_transit for it.
    const std::scoped_lock lock{in_transit_mutex};
    // Read before the copy: a suspension or resume during it makes the GPU thread copy again
    // (EDEN_NX2_PB_RESNAPSHOT, Push).
    entries.snapshot_suspensions = suspensions.load(std::memory_order_acquire);
    entries.submitted_words.resize(words);
    entries.submitted_offsets.reserve(entries.command_lists.size());
    size_t offset = 0;
    for (const auto& entry : entries.command_lists) {
        entries.submitted_offsets.push_back(static_cast<u32>(offset));
        memory_manager.ReadBlockUnsafe(entry.addr, entries.submitted_words.data() + offset,
                                       entry.size * sizeof(u32));
        offset += entry.size;
    }
    if (SnapshotTraceEnabled()) {
        entries.submitted_at = std::chrono::steady_clock::now();
    }
    if (!InTransitCopies()) {
        return;
    }
    entries.submit_sequence = ++last_submit_sequence;
    in_transit.push_back(InTransitList{
        .submit_sequence = entries.submit_sequence,
        .entries = std::vector<CommandListHeader>(entries.command_lists.begin(),
                                                  entries.command_lists.end()),
        .offsets = entries.submitted_offsets,
        .words = words,
    });
}

void DmaPusher::CopyInTransitAgain(u64 now, bool reread) {
    const std::scoped_lock lock{in_transit_mutex};
    for (InTransitList& list : in_transit) {
        if (!reread) {
            list.current = true;
            list.suspensions = now;
            continue;
        }
        // As in RefreshSnapshot: through a GMMU update still queued, or from an unmapped page,
        // the words would not be the list's. Such a list keeps the copy it has, and Push copies
        // it again as before.
        const bool readable =
            std::ranges::all_of(list.entries, [this](const CommandListHeader& entry) {
                const size_t bytes = size_t{entry.size} * sizeof(u32);
                return !memory_manager.IsUpdateQueued(entry.addr, bytes) &&
                       CommandsMapped(entry.addr, bytes);
            });
        if (!readable) {
            list.copied = false;
            list.current = false;
            continue;
        }
        list.newer.resize(list.words);
        for (size_t i = 0; i < list.entries.size(); ++i) {
            const CommandListHeader& entry = list.entries[i];
            memory_manager.ReadBlockUnsafe(entry.addr, list.newer.data() + list.offsets[i],
                                           size_t{entry.size} * sizeof(u32));
        }
        list.copied = true;
        list.current = true;
        list.suspensions = now;
    }
}

void DmaPusher::TakeInTransitCopy(CommandList& list) {
    InTransitList taken;
    {
        const std::scoped_lock lock{in_transit_mutex};
        // Older lists never come (nvhost-gpu failed their submit after the copy).
        while (!in_transit.empty() && in_transit.front().submit_sequence < list.submit_sequence) {
            in_transit.pop_front();
        }
        if (in_transit.empty() || in_transit.front().submit_sequence != list.submit_sequence) {
            return;
        }
        taken = std::move(in_transit.front());
        in_transit.pop_front();
    }
    if (!taken.current || (taken.copied && taken.newer.size() != list.submitted_words.size())) {
        return;
    }
    if (taken.copied) {
        if (SnapshotTraceEnabled()) {
            u32 changed_entries = 0;
            u64 changed_words = 0;
            size_t first_changed = list.command_lists.size();
            for (size_t i = 0; i < list.command_lists.size() && i < list.submitted_offsets.size();
                 ++i) {
                const size_t offset = list.submitted_offsets[i];
                const size_t size = list.command_lists[i].size;
                if (offset + size > list.submitted_words.size()) {
                    continue;
                }
                const u32 changed = ChangedWords(
                    std::span<const CommandHeader>{list.submitted_words.data() + offset, size},
                    std::span<const CommandHeader>{taken.newer.data() + offset, size});
                if (changed != 0) {
                    ++changed_entries;
                    changed_words += changed;
                    first_changed = (std::min)(first_changed, i);
                }
            }
            NoteRecopy(list, "on its way, at a resume", list.snapshot_suspensions,
                       taken.suspensions, changed_entries, changed_words, first_changed);
        }
        list.submitted_words.swap(taken.newer);
    }
    list.snapshot_suspensions = taken.suspensions;
}

bool DmaPusher::IsGpuWrittenCommands(GPUVAddr address, size_t size) const {
    // Title 1 runs small command lists its culling dispatches fill in each frame. Accurate GPU
    // mode reads every command list through the caches (safe reads); here only the ones on pages
    // the GPU has written are, or their draws run with an older frame's words (trees vanish).
    // EDEN_NX2_GPU_WRITTEN_CMDLISTS=0 reads them like any other (faster, trees may vanish).
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_GPU_WRITTEN_CMDLISTS");
        return value == nullptr || value[0] != '0';
    }();
    if (!enabled || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || size == 0) {
        return false;
    }
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsGpuWritten(*device)) {
            return true;
        }
    }
    return false;
}

bool DmaPusher::ReadGpuWrittenCommands(GPUVAddr address, u32 count, bool* pending) {
    // The pages are marked as command list pages: from the next write on, the buffer cache copies
    // them to host memory right after the dispatch or copy that writes them and submits. The
    // front then waits only for the back to run that work and for the GPU to finish the copy, not
    // for everything queued and a cache flush (FlushGpuWrittenCommands: ~400 GPU round trips a
    // second in a Mario Kart World race, during which the back had nothing to do). Pages
    // FlushGpuWrittenCommands flushed whole and no GPU work wrote since are read from guest
    // memory.
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || count == 0 ||
        rasterizer == nullptr) {
        return false;
    }
    const GPUVAddr end = address + u64{count} * sizeof(u32);
    bool written = false;
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < end; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsGpuWritten(*device)) {
            Tegra::Nx2MarkCommandListPage(*device);
            written = true;
        }
    }
    if (!written) {
        return false;
    }
    // Resuming after the copy's tick: the back ran the list's writer before the suspension
    // (this wait ran then), and waiting again would wait for the other channels' work queued
    // meanwhile too.
    const bool resumed = std::exchange(resumed_list_read, false);
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active() && !resumed) {
        split.WaitForWriters(true);
    }
    using GpuWrittenList = VideoCore::RasterizerInterface::GpuWrittenList;
    command_headers.resize_destructive(count);
    u8* const out = reinterpret_cast<u8*>(command_headers.data());
    for (GPUVAddr chunk = address; chunk < end;) {
        const GPUVAddr chunk_end = (std::min)(end, (chunk & ~GPUVAddr{0xfff}) + 0x1000);
        const std::span<u8> part{out + (chunk - address), static_cast<size_t>(chunk_end - chunk)};
        const auto device = memory_manager.GpuToCpuAddress(chunk);
        if (!device) {
            return false;
        }
        GpuWrittenList where = GpuWrittenList::Synced;
        if (Tegra::Nx2IsGpuWritten(*device)) {
            u64 tick = 0;
            where = rasterizer->ReadGpuWrittenCommandList(*device, part,
                                                          pending != nullptr ? &tick : nullptr);
            if (where == GpuWrittenList::Pending) {
                // The copy is still on the GPU: the channel waits for it without blocking the
                // others (Step parses this entry again once IsGpuTickDone).
                pending_list_tick = tick;
                *pending = true;
                return false;
            }
        }
        if (where == GpuWrittenList::Unknown) {
            return false;
        }
        if (where == GpuWrittenList::Synced) {
            memory_manager.ReadBlockUnsafe(chunk, part.data(), part.size());
        }
        chunk = chunk_end;
    }
    return true;
}

void DmaPusher::FlushGpuWrittenCommands(GPUVAddr address, size_t size) {
    // Title 1 runs small command lists its culling dispatches fill in each frame. Accurate GPU
    // mode reads every command list through the caches (safe reads); here only the ones on pages
    // the GPU has written are, or their draws run with an older frame's words (trees vanish).
    // These are the lists ReadGpuWrittenCommands can't take from a copy or guest memory (a page's
    // first list, or pages draws write too).
    if (!IsGpuWrittenCommands(address, size)) {
        Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Memory);
        return;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    // The entry is the rest of a macro call's arguments: NVN's indirect draw macro draws them
    // from GPU memory instead (Maxwell3D::FrontTakeIndirectArguments).
    if (command_resume_offset == 0 && dma_state.method_count != 0 &&
        size == size_t{dma_state.method_count} * sizeof(u32) && split.Active() &&
        split.FrontMacros() && subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D) {
        auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannels[dma_state.subchannel]);
        if (maxwell3d->GetNativeMacroEngine().native_turing &&
            maxwell3d->FrontTakeIndirectArguments(system, split, dma_state.method, address,
                                                  dma_state.method_count)) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::IndirectEntries, 1);
            Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Indirect);
            return;
        }
    }
    Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Slow);
    const bool diag = Nx2CmdPageDiag::g_enabled;
    const auto diag_start =
        diag ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::vector<u32> diag_before;
    if (diag) {
        diag_before.resize(size / sizeof(u32));
        memory_manager.ReadBlockUnsafe(address, diag_before.data(),
                                       diag_before.size() * sizeof(u32));
    }
    // The caches belong to the split's back: it first runs what the front queued (the dispatch
    // that writes these words among it).
    // The whole pages are flushed: their next lists read guest memory until GPU work writes them
    // again.
    bool flushed = false;
    const GPUVAddr first_page = address & ~GPUVAddr{0xfff};
    const GPUVAddr end_page = (address + size + 0xfff) & ~GPUVAddr{0xfff};
    auto flush = [this, address = first_page, size = static_cast<size_t>(end_page - first_page),
                  flushed_out = &flushed] {
        if (!memory_manager.IsMemoryDirty(address, size, VideoCommon::CacheType::BufferCache)) {
            return;
        }
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::CommandListFlushes, 1);
        *flushed_out = true;
        // EDEN_NX2_CMD_BATCH=1: each flush waits for the GPU, and most GPU-written entries are
        // written together before the first is read. The other entries that needed a flush
        // before and are dirty again come along in the same wait. The front waits for this
        // (Drain), so the back may use flushed_command_entries.
        static Common::Nx2::SessionValue<bool> batch{[] {
            return Common::Nx2::Setting("EDEN_NX2_CMD_BATCH", "0").starts_with('1');
        }};
        if (!batch.Get() || rasterizer == nullptr) {
            memory_manager.FlushRegion(address, size, VideoCommon::CacheType::BufferCache);
            return;
        }
        if (flushed_command_entries.size() < 1024) {
            flushed_command_entries.emplace(address, size);
        }
        rasterizer->BeginFlushBatch();
        memory_manager.FlushRegion(address, size, VideoCommon::CacheType::BufferCache);
        for (const auto& [other_address, other_size] : flushed_command_entries) {
            if (other_address != address &&
                memory_manager.IsMemoryDirty(other_address, other_size,
                                             VideoCommon::CacheType::BufferCache)) {
                memory_manager.FlushRegion(other_address, other_size,
                                           VideoCommon::CacheType::BufferCache);
            }
        }
        rasterizer->EndFlushBatch();
    };
    if (split.Active()) {
        split.Defer(flush);
        split.Drain(VideoCommon::GpuSplit::DrainSite::Commands);
    } else {
        flush();
    }
    if (rasterizer != nullptr) {
        for (GPUVAddr page = first_page; page < end_page; page += 0x1000) {
            if (const auto device = memory_manager.GpuToCpuAddress(page);
                device && Tegra::Nx2IsCommandListPage(*device)) {
                rasterizer->NoteGpuWrittenCommandListSynced(*device);
            }
        }
    }
    if (diag) {
        RecordGpuWrittenCommandsDiag(address, size, flushed, diag_before, diag_start);
    }
}

// EDEN_NX2_INDIRECT_DISPATCH: the entry is the [x, y, z] of NVN's grid size macro, written by the
// GPU. The dispatch reads them from GPU memory (KeplerCompute::FrontTakeIndirectArguments), so the
// entry is parsed as it is in guest memory, without waiting for a copy of the list or flushing it.
bool DmaPusher::TakeIndirectGrid(GPUVAddr address, size_t size) {
    if (command_resume_offset != 0 || dma_state.method_count == 0 ||
        size != size_t{dma_state.method_count} * sizeof(u32) ||
        subchannel_type[dma_state.subchannel] != Engines::EngineTypes::KeplerCompute) {
        return false;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannels[dma_state.subchannel]);
    if (!split.Active() || !split.FrontMacros() || !compute->FrontRunsMacros() ||
        !IsGpuWrittenCommands(address, size) ||
        !compute->FrontTakeIndirectArguments(split, dma_state.method, address,
                                             dma_state.method_count)) {
        return false;
    }
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::IndirectEntries, 1);
    Nx2CmdPageDiag::CountEntry(Nx2CmdPageDiag::Path::Indirect);
    return true;
}

void DmaPusher::RecordGpuWrittenCommandsDiag(GPUVAddr address, size_t size, bool flushed,
                                             std::span<const u32> before,
                                             std::chrono::steady_clock::time_point start) {
    const GPUVAddr first_page = address & ~GPUVAddr{0xfff};
    const GPUVAddr end_page = (address + size + 0xfff) & ~GPUVAddr{0xfff};
    auto kind = Nx2CmdPageDiag::Kind::NoWrite;
    Nx2CmdPageDiag::PageView first_view;
    u64 first_device{};
    for (GPUVAddr page = first_page; page < end_page; page += 0x1000) {
        const auto device = memory_manager.GpuToCpuAddress(page);
        if (!device) {
            continue;
        }
        const u32 begin = static_cast<u32>((std::max)(address, page) - page);
        const u32 end = static_cast<u32>((std::min)(address + size, page + 0x1000) - page);
        const auto view = Nx2CmdPageDiag::TakePage(*device, begin, end);
        if (page == first_page) {
            first_view = view;
            first_device = *device;
        }
        kind = (std::max)(kind, view.kind);
    }
    std::vector<u32> after(before.size());
    memory_manager.ReadBlockUnsafe(address, after.data(), after.size() * sizeof(u32));
    u64 changed{};
    for (size_t i = 0; i < after.size(); ++i) {
        changed += after[i] != before[i] ? 1 : 0;
    }
    const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
    Nx2CmdPageDiag::RecordSlow(
        kind, changed, flushed, ns, address, first_device,
        static_cast<u32>(address - first_page),
        static_cast<u32>((std::min)(address + size, first_page + 0x1000) - first_page),
        first_view);
}

void DmaPusher::ReportUnboundSubchannel() {
    // Methods sent to a subchannel with no engine (no SetObject yet) are dropped.
    static std::atomic<u32> reported{};
    if (reported.fetch_add(1) < 8) {
        LOG_WARNING(HW_GPU,
                    "Dropping method {:#x} on unbound subchannel {} (channel {}, entry {} at "
                    "{:#x})",
                    dma_state.method, dma_state.subchannel, channel_state.bind_id,
                    dma_pushbuffer_subindex, dma_state.dma_get);
    }
    // Usually misread command words (Bananza's first cave: a burst here, then a BindObject of
    // class 0x0f39): dump the entries that led there, once.
    if (static bool dumped = false; g_nx2_ampere && !dumped) {
        dumped = true;
        DumpCurrentEntry("unbound subchannel");
    }
}

void DmaPusher::CheckSubmitHash(const CommandListHeader& header, u64 submit_hash,
                                bool after_acquire) {
    std::vector<u32> words(header.size);
    memory_manager.ReadBlockUnsafe(header.addr, words.data(), words.size() * sizeof(u32));
    static std::atomic<u64> checked{};
    static std::atomic<u64> changed{};
    static std::atomic<u64> changed_after_acquire{};
    ++checked;
    if (HashPushbufferWords(words) != submit_hash) {
        if (after_acquire) {
            ++changed_after_acquire;
        }
        if (changed.fetch_add(1) < 16) {
            LOG_WARNING(HW_GPU, "pushbuffer entry {:#x}+{:#x} changed between submit and execution",
                        header.addr.Value(), header.size.Value());
        }
    }
    if ((checked % 20000) == 0) {
        LOG_WARNING(HW_GPU,
                    "pushbuffer check: {} of {} entries changed after submit, {} of them after a "
                    "semaphore acquire of their command list; {} parsed from their submit copy, "
                    "{} from memory",
                    changed.load(), checked.load(), changed_after_acquire.load(),
                    g_parsed_from_snapshot.load(), g_parsed_from_memory.load());
    }
}

void DmaPusher::RecordParsedEntry(const CommandListHeader& header,
                                  std::span<const CommandHeader> commands, bool from_snapshot,
                                  bool continues_split) {
    if (!RecordParsedEntries()) {
        return;
    }
    (from_snapshot ? g_parsed_from_snapshot : g_parsed_from_memory)
        .fetch_add(1, std::memory_order_relaxed);
    u64 hash = 0xcbf29ce484222325ULL;
    for (const CommandHeader& command : commands) {
        hash = (hash ^ command.argument) * 0x100000001b3ULL;
    }
    // A method can go on in an entry that starts elsewhere (Mario Kart World's 0xe59 gathers),
    // and seems to when command words are misread: the words it goes on with tell them apart.
    // Entries that carry its rest (as long as the words left, or GPU-written records, see
    // ResolveSplitMethod) are not counted.
    const bool unexpected =
        continues_split && u64{header.size} != dma_state.method_count &&
        !IsGpuWrittenCommands(header.addr, size_t{header.size} * sizeof(u32));
    ParsedEntry& entry = parsed_entries[parsed_entry_count++ % parsed_entries.size()];
    entry = {header.addr, static_cast<u32>(header.size), hash, from_snapshot, unexpected, {}};
    if (!unexpected) {
        return;
    }
    for (size_t i = 0; i < (std::min)(entry.first_words.size(), commands.size()); ++i) {
        entry.first_words[i] = commands[i].argument;
    }
    static std::atomic<u32> reported{};
    const u32 count = reported.fetch_add(1) + 1;
    if (count <= 32 || (count % 4096) == 0) {
        LOG_INFO(HW_GPU,
                 "Split method {:#x} (sub {}, {} words left) on channel {} goes on in entry {} at "
                 "{:#x} (size {}, from {}), not at {:#x} nor {:#x}; it starts {:08x} (#{})",
                 dma_state.method, dma_state.subchannel, dma_state.method_count,
                 channel_state.bind_id, dma_pushbuffer_subindex, header.addr.Value(),
                 header.size.Value(), from_snapshot ? "its submit copy" : "memory",
                 split_method_end, split_stream_end, fmt::join(entry.first_words, " "), count);
    }
}

void DmaPusher::DumpCurrentEntry(const char* reason) {
    LOG_WARNING(HW_GPU,
                "pushbuffer dump ({}) on channel {}, entry {}: method={:#x} sub={} count={} "
                "non_inc={} get={:#x}",
                reason, channel_state.bind_id, dma_pushbuffer_subindex, dma_state.method,
                dma_state.subchannel, dma_state.method_count, dma_state.non_incrementing,
                dma_state.dma_get);
    // Entries whose memory changed after they were parsed (the CPU rewrote them meanwhile).
    for (size_t i = 0; i < std::min(parsed_entry_count, parsed_entries.size()); ++i) {
        const auto& entry = parsed_entries[(parsed_entry_count - 1 - i) % parsed_entries.size()];
        std::vector<u32> words(entry.size);
        memory_manager.ReadBlockUnsafe(entry.address, words.data(), words.size() * sizeof(u32));
        u64 hash = 0xcbf29ce484222325ULL;
        for (const u32 word : words) {
            hash = (hash ^ word) * 0x100000001b3ULL;
        }
        LOG_WARNING(HW_GPU, "  parsed -{}: {:#x}+{:#x} from {}, {}{}", i, entry.address,
                    entry.size, entry.from_snapshot ? "its submit copy" : "memory",
                    hash == entry.hash ? "unchanged" : "CHANGED since parsed",
                    entry.continues_split
                        ? fmt::format("; a split method went on in it, starting {:08x}",
                                      fmt::join(entry.first_words, " "))
                        : std::string{});
    }
    // EDEN_NX2_PB_DUMP=<file>: also write the whole command list (u64 address, u32 size and the
    // current words of every entry, then the current entry index) for offline replay. Only at
    // the first dump: a later one may be another list, after the one first misread.
    static bool written = false;
    if (static const char* const path = std::getenv("EDEN_NX2_PB_DUMP");
        path != nullptr && !written && !dma_pushbuffer.empty()) {
        written = true;
        if (std::FILE* file = std::fopen(path, "wb")) {
            const auto& lists = dma_pushbuffer.front().command_lists;
            const u64 current = dma_pushbuffer_subindex;
            const u64 count = lists.size();
            std::fwrite(&count, sizeof(count), 1, file);
            std::fwrite(&current, sizeof(current), 1, file);
            for (const CommandListHeader& header : lists) {
                const u64 address = header.addr;
                const u32 size = static_cast<u32>(header.size);
                std::vector<u32> words(size);
                memory_manager.ReadBlockUnsafe(address, words.data(), words.size() * sizeof(u32));
                std::fwrite(&address, sizeof(address), 1, file);
                std::fwrite(&size, sizeof(size), 1, file);
                std::fwrite(words.data(), sizeof(u32), words.size(), file);
            }
            std::fclose(file);
        }
    }
    if (dma_pushbuffer.empty()) {
        return;
    }
    const auto& lists = dma_pushbuffer.front().command_lists;
    const size_t first = dma_pushbuffer_subindex >= 2 ? dma_pushbuffer_subindex - 2 : 0;
    for (size_t i = first; i <= dma_pushbuffer_subindex && i < lists.size(); ++i) {
        const CommandListHeader& header = lists[i];
        LOG_WARNING(HW_GPU, "  entry {} of {}: addr={:#x} size={:#x}", i, lists.size(),
                    header.addr.Value(), header.size.Value());
        for (u64 word = 0; word < std::min<u64>(header.size, 0x80); word += 8) {
            std::string gpu_line;
            std::string cpu_line;
            for (u64 j = word; j < std::min<u64>(header.size, word + 8); ++j) {
                const GPUVAddr address = header.addr + j * sizeof(u32);
                const u32 gpu_value = memory_manager.Read<u32>(address);
                const u32 cpu_value = system.ApplicationMemory().Read32(address);
                gpu_line += fmt::format(" {:08x}", gpu_value);
                cpu_line += gpu_value == cpu_value ? std::string(" ........")
                                                   : fmt::format(" {:08x}", cpu_value);
            }
            LOG_WARNING(HW_GPU, "    +{:03x}:{}", word * 4, gpu_line);
            LOG_WARNING(HW_GPU, "    cpu :{}", cpu_line);
        }
    }
}

void DmaPusher::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

} // namespace Tegra
