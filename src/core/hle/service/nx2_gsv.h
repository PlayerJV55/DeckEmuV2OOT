// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <atomic>
#include <deque>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/nx2_title_defaults.h"
#include "common/settings.h"
#include "common/thread.h"
#include "common/video_activity.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/perf_stats.h"
#include "video_core/gpu.h"
#include "video_core/nx2_perf.h"
#include "video_core/framebuffer_config.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/k_system_resource.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_group.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/loader/nso.h"
#include "core/memory.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/h264_packet.h"
#include "video_core/host1x/nx2_video_decoder.h"
#include "video_core/nx2.h"
#include "video_core/nx2_present_sync.h"
#include "video_core/textures/decoders.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/nx2_gsv_server.h"
#include "core/hle/service/nx2_movie_clock.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/sm/sm.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/service.h"

namespace Service {

// Creates a session served by `iface` and returns its client end. Unlike PushIpcInterface this
// works from a domain too, for callers that want a raw session handle. It is served by `server`
// if given, else by the server of the session that asks: what nx2:gsv hands out stays on the
// server ISelfController 2000 picked for it (nx2_gsv_server.h).
template <class T>
inline Kernel::KClientSession* CreateRawSession(Core::System& system, HLERequestContext& ctx,
                                                std::shared_ptr<T> iface,
                                                ServerManager* server = nullptr) {
    auto& kernel = system.Kernel();
    ASSERT(Kernel::GetCurrentProcess(kernel).GetResourceLimit()->Reserve(
        kernel, Kernel::LimitableResource::SessionCountMax, 1));

    auto* session = Kernel::KSession::Create(kernel);
    session->Initialize(kernel, nullptr, 0);
    Kernel::KSession::Register(kernel, session);

    ServerManager& target = server != nullptr ? *server : ctx.GetManager()->GetServerManager();
    auto manager = std::make_shared<SessionRequestManager>(kernel, target);
    manager->SetSessionHandler(std::move(iface));
    target.RegisterSession(&session->GetServerSession(), manager);
    return &session->GetClientSession();
}

// Replies to a TIPC request with success and raw u32 data words. ResponseBuilder::Push(Result)
// writes the CMIF two-word result, which shifts TIPC data one word past where the client reads
// it (and past the copied size); TIPC has a one-word result followed directly by the data.
inline void TipcReplyWords(HLERequestContext& ctx, std::span<const u32> words) {
    IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
    rb.PushRaw<u32>(ResultSuccess.raw);
    for (const u32 w : words) {
        rb.Push<u32>(w);
    }
}

inline void TipcReplyWords(HLERequestContext& ctx, std::initializer_list<u32> words) {
    TipcReplyWords(ctx, std::span<const u32>{words.begin(), words.size()});
}

// Replies with success and a copied handle to a new session served by `iface` (on `server` if
// given, see CreateRawSession). The server keeps a reference too, so the session lives until
// emulation stops.
template <class T>
inline void ReturnRawSession(Core::System& system, HLERequestContext& ctx, std::shared_ptr<T> iface,
                             ServerManager* server = nullptr) {
    auto* client = CreateRawSession(system, ctx, std::move(iface), server);
    IPC::ResponseBuilder rb{ctx, 2, 1, 0};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, *client);
}

// The same reply (a copy handle at word 3), but the client's handle holds the only reference:
// closing it destroys the session and `iface`. For sessions a game opens again and again (one
// per movie), which otherwise kept a decoder thread, an FFmpeg context and a syncpoint each.
// EDEN_NX2_KEEP_SESSIONS=1 keeps the server's reference, as ReturnRawSession does.
template <class T>
inline void ReturnClientOwnedRawSession(Core::System& system, HLERequestContext& ctx,
                                        std::shared_ptr<T> iface) {
    static const bool keep = [] {
        const char* value = std::getenv("EDEN_NX2_KEEP_SESSIONS");
        return value != nullptr && value[0] != '0';
    }();
    if (keep) {
        ReturnRawSession(system, ctx, std::move(iface));
        return;
    }
    auto* client = CreateRawSession(system, ctx, std::move(iface));
    IPC::ResponseBuilder rb{ctx, 2, 1, 0};
    rb.Push(ResultSuccess);
    // Written to the copy handle slot all the same; as a move object, the reference this creator
    // holds is closed once the handle is in the client's table.
    rb.PushMoveObjects(ctx, *client);
}

// The session gsv 1121 returns. Welcome Tour stops booting when 1121 replies without one. Its
// handshake echoes the token; 11 (two u64 arguments, meaning unknown) succeeds with zeros.
class INx2GsvSession1121 final : public ServiceFramework<INx2GsvSession1121> {
public:
    explicit INx2GsvSession1121(Core::System& system_) : ServiceFramework{system_, "nx2:gsv:1121"} {
        RegisterHandlersTipc({
            {0, &INx2GsvSession1121::Handshake, "Handshake"},
            {11, &INx2GsvSession1121::Probe, "Probe (guessed)"},
        });
    }

private:
    void Handshake(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        TipcReplyWords(ctx, {w[2], w[3]});
    }

    void Probe(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_DEBUG(Service, "(STUBBED) nx2:gsv:1121 TIPC 11 args={:#x}, {:#x}",
                  u64{w[2]} | (u64{w[3]} << 32), u64{w[4]} | (u64{w[5]} << 32));
        TipcReplyWords(ctx, {0, 0});
    }
};

// Placeholder for the Switch 2 NVIDIA driver server (nn::nvsrv). It has no handlers, so every
// command (CMIF or TIPC) gets an empty success and is logged through the nx2: prefix.
class INx2NvsrvSession final : public ServiceFramework<INx2NvsrvSession> {
public:
    explicit INx2NvsrvSession(Core::System& system_) : ServiceFramework{system_, "nx2:nvsrv"} {}
};

// The syncpoint values of nn::syncpt's shared RO page (gsv 204). The gsv session's poll mirrors
// them all, and the syncpt sessions' slot actions publish their own. Every write reads the counter
// under `mutex`, so the page never goes back (also across a 32-bit wrap, which a numeric max would
// get stuck on).
struct Nx2SyncptPage {
    static constexpr u32 ValuesOffset = 0x40;  // u64 per syncpoint
    static constexpr u32 HostSyncpoints = 192; // Tegra::Host1x::SyncpointManager

    /// Mirrors syncpoint `id` into the page and returns true if it changed. Needs `mutex`.
    bool UpdateLocked(const Tegra::Host1x::SyncpointManager& manager, u32 id) {
        if (memory == nullptr || id >= (std::min)(count, HostSyncpoints)) {
            return false;
        }
        const u64 value = manager.GetGuestSyncpointValue(id);
        const u64 address = ro + ValuesOffset + id * 8;
        if (memory->Read64(address) == value) {
            return false;
        }
        memory->Write64(address, value);
        return true;
    }

    std::mutex mutex;
    Core::Memory::Memory* memory{}; ///< Set by gsv 204, cleared when the gsv session closes
    u64 ro{};
    u32 count{};
};

// Placeholder for the Switch 2 syncpoint server (nn::syncpt), handed out by gsv 202. TIPC 1
// returns a u32 the SDK requires to be < 0x400 and then passes min(value, 63) to TIPC 21.
// Everything else is auto-stubbed through the nx2: prefix.
// - TIPC 0(u64): handshake, echo the token (newer SDKs).
// - TIPC 22(u32 slot): an event handle for that slot (copy handle at word 3, result at word 4).
//   NVN's NvOsEventPool asks for slots and multi-waits on them.
// - TIPC 201(slot, counter, u64 value, mode): signal the slot's event once the counter reaches
//   value. The counter is a SyncCounterId, the bitwise NOT of the host1x syncpoint id; mode 0
//   compares 32-bit wrapping, mode 1 64-bit. Eden's guest syncpoints back it.
// - TIPC 202(slot): cancel the slot's request.
class INx2SyncptSession final : public ServiceFramework<INx2SyncptSession> {
public:
    explicit INx2SyncptSession(Core::System& system_, std::shared_ptr<Nx2SyncptPage> page)
        : ServiceFramework{system_, "nx2:syncpt"}, m_context{system_, "nx2:syncpt"} {
        m_shared->page = std::move(page);
        RegisterHandlersTipc({
            {0, &INx2SyncptSession::Handshake, "Ping"},
            {1, &INx2SyncptSession::GetCount, "GetCount (guessed)"},
            {22, &INx2SyncptSession::GetSlotEvent, "GetSlotEvent (guessed)"},
            {103, &INx2SyncptSession::QueryCounter, "QueryCounter"},
            {201, &INx2SyncptSession::RequestSlotEvent, "RequestSlotEvent (guessed)"},
            {202, &INx2SyncptSession::CancelSlotEvent, "CancelSlotEvent (guessed)"},
        });
    }

    ~INx2SyncptSession() override {
        // A registered action may be running on the GPU thread right now: mark the session dead
        // under the lock it signals under, so it can't touch a request or event closed below.
        {
            std::scoped_lock lk{m_shared->mutex};
            m_shared->alive = false;
        }
        for (auto& [slot, request] : m_requests) {
            Cancel(request);
        }
        for (auto& [slot, event] : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    // Newer SDKs (title 1) open with TIPC 0(u64 token) and abort unless the reply echoes it.
    void Handshake(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_INFO(Service, "nx2:syncpt TIPC 0 handshake {:#x}", u64{w[2]} | (u64{w[3]} << 32));
        TipcReplyWords(ctx, {w[2], w[3]});
    }

    void GetCount(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:syncpt TIPC 1, returning 32");
        TipcReplyWords(ctx, {32});
    }

    // From changes.patch: the counter is the bitwise NOT of the host1x syncpoint id.
    void QueryCounter(HLERequestContext& ctx) {
        const u32 syncpoint = ~ctx.CommandBuffer()[2];
        const u64 value = syncpoint < HostSyncpoints
                              ? system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(syncpoint)
                              : 0;
        LOG_DEBUG(Service, "nx2:syncpt 103 syncpoint={} value={:#x}", syncpoint, value);
        TipcReplyWords(ctx, {static_cast<u32>(value), static_cast<u32>(value >> 32), 0});
    }

    // On hardware an idle slot's event is signalled: nn::syncpt::sdk skips 201 for a fence the
    // page already shows reached and waits on the event all the same, and DestroySyncPointEvent
    // only frees a slot whose event is signalled. Created unsignalled, a movie that got a fresh
    // slot could wait forever. A request (201) clears the event before it registers.
    // EDEN_NX2_SYNCPT_SIGNAL_NEW_SLOTS=0 creates slot events unsignalled, as before.
    Kernel::KEvent* SlotEvent(u32 slot) {
        static const bool signal_new = [] {
            const char* value = std::getenv("EDEN_NX2_SYNCPT_SIGNAL_NEW_SLOTS");
            return value == nullptr || value[0] != '0';
        }();
        auto*& event = m_events[slot];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:syncpt:Slot{:x}", slot));
            if (signal_new) {
                event->Signal(system.Kernel());
            }
        }
        return event;
    }

    void GetSlotEvent(HLERequestContext& ctx) {
        const u32 slot = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:syncpt TIPC 22 slot={:#x}, returning an event", slot);
        auto* event = SlotEvent(slot);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    // Map nodes are stable, so a registered action can hold a reference to its request.
    struct SlotRequest {
        u32 syncpoint{};
        std::atomic<bool> pending{};
        u64 generation{}; ///< Bumped by each request, under SharedState::mutex
        Tegra::Host1x::SyncpointManager::ActionHandle handle{};
    };

    // Outlives the session (actions hold a reference): signalling happens under `mutex` and only
    // while `alive`.
    struct SharedState {
        std::mutex mutex;
        bool alive{true};
        std::shared_ptr<Nx2SyncptPage> page; ///< That of the gsv session that opened this one
    };

    // A slot action writes its syncpoint's value into the RO page before it signals, so a client
    // woken by the event reads the value it waited for. Left to the 500 us poll, title 1's movie
    // player usually found its fence not reached yet and armed a wait, which only the poll's read
    // order kept from aborting. EDEN_NX2_SYNCPT_PUBLISH_IN_ACTION=0 leaves the page to the poll,
    // as before.
    static bool PublishInAction() {
        static const bool publish = [] {
            const char* value = std::getenv("EDEN_NX2_SYNCPT_PUBLISH_IN_ACTION");
            return value == nullptr || value[0] != '0';
        }();
        return publish;
    }

    void RequestSlotEvent(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        const u32 slot = cmd_buf[2];
        const u32 syncpoint = ~cmd_buf[3];
        const u64 value = u64{cmd_buf[4]} | (u64{cmd_buf[5]} << 32);
        const u32 mode = cmd_buf[6];
        LOG_DEBUG(Service, "nx2:syncpt TIPC 201 slot={:#x} syncpoint={} value={:#x} mode={}", slot,
                  syncpoint, value, mode);
        auto* event = SlotEvent(slot);
        if (syncpoint >= HostSyncpoints) {
            LOG_WARNING(Service, "nx2:syncpt TIPC 201 syncpoint {} out of range, signalling",
                        syncpoint);
            event->Signal(system.Kernel());
            TipcReplyWords(ctx, {});
            return;
        }
        auto& request = m_requests[slot];
        Cancel(request);
        u64 generation{};
        {
            // A new request starts unsignalled. Slots are reused by different client objects:
            // title 1's movie player got slot 0 after NVN's fence waits had left it signalled,
            // took that as a decoded frame and aborted when the decoder had none. The previous
            // request's action may be between pending = false and its Signal: taking the mutex
            // here orders that signal before this Clear, and the generation would drop it if an
            // old action ever ran later.
            std::scoped_lock lk{m_shared->mutex};
            generation = ++request.generation;
            event->Clear(system.Kernel());
            request.syncpoint = syncpoint;
            request.pending = true;
        }
        auto& manager = system.Host1x().GetSyncpointManager();
        // The action runs on the GPU thread outside the manager's lock (a cancel waits for it), or
        // inline if the value is already reached (so the mutex is not held here). Locks go
        // session, page.
        request.handle = manager.RegisterGuestAction(
            syncpoint, static_cast<u32>(value),
            [shared = m_shared, &system = system, &request, event, generation] {
                std::scoped_lock lk{shared->mutex};
                if (!shared->alive || request.generation != generation) {
                    return;
                }
                request.pending = false;
                if (PublishInAction()) {
                    std::scoped_lock page_lk{shared->page->mutex};
                    shared->page->UpdateLocked(system.Host1x().GetSyncpointManager(),
                                               request.syncpoint);
                }
                event->Signal(system.Kernel());
            });
        TipcReplyWords(ctx, {});
    }

    void CancelSlotEvent(HLERequestContext& ctx) {
        const u32 slot = ctx.CommandBuffer()[2];
        LOG_DEBUG(Service, "nx2:syncpt TIPC 202 slot={:#x}", slot);
        if (auto it = m_requests.find(slot); it != m_requests.end()) {
            Cancel(it->second);
        }
        // nn::syncpt::sdk's cancel always waits on the slot's event after this reply, so a cancel
        // signals it like a completion, also when nothing was pending; the client rechecks the
        // counter (TIPC 103) itself. Title 1 hung forever there when the GPU thread ran a frame
        // behind (EDEN_NX2_FRAMES_AHEAD) and a mid-frame wait outlasted NvOsEventPool's 100 ms,
        // and its movie player cancels each slot before its first request: signalled only for
        // pending requests, the player never got to start a movie.
        SlotEvent(slot)->Signal(system.Kernel());
        TipcReplyWords(ctx, {});
    }

    /// Returns true if the request was still pending.
    bool Cancel(SlotRequest& request) {
        if (request.pending.exchange(false)) {
            system.Host1x().GetSyncpointManager().DeregisterGuestAction(request.syncpoint,
                                                                        request.handle);
            return true;
        }
        return false;
    }

    static constexpr u32 HostSyncpoints = 192; // Tegra::Host1x::SyncpointManager

    KernelHelpers::ServiceContext m_context;
    std::map<u32, Kernel::KEvent*> m_events;
    std::map<u32, SlotRequest> m_requests;
    std::shared_ptr<SharedState> m_shared = std::make_shared<SharedState>();
};

// Placeholder for the Switch 2 GPU session (nn::gpu::sdk), handed out by gsv 497.
// - TIPC 11: [first, last] id range (first <= min(last, 64)); ids outside it are masked out.
// - TIPC 12(id): event handle for that id (handle at word 3, result at word 4); the SDK attaches
//   it as a SystemEvent.
// - TIPC 21(id): starts an operation; the SDK then waits on the id's event. Nothing runs here, so
//   the event is signalled at once.
// - TIPC 22: another handle the SDK keeps like a session; a placeholder session is returned.
class INx2GpuSession final : public ServiceFramework<INx2GpuSession> {
public:
    explicit INx2GpuSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gpu"}, m_context{system_, "nx2:gpu"} {
        RegisterHandlersTipc({
            {11, &INx2GpuSession::GetIdRange, "GetEventIdRange"},
            {12, &INx2GpuSession::GetIdEvent, "CreateEvent"},
            {21, &INx2GpuSession::Start, "RequestKUserWorkerSession"},
            {22, &INx2GpuSession::OpenSubSession, "GetKUserWorkerSession"},
            {33, &INx2GpuSession::GetCount33, "CreateGpuAddressSpace"},
            {41, &INx2GpuSession::CreateGraphicsDevice, "CreateGraphicsDevice"},
            {42, &INx2GpuSession::DestroyGraphicsDevice, "DestroyGraphicsDevice"},
            {201, &INx2GpuSession::GetUserPartition, "GetUserPartitionGpuAddress"},
            {202, &INx2GpuSession::GetGsvPartition, "GetGsvPartitionGpuAddress"},
            {203, &INx2GpuSession::GetVaRange, "GetVaRange (guessed)"},
            // KUserWorkerSession commands (22 hands out that session; this same object answers).
            {101, &INx2GpuSession::SubRegisterMemory, "CreateGpuMemory"},
            {102, &INx2GpuSession::SubDestroyMemory, "DestroyGpuMemory"},
            {211, &INx2GpuSession::SubSubmitJob, "NormalMapGpuMemory4K"},
            {212, &INx2GpuSession::SubModifyKind, "NormalModifyMemoryKind4K"},
            {213, &INx2GpuSession::SubUnmapJob, "NormalUnmapGpuMemory4K"},
            {214, &INx2GpuSession::SubCreateGpuOnlyRegion, "CreateSparseRegion"},
            {215, &INx2GpuSession::SubDestroySparseRegion, "DestroySparseRegion"},
            {216, &INx2GpuSession::SubSparseMap, "ExecuteSparseMapRequests"},
        });
        if (LazySparseRegions() || LazyGpuOnlyRegions()) {
            Tegra::Nx2SetSparseBacking(
                [this](u64 gpu_address, u64 size, const u8* data) {
                    BackSparseWrite(gpu_address, size, data);
                });
        }
    }

    ~INx2GpuSession() override {
        // Waits for a backing the GPU thread may be doing.
        Tegra::Nx2SetSparseBacking({});
        // Jobs still queued on the GPU thread must not signal the events closed below.
        {
            std::scoped_lock lk{m_shared->mutex};
            m_shared->alive = false;
        }
        // Destroyed regions the GPU thread is done with go back to the kernel. Live ones stay: the
        // GPU may still use them, and at shutdown the kernel takes them with everything else.
        FreeReleasedRegions();
        for (auto* event : m_events) {
            if (event != nullptr) {
                m_context.CloseEvent(event);
            }
        }
    }

private:
    static constexpr u32 NumIds = 32;

    Kernel::KEvent* EventFor(u32 id) {
        id %= NumIds;
        if (m_events[id] == nullptr) {
            m_events[id] = m_context.CreateEvent(fmt::format("nx2:gpu:Event{}", id));
        }
        return m_events[id];
    }

    void GetIdRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 11, returning [0, {}]", NumIds - 1);
        TipcReplyWords(ctx, {0, NumIds - 1});
    }

    void GetIdEvent(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 12 id={}", id);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, EventFor(id)->GetReadableEvent());
    }

    void Start(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 21 id={}, signalling completion", id);
        EventFor(id)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // nn::gpu::sdk::CreateGraphicsDevice(name, e.g. "NvRmGpu" in six words): the reply's first
    // byte is the device id, the first free of 8 slots. The auto stub's reply left the request's
    // name bytes there, so Fire Emblem's CUDA driver got device 0x47.
    void CreateGraphicsDevice(HLERequestContext& ctx) {
        u32 id = 0;
        while (id < m_graphics_devices.size() && m_graphics_devices[id]) {
            ++id;
        }
        if (id == m_graphics_devices.size()) {
            LOG_ERROR(Service, "nx2:gpu TIPC 41: no free graphics device slot");
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultUnknown);
            return;
        }
        m_graphics_devices[id] = true;
        LOG_INFO(Service, "nx2:gpu TIPC 41 graphics device {}", id);
        TipcReplyWords(ctx, {id});
    }

    void DestroyGraphicsDevice(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_INFO(Service, "nx2:gpu TIPC 42 graphics device {}", id);
        if (id < m_graphics_devices.size()) {
            m_graphics_devices[id] = false;
        }
        TipcReplyWords(ctx, {});
    }

    void GetCount33(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 33 [2]={:#x}, returning 1", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {1});
    }

    // Used by nn::gpu::sdk::CreateGpuAddressSpace: three u64s after the result, a kind (< 5, 0 =
    // none) and the GPU VA range of the in-process VirtualMachine that serves AllocSpace
    // (nn::gpu::sdk::util::ExecuteVirtualMachineAllocSpace). NVN reserves fixed VA from 0x8000000
    // up to 512 GiB, so report the same range NvRm gives AllocAsEx: [0x8000000, 0xC000000000).
    void GetVaRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 203 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 kind = 1;
        constexpr u64 start = 0x8000000ULL;
        constexpr u64 size = 0xC000000000ULL - start;
        TipcReplyWords(ctx, {static_cast<u32>(kind), static_cast<u32>(kind >> 32),
                             static_cast<u32>(start), static_cast<u32>(start >> 32),
                             static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    // Newer SDKs (title 1) query the address space's user and gsv partitions with 201/202(id):
    // two u64s each, stored as the partition's min/max. The user value works whether it is read
    // as (start, size) or (min, max). Title 4 never needed the gsv partition, so it stays empty.
    void GetUserPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 201 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 first = 0x8000000ULL;
        constexpr u64 second = 0xC000000000ULL - first;
        TipcReplyWords(ctx, {static_cast<u32>(first), static_cast<u32>(first >> 32),
                             static_cast<u32>(second), static_cast<u32>(second >> 32)});
    }

    void GetGsvPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 202 [2]={:#x}, empty", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {0, 0, 0, 0});
    }

    void OpenSubSession(HLERequestContext& ctx) {
        // Served by this object so sub-session jobs can signal the per-id events.
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 22, returning a sub-session on nx2:gpu");
        ReturnRawSession(system, ctx, std::static_pointer_cast<INx2GpuSession>(shared_from_this()));
    }

    // Sub 101: (start u64, last u64, flags...) after CreateGpuAccessibleMemory's svc 0x80.
    void SubRegisterMemory(HLERequestContext& ctx) {
        const auto* b = ctx.CommandBuffer();
        const u64 start = u64{b[2]} | (u64{b[3]} << 32);
        const u64 last = u64{b[4]} | (u64{b[5]} << 32);
        LOG_DEBUG(Service, "nx2:gpu sub 101 start={:#x} last={:#x} [6]={:#x} [7]={:#x}", start,
                    last, b[6], b[7]);
        if (last > start) {
            {
                // The GPU thread takes the registry's lock too (BackSparse): queue its work after.
                Tegra::GPU::RunInOrderBatch batch;
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Register(start, last - start + 1);
            }
            // Two-page regions hold queue semaphores: GPU releases go to the second page, waits
            // read the first (see video_core/nx2.h).
            if (last - start + 1 == 0x2000) {
                Tegra::Nx2AddSemaphoreRegion(start);
                LOG_DEBUG(Service, "nx2:gpu semaphore page {:#x} mirrored to {:#x}", start + 0x1000,
                         start);
            }
        }
        // nn::gpu::sdk::CreateGpuAccessibleMemory stores word 3 of the reply as the
        // GpuMemoryHandle that later map jobs (211) name.
        const u32 handle = m_next_memory_handle++;
        m_memory[handle] = {start, last - start + 1};
        TipcReplyWords(ctx, {handle});
    }

    // Jobs that complete through a job-slot event (word Slot of the request), like 211.
    template <size_t Slot>
    void SubSlotJob(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_DEBUG(Service, "(STUBBED) nx2:gpu {} words {:08x} slot={}", ctx.GetCommand(),
                 fmt::join(std::span<const u32>{w + 2, 18}, " "), w[Slot]);
        EventFor(w[Slot])->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 211: job submission (nn::gpu::sdk job tracking). Word 2 is a constant id; word 17 is
    // the job slot whose event (gpu TIPC 12) the SDK then waits on. Nothing runs here, so
    // complete it at once.
    void SubSubmitJob(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        const u32 slot = cmd_buf[17];
        const u64 a = u64{cmd_buf[3]} | (u64{cmd_buf[4]} << 32);
        const u64 b = u64{cmd_buf[5]} | (u64{cmd_buf[6]} << 32);
        const u64 c = u64{cmd_buf[8]} | (u64{cmd_buf[9]} << 32);
        // Words 3-4: first GPU address, 5-6: last, 7: memory handle, 8-9: offset into it.
        const u32 handle = cmd_buf[7];
        LOG_DEBUG(Service, "nx2:gpu 211 map [{:#x}, {:#x}] handle={} offset={:#x} slot={}", a, b,
                 handle, c, slot);
        FreeReleasedRegions();
        if (const auto it = m_memory.find(handle); it != m_memory.end() && b > a) {
            // Also at the memory's own address: an earlier 213 may have unmapped the mapping 101
            // made there.
            const u64 cpu = it->second.address + c;
            // Its GPU work is queued once the registry's lock is released (as in 101), still
            // before the event below.
            Tegra::GPU::RunInOrderBatch batch;
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapAt(a, cpu, b - a + 1);
        } else {
            LOG_WARNING(Service, "nx2:gpu 211 map of unknown handle {}", handle);
        }
        EventFor(slot)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 212 (nn::gpu::sdk::NormalModifyMemoryKind4K, behind NvRmGpuMappingModify): word 2 the
    // address space, 3-4 first GPU address, 5-6 last, 7 the wanted (compressible) kind, 8 the
    // fallback kind, 9 a sync flag, 10 the job slot. The reply's word 3 is the kind applied. Kinds
    // do not matter to the emulated GPU, so keep the mapping and report the wanted kind.
    void SubModifyKind(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        const u32 kind = w[7] & 0xff;
        LOG_DEBUG(Service, "nx2:gpu 212 modify [{:#x}, {:#x}] kind={:#x} fallback={:#x} slot={}",
                  first, last, kind, w[8] & 0xff, w[10]);
        EventFor(w[10])->Signal(system.Kernel());
        TipcReplyWords(ctx, {kind});
    }

    // Sub 213: words 3-4 first GPU address, 5-6 last, job slot in word 8.
    void SubUnmapJob(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        LOG_DEBUG(Service, "nx2:gpu 213 unmap [{:#x}, {:#x}] slot={}", first, last, w[8]);
        FreeReleasedRegions();
        if (last > first) {
            // After the registry's lock, as in 101.
            Tegra::GPU::RunInOrderBatch batch;
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(first, last - first + 1);
        }
        SignalAfterGpu(w[8]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Signals a job slot's event once the GPU thread has run everything queued so far, as the
    // console's graphics server completes unmap jobs behind the submitted work.
    void SignalAfterGpu(u32 slot) {
        auto* const event = EventFor(slot);
        system.GPU().RunInOrder([shared = m_shared, &kernel = system.Kernel(), event] {
            std::scoped_lock lk{shared->mutex};
            if (shared->alive) {
                event->Signal(kernel);
            }
        });
    }

    // Sub 102: word 2 is the memory handle 101 returned, word 3 the job slot. The game frees the
    // memory (svc 0x80 clears GPU access) and reuses it as ordinary heap right after, so nothing
    // on the GPU side may keep it: drop the mapping 101 made at its own address.
    void SubDestroyMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 handle = w[2];
        FreeReleasedRegions();
        if (const auto it = m_memory.find(handle); it != m_memory.end()) {
            const auto [address, size] = it->second;
            LOG_DEBUG(Service, "nx2:gpu 102 destroy handle={} [{:#x}, +{:#x}] slot={}", handle,
                      address, size, w[3]);
            if (size != 0) {
                // After the registry's lock, as in 101.
                Tegra::GPU::RunInOrderBatch batch;
                auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
                registry.Unmap(address, size);
                // Its device ranges too, once no GPU mapping uses the memory. Kept, they hold the
                // pages the memory had: Mario Kart World creates new memory at the same address,
                // and reading it through an old range showed the old pages.
                if (Nvidia::Devices::Nx2DeviceRangeCheck()) {
                    registry.ReleaseWhenUnmapped(address, size);
                }
            }
            if (size == 0x2000) {
                Tegra::Nx2RemoveSemaphoreRegion(address);
            }
            m_memory.erase(it);
        } else {
            LOG_WARNING(Service, "nx2:gpu 102 destroy of unknown handle {}", handle);
        }
        SignalAfterGpu(w[3]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 214: GPU-only memory [start, last] (words 3-6, job slot in word 11). The graphics
    // server backs it itself: the client never maps anything there but renders into it (title 4
    // keeps its render targets in six 300 MB regions at 0x81_0000_0000). Back each region with
    // kernel memory placed in the process's alias code region and map that for the GPU.
    void SubCreateGpuOnlyRegion(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 start = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        const u32 slot = w[11];
        LOG_DEBUG(Service, "nx2:gpu 214 words {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}",
                 w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12]);
        if (last > start && ((last + 1 - start) % Core::Memory::YUZU_PAGESIZE) == 0) {
            const u64 size = last + 1 - start;
            FreeReleasedRegions();
            // The GPU thread takes m_region_mutex too (BackSparseWrite): queue its work after.
            Tegra::GPU::RunInOrderBatch batch;
            std::scoped_lock lk{m_region_mutex};
            if ((w[7] & SparseRegionFlag) == 0) {
                // Created again at the same start (title 1 grows regions 300 MB -> 600 MB -> 1 GB):
                // the new backing is mapped over the game's tiles there, so forget them as 215 does.
                if (const auto old = m_gpu_only_ranges.find(start); old != m_gpu_only_ranges.end()) {
                    DropGpuOnlyTiles(start, old->second, false);
                    m_shared_regions.erase(start);
                    std::erase_if(m_shared_memory, [start](const auto& entry) { return entry.second == start; });
                    // Backed as it was written: its device range and blocks go too, as with 215.
                    // Left there, each re-creation would leak a device range of its size, and
                    // backing a block again would unmap that block of the new region.
                    if (const auto lazy = m_sparse_regions.find(start);
                        lazy != m_sparse_regions.end() && lazy->second.gpu_only) {
                        Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DestroySparse(
                            start, lazy->second.size);
                        ReleaseBackings(start, start + lazy->second.size - 1);
                        m_sparse_regions.erase(lazy);
                    }
                }
                m_gpu_only_ranges[start] = size; // kept out of 216, however it is backed
            }
            if ((w[7] & SparseRegionFlag) != 0 && LazySparseRegions()) {
                // Title 1's terrain texture arrays and layer pools (170 MB-5.8 GB, mostly empty):
                // backed whole, they ran the pool dry and the later ones got no memory at all.
                // The region gets a device range of its own, unbacked pages read zero, and
                // BackSparseWrite gives a block memory before the GPU writes into it.
                m_sparse_regions[start] = {
                    .size = size,
                    .blocks = std::vector<u8>(Common::DivCeil(size, SparseBlockSize)),
                };
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().CreateSparse(start, size);
                Tegra::Nx2SparseRegionCreated(start, size, SparseBlockSize);
                Tegra::Nx2AddSparseSpan(start, size);
                LOG_INFO(Service, "nx2:gpu 214 sparse region [{:#x}, {:#x}] backed as the GPU writes it",
                         start, last);
            } else if ((w[7] & SparseRegionFlag) == 0 && LazyGpuOnlyRegions()) {
                // The rest is written by the host GPU, not through guest memory (title 1's and
                // title 4's render targets, 285-300 MB regions): like a sparse region it gets a
                // device range of its own and memory per block only when something writes guest
                // memory there (downloads, copies). The texture cache sees ordinary memory.
                m_sparse_regions[start] = {
                    .gpu_only = true,
                    .size = size,
                    .blocks = std::vector<u8>(Common::DivCeil(size, SparseBlockSize)),
                };
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().CreateSparse(start, size);
                Tegra::Nx2AddSparseSpan(start, size);
                LOG_INFO(Service, "nx2:gpu 214 GPU-only region [{:#x}, {:#x}] backed as it is written",
                         start, last);
            } else if (const auto address = BackGpuOnlyRegion(start, size)) {
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapAt(start, *address, size);
                LOG_INFO(Service, "nx2:gpu 214 GPU-only region [{:#x}, {:#x}] backed at {:#x} "
                                  "({} MiB of GPU-only memory in use, peak {} MiB)",
                         start, last, *address, m_gpu_only_bytes >> 20, m_gpu_only_peak >> 20);
            }
        } else {
            LOG_WARNING(Service, "nx2:gpu 214 unexpected region [{:#x}, {:#x}]", start, last);
        }
        EventFor(slot)->Signal(system.Kernel());
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 215 (nn::gpu::sdk::DestroySparseRegion): words 3-6 the range, word 8 the job slot, like
    // 213. Drop the GPU mapping, then the backing 214 added: its SMMU mapping (in GPU-thread
    // order) and, once the GPU thread is past that, its pages. Kept allocated, title 1 ran the
    // pool dry within minutes and later regions had no memory at all (GPU writes to them lost).
    void SubDestroySparseRegion(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 first = u64{w[3]} | (u64{w[4]} << 32);
        const u64 last = u64{w[5]} | (u64{w[6]} << 32);
        LOG_DEBUG(Service, "nx2:gpu 215 destroy sparse region [{:#x}, {:#x}] words {:#x} {:#x} {:#x} {:#x}",
                 first, last, w[1], w[2], w[7], w[8]);
        FreeReleasedRegions();
        Tegra::GPU::RunInOrderBatch batch;
        std::scoped_lock lk{m_region_mutex};
        if (m_gpu_only_ranges.erase(first) != 0 && last > first) {
            DropGpuOnlyTiles(first, last - first + 1, false);
            m_shared_regions.erase(first);
            std::erase_if(m_shared_memory, [first](const auto& entry) { return entry.second == first; });
        }
        if (last > first) {
            if (const auto region = m_sparse_regions.find(first); region != m_sparse_regions.end()) {
                m_tile_bytes -= Core::Memory::YUZU_PAGESIZE *
                                static_cast<u64>(std::count(region->second.tile_pages.begin(),
                                                            region->second.tile_pages.end(), true));
                m_sparse_regions.erase(region);
                Tegra::Nx2SparseRegionDestroyed(first);
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DestroySparse(first, last - first + 1);
            } else {
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(first, last - first + 1);
            }
            // The region's backing: all of it, or the blocks a sparse region got as it was written.
            ReleaseBackings(first, last);
        }
        SignalAfterGpu(w[8]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Sub 216 (nn::gpu::sdk::ExecuteSparseMapRequests, behind nvnMemoryPoolMapVirtual and NvRm's
    // remap): an A buffer of SparseMapRequests and a B buffer that gets the kind applied to each;
    // word 8-9 is 1, 10 the address space, 11-12 the request size (40), 13 a sync flag, 14 the job
    // slot. The reply is the number of requests done. This is how the game commits and releases
    // its sparse texture tiles: a map points a tile at its own memory (sub 101), an unmap makes it
    // read zero again. A region the game maps tiles into needs no memory of its own.
    void SubSparseMap(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 slot = w[14];
        const auto data = ctx.BufferDescriptorA().empty() ? std::span<const u8>{} : ctx.ReadBufferA(0);
        const size_t count = data.size() / sizeof(SparseMapRequest);
        std::vector<u8> kinds(count);
        FreeReleasedRegions();
        {
            Tegra::GPU::RunInOrderBatch batch;
            std::scoped_lock lk{m_region_mutex};
            ++m_tile_calls;
            for (size_t i = 0; i < count; ++i) {
                SparseMapRequest request;
                std::memcpy(&request, data.data() + i * sizeof(SparseMapRequest), sizeof(request));
                kinds[i] = request.kind;
                ApplySparseMapRequest(request);
            }
            // The first request, then at most every 30 s.
            const auto now = std::chrono::steady_clock::now();
            if (count != 0 && (m_tile_requests == count ||
                               now - m_tile_log_time >= std::chrono::seconds{30})) {
                m_tile_log_time = now;
                LOG_INFO(Service, "nx2:gpu 216: {} calls, {} maps, {} unmaps; {} MiB of sparse tiles "
                                  "and {} MiB in GPU-only regions mapped, {} maps into regions that "
                                  "keep their own memory left out; {} MiB of sparse blocks backed, {} "
                                  "MiB released",
                         m_tile_calls, m_tile_maps, m_tile_unmaps, m_tile_bytes >> 20,
                         m_gpu_only_tile_bytes >> 20, m_tile_kept, m_sparse_backed_bytes >> 20,
                         m_sparse_released_bytes >> 20);
            }
        }
        if (!ctx.BufferDescriptorB().empty() && count != 0) {
            ctx.WriteBufferB(kinds.data(), kinds.size(), 0);
        }
        SignalAfterGpu(slot);
        TipcReplyWords(ctx, {static_cast<u32>(count), static_cast<u32>(u64{count} >> 32)});
    }

    static bool IsAllZero(const u8* data, size_t size) {
        size_t i = 0;
        for (; i + sizeof(u64) <= size; i += sizeof(u64)) {
            u64 word;
            std::memcpy(&word, data + i, sizeof(word));
            if (word != 0) {
                return false;
            }
        }
        for (; i < size; ++i) {
            if (data[i] != 0) {
                return false;
            }
        }
        return true;
    }

    // Returns the pages of destroyed regions the GPU thread is done with to the kernel. Only the
    // service thread touches m_released_regions (its handlers are serialized), so this unmaps
    // without m_region_mutex, like AllocateGpuOnlyBacking maps. Also runs at the start of 102, 211
    // and 213, so the pages don't wait for the next 214/215.
    void FreeReleasedRegions() {
        if (m_released_regions.empty()) {
            return;
        }
        auto* const process = system.ApplicationProcess();
        u64 freed = 0;
        std::erase_if(m_released_regions, [&](ReleasedRegion& released) {
            if (!released.done->load(std::memory_order_acquire)) {
                return false;
            }
            auto& region = released.region;
            if (process != nullptr &&
                R_FAILED(process->GetPageTable().UnmapPageGroup(
                    region.cpu_address, *region.pages, Kernel::KMemoryState::IoMemory))) {
                // Still mapped: keep its pages, and try again a few times.
                if (released.unmap_attempts++ == 0) {
                    LOG_ERROR(Service, "nx2:gpu could not unmap GPU-only backing {:#x}+{:#x}",
                              region.cpu_address, region.size);
                }
                return released.unmap_attempts >= MaxUnmapAttempts;
            }
            region.pages->Close(system.Kernel());
            freed += region.size;
            return true;
        });
        if (freed != 0) {
            std::scoped_lock lk{m_region_mutex};
            m_gpu_only_bytes -= freed;
        }
    }

    struct MappedBacking {
        u64 address;
        u64 size;
        std::unique_ptr<Kernel::KPageGroup> pages;
    };

    // Allocates kernel memory and maps it in the alias code region. Touches no region state, so
    // it needs no lock: the GPU back thread calls it without m_region_mutex, because mapping can
    // wait on the guest page table lock and a guest thread holding that lock must not be stalled
    // behind a host mutex the service thread is waiting for.
    std::optional<MappedBacking> AllocateGpuOnlyBacking(u64 size, bool log_failure, bool clear) {
        using Core::Memory::YUZU_PAGESIZE;
        auto& kernel_core = system.Kernel();
        auto* const process = system.ApplicationProcess();
        if (process == nullptr) {
            return std::nullopt;
        }
        auto pg = std::make_unique<Kernel::KPageGroup>(
            kernel_core, std::addressof(kernel_core.GetSystemSystemResource().GetBlockInfoManager()));
        // The graphics server's own memory, not the application's: taken from the application
        // pool, title 4's six regions left it 1.8 GB less than the kernel reports to it. Not
        // cleared: the GPU writes these before reading them, and touching 300 MB would commit it
        // on the host.
        if (R_FAILED(kernel_core.MemoryManager().AllocateAndOpen(
                pg.get(), size / YUZU_PAGESIZE,
                Kernel::KMemoryManager::EncodeOption(Kernel::KMemoryManager::Pool::SystemNonSecure,
                                                     Kernel::KMemoryManager::Direction::FromBack)))) {
            if (log_failure) {
                LOG_ERROR(Service, "nx2:gpu 214 could not allocate {:#x} bytes", size);
            }
            return std::nullopt;
        }
        if (clear) {
            // Cheap for memory that's still zero (most of it): only reused pages get cleared.
            for (const auto& block : *pg) {
                system.DeviceMemory().buffer.ClearBackingRegion(
                    GetInteger(block.GetAddress()) - Core::DramMemoryMap::Base, block.GetSize(), 0);
            }
        }
        auto& page_table = process->GetPageTable();
        const u64 region_start = GetInteger(page_table.GetAliasCodeRegionStart());
        const u64 region_size = page_table.GetAliasCodeRegionSize();
        constexpr u64 Alignment = 0x200000; // 2 MiB
        // From where the last backing went (sparse regions back hundreds of blocks), then wrapping.
        const u64 slots = region_size / Alignment;
        for (u64 i = 0; i < slots; ++i) {
            const u64 offset = ((m_alias_hint.load(std::memory_order_relaxed) + i) % slots) * Alignment;
            if (offset + size > region_size) {
                continue;
            }
            const Common::ProcessAddress address{region_start + offset};
            if (R_SUCCEEDED(page_table.MapPageGroup(address, *pg, Kernel::KMemoryState::IoMemory,
                                                    Kernel::KMemoryPermission::UserReadWrite))) {
                m_alias_hint.store((offset + Common::AlignUp(size, Alignment)) / Alignment,
                                   std::memory_order_relaxed);
                return MappedBacking{GetInteger(address), size, std::move(pg)};
            }
        }
        LOG_ERROR(Service, "nx2:gpu 214 no room for {:#x} bytes in the alias code region "
                           "[{:#x}, +{:#x})", size, region_start, region_size);
        pg->Close(kernel_core);
        return std::nullopt;
    }

    // Records a mapped backing for the region at gpu_address. Needs m_region_mutex.
    void RegisterGpuOnlyBacking(u64 gpu_address, MappedBacking&& backing) {
        // A title may create the same region again from the same start with a different size
        // (Mario Kart World walks 300 MB -> 600 MB -> 1 GB on the same address). Overwriting the
        // entry used to drop the previous mapping without undoing it: the backing stayed mapped
        // in the alias code region and its pages never went back to the pool, so the GPU-only
        // total grew without bound (250 MiB -> 6.5 GiB in one session) until the process died.
        // Release the old backing the same way 215 does, i.e. only once the GPU thread is past it.
        if (const auto it = m_gpu_only_regions.find(gpu_address); it != m_gpu_only_regions.end()) {
            {
                // BackSparseWrite gets here without a batch of its own: the release work goes
                // after the registry's lock, still before `done` below.
                Tegra::GPU::RunInOrderBatch batch;
                auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
                registry.Unmap(gpu_address, it->second.size);
                registry.ReleaseBacking(it->second.cpu_address, it->second.size);
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            system.GPU().RunInOrder([done] { done->store(true, std::memory_order_release); });
            m_released_regions.push_back({std::move(it->second), done});
            m_gpu_only_regions.erase(it);
        }
        m_gpu_only_regions[gpu_address] = {backing.address, backing.size, std::move(backing.pages)};
        m_gpu_only_bytes += backing.size;
        m_gpu_only_peak = (std::max)(m_gpu_only_peak, m_gpu_only_bytes);
    }

    // Releases the backings in [first, last] as 215 does: their SMMU mappings go in GPU-thread
    // order and FreeReleasedRegions returns the pages after. Service thread, with m_region_mutex.
    void ReleaseBackings(u64 first, u64 last) {
        std::shared_ptr<std::atomic<bool>> done;
        for (auto region = m_gpu_only_regions.lower_bound(first);
             region != m_gpu_only_regions.end() && region->first <= last;) {
            if (region->first + region->second.size - 1 > last) {
                ++region;
                continue;
            }
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().ReleaseBacking(
                region->second.cpu_address, region->second.size);
            if (!done) {
                done = std::make_shared<std::atomic<bool>>(false);
                system.GPU().RunInOrder([done] { done->store(true, std::memory_order_release); });
            }
            m_released_regions.push_back({std::move(region->second), done});
            region = m_gpu_only_regions.erase(region);
        }
    }

    // Needs m_region_mutex.
    std::optional<u64> BackGpuOnlyRegion(u64 gpu_address, u64 size) {
        auto backing = AllocateGpuOnlyBacking(size, true, false);
        if (!backing) {
            return std::nullopt;
        }
        const u64 address = backing->address;
        RegisterGpuOnlyBacking(gpu_address, std::move(*backing));
        return address;
    }

    // Runs before a GPU-side write that may land in a sparse region (Tegra::Nx2BackSparseWrite,
    // usually on the GPU thread): the blocks the write touches get memory, cleared, behind the
    // region's device pages. The GPU mapping stays as it is, so the images over the region keep
    // their storage and only see the write.
    void BackSparseWrite(u64 gpu_address, u64 size, const u8* data) {
        const auto call_begin = std::chrono::steady_clock::now();
        struct Pending {
            u64 region;
            u64 block;
            u64 write_bytes; ///< The part of this write in the block
            bool zeros;      ///< That part is all zero
            bool gpu_only;
        };
        std::vector<Pending> todo;
        const u64 end = gpu_address + size;
        // Calls fn(region_start, region, first_block, last_block) for each region the write touches.
        auto for_regions = [&](auto&& fn) {
            auto it = m_sparse_regions.upper_bound(gpu_address);
            if (it != m_sparse_regions.begin()) {
                --it;
            }
            for (; it != m_sparse_regions.end() && it->first < end; ++it) {
                auto& [start, region] = *it;
                if (start + region.size <= gpu_address) {
                    continue;
                }
                fn(start, region, ((std::max)(gpu_address, start) - start) / SparseBlockSize,
                   ((std::min)(end, start + region.size) - 1 - start) / SparseBlockSize);
            }
        };
        bool waits_for_other = false;
        u64 zero_bytes = 0;
        u64 gpu_only_zero_bytes = 0;
        {
            std::unique_lock lk{m_region_mutex};
            for_regions([&](u64 start, SparseRegion& region, u64 first, u64 last) {
                for (u64 block = first; block <= last; ++block) {
                    if (region.blocks[block] == BlockEmpty && region.tiles) {
                        // The write lands in the game's own memory (a tile it mapped) or nowhere;
                        // the texture cache only learns that the block has contents now.
                        region.blocks[block] = BlockWritten;
                        Tegra::Nx2SparseBlockWritten(start, block);
                    } else if (region.blocks[block] == BlockEmpty) {
                        // Zeros over a block nobody wrote change nothing: it reads zero without
                        // memory. Title 1 clears ~1 GB of them at boot. The texture cache's
                        // block state only knows sparse regions: an empty block of a GPU-only
                        // region has never had memory. Single words (MemoryManager::Write)
                        // still back it there, as that write asserts on a page without memory.
                        const u64 block_begin = start + block * SparseBlockSize;
                        const u64 write_begin = (std::max)(gpu_address, block_begin);
                        const u64 write_end = (std::min)(
                            {end, block_begin + SparseBlockSize, start + region.size});
                        const bool zeros =
                            data != nullptr &&
                            IsAllZero(data + (write_begin - gpu_address), write_end - write_begin);
                        if (zeros && (region.gpu_only
                                          ? GpuOnlyZeroSkip() && size > sizeof(u64)
                                          : !Tegra::Nx2SparseBlockIsWritten(start, block))) {
                            (region.gpu_only ? gpu_only_zero_bytes : zero_bytes) +=
                                write_end - write_begin;
                            continue;
                        }
                        region.blocks[block] = BlockPending;
                        todo.push_back({start, block, write_end - write_begin, zeros,
                                        region.gpu_only});
                    } else if (region.blocks[block] == BlockPending) {
                        waits_for_other = true;
                    }
                }
            });
        }
        if (zero_bytes != 0) {
            std::scoped_lock lk{m_region_mutex};
            const u64 before = m_sparse_zero_bytes;
            m_sparse_zero_bytes += zero_bytes;
            if (before == 0 || (before >> 28) != (m_sparse_zero_bytes >> 28)) {
                LOG_INFO(Service, "nx2:gpu {} MiB of zeros over unwritten sparse blocks left out "
                                  "(they read zero without memory)",
                         m_sparse_zero_bytes >> 20);
            }
        }
        if (gpu_only_zero_bytes != 0) {
            std::scoped_lock lk{m_region_mutex};
            const u64 before = m_gpu_only_zero_bytes;
            m_gpu_only_zero_bytes += gpu_only_zero_bytes;
            if (before == 0 || (before >> 28) != (m_gpu_only_zero_bytes >> 28)) {
                LOG_INFO(Service, "nx2:gpu {} MiB of zeros over unwritten GPU-only blocks left out",
                         m_gpu_only_zero_bytes >> 20);
            }
        }
        // Allocating and mapping happens outside the lock (see AllocateGpuOnlyBacking).
        for (const Pending& pending : todo) {
            const u64 start = pending.region;
            const u64 block = pending.block;
            u64 block_size = SparseBlockSize;
            {
                std::scoped_lock lk{m_region_mutex};
                const auto it = m_sparse_regions.find(start);
                if (it != m_sparse_regions.end()) {
                    block_size = (std::min)(SparseBlockSize, it->second.size - block * SparseBlockSize);
                }
            }
            auto backing = AllocateGpuOnlyBacking(block_size, false, true);
            bool stale = false;
            {
                std::scoped_lock lk{m_region_mutex};
                const auto it = m_sparse_regions.find(start);
                if (it == m_sparse_regions.end() || block >= it->second.blocks.size()) {
                    stale = true; // the region was destroyed while its block was being backed
                } else if (it->second.tiles) {
                    // The game started mapping its tiles there meanwhile (216): no block.
                    stale = true;
                    it->second.blocks[block] = BlockWritten;
                    Tegra::Nx2SparseBlockWritten(start, block);
                } else if (!backing) {
                    auto& region = it->second;
                    region.blocks[block] = BlockFailed; // out of memory: not again for every write
                    if (!region.warned) {
                        region.warned = true;
                        LOG_WARNING(Service, "nx2:gpu sparse region at {:#x}: pool exhausted, GPU writes "
                                             "to its unbacked blocks are dropped",
                                    start);
                    }
                } else {
                    const u64 block_address = start + block * SparseBlockSize;
                    const u64 address = backing->address;
                    it->second.blocks[block] = BlockBacked;
                    RegisterGpuOnlyBacking(block_address, std::move(*backing));
                    Nvidia::Devices::Nx2GpuMemoryRegistry::Get().BackSparse(block_address, address,
                                                                            block_size);
                    Tegra::Nx2SparseBlockWritten(start, block);
                    m_sparse_backed_bytes += block_size;
                    if (++m_sparse_blocks == 1 || (m_sparse_blocks % 64) == 0) {
                        LOG_INFO(Service, "nx2:gpu {} sparse blocks backed ({} MiB); {} MiB of GPU-only "
                                          "memory in use, peak {} MiB",
                                 m_sparse_blocks, m_sparse_backed_bytes >> 20, m_gpu_only_bytes >> 20,
                                 m_gpu_only_peak >> 20);
                    }
                    auto& backed = pending.gpu_only ? m_write_stats.gpu_only : m_write_stats.sparse;
                    ++backed.blocks;
                    backed.write_bytes += pending.write_bytes;
                    backed.zero_bytes += pending.zeros ? pending.write_bytes : 0;
                    if (!it->second.first_block_logged) {
                        it->second.first_block_logged = true;
                        LOG_INFO(Service,
                                 "nx2:gpu first block of {} region [{:#x}, +{:#x}) backed for a "
                                 "write of {:#x} bytes at {:#x} ({}), {:.1f} ms into the write",
                                 pending.gpu_only ? "GPU-only" : "sparse", start, it->second.size,
                                 size, gpu_address, pending.zeros ? "zeros" : "data",
                                 std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - call_begin)
                                     .count());
                    }
                }
            }
            m_region_cv.notify_all();
            if (stale && backing) {
                // Never handed to the GPU: unmap and free right away.
                if (auto* const process = system.ApplicationProcess(); process != nullptr) {
                    process->GetPageTable().UnmapPageGroup(Common::ProcessAddress{backing->address},
                                                           *backing->pages,
                                                           Kernel::KMemoryState::IoMemory);
                }
                backing->pages->Close(system.Kernel());
            }
        }
        if (waits_for_other) {
            // Another thread is backing a block this write needs: wait until it is done.
            std::unique_lock lk{m_region_mutex};
            m_region_cv.wait(lk, [&] {
                bool pending = false;
                for_regions([&](u64, SparseRegion& region, u64 first, u64 last) {
                    for (u64 block = first; block <= last; ++block) {
                        pending |= region.blocks[block] == BlockPending;
                    }
                });
                return !pending;
            });
        }
        CountSparseWrite(std::chrono::steady_clock::now() - call_begin);
    }

    // BackSparseWrite's calls and the blocks they backed, logged by the first call 10 s after the
    // last line, over the time since, when blocks got memory or the calls took 50 ms or more.
    void CountSparseWrite(std::chrono::steady_clock::duration elapsed) {
        std::scoped_lock lk{m_region_mutex};
        auto& stats = m_write_stats;
        const auto now = std::chrono::steady_clock::now();
        if (stats.window == std::chrono::steady_clock::time_point{}) {
            stats.window = now;
        }
        ++stats.calls;
        stats.time += elapsed;
        if (now - stats.window < std::chrono::seconds{10}) {
            return;
        }
        if (stats.gpu_only.blocks + stats.sparse.blocks != 0 ||
            stats.time >= std::chrono::milliseconds{50}) {
            LOG_INFO(Service,
                     "nx2:gpu sparse writes in the last {:.1f} s: {} calls, {:.1f} ms; GPU-only "
                     "regions {} blocks backed, {} MiB written into them ({} MiB zeros); sparse "
                     "regions {} blocks, {} MiB ({} MiB zeros)",
                     std::chrono::duration<double>(now - stats.window).count(), stats.calls,
                     std::chrono::duration<double, std::milli>(stats.time).count(),
                     stats.gpu_only.blocks, stats.gpu_only.write_bytes >> 20,
                     stats.gpu_only.zero_bytes >> 20, stats.sparse.blocks,
                     stats.sparse.write_bytes >> 20, stats.sparse.zero_bytes >> 20);
        }
        stats = {};
        stats.window = now;
    }

    // EDEN_NX2_SPARSE_LAZY=0 backs sparse regions whole, as before.
    static bool LazySparseRegions() {
        static const bool lazy = [] {
            const char* value = std::getenv("EDEN_NX2_SPARSE_LAZY");
            return value == nullptr || value[0] != '0';
        }();
        return lazy;
    }

    // EDEN_NX2_GPU_ONLY_LAZY=0 backs GPU-only regions whole, as before.
    static bool LazyGpuOnlyRegions() {
        static const bool lazy = [] {
            const char* value = std::getenv("EDEN_NX2_GPU_ONLY_LAZY");
            return value == nullptr || value[0] != '0';
        }();
        return lazy;
    }

    // EDEN_NX2_GPU_ONLY_ZERO_SKIP=1 opts into leaving GPU-only zero writes unbacked.
    static bool GpuOnlyZeroSkip() {
        static const bool skip = [] {
            const char* value = std::getenv("EDEN_NX2_GPU_ONLY_ZERO_SKIP");
            return value != nullptr && value[0] == '1';
        }();
        return skip;
    }

    static constexpr u32 SparseRegionFlag = 0x200; ///< Word 7 of 214 (0x60c arrays, 0xa0c pools)
    static constexpr u64 SparseBlockSize = 4ULL << 20;
    static constexpr u8 BlockEmpty = 0;
    static constexpr u8 BlockBacked = 1;
    static constexpr u8 BlockFailed = 2;
    static constexpr u8 BlockPending = 3; ///< Being allocated and mapped by BackSparseWrite
    static constexpr u8 BlockWritten = 4; ///< Written in a region the game maps tiles into
    struct SparseRegion {
        bool gpu_only{};        ///< A GPU-only region (no sparse flag), backed lazily as well
        u64 size;
        std::vector<u8> blocks; ///< BlockEmpty / Backed / Failed / Pending / Written per block
        bool warned{};          ///< Logged that the pool ran out for its blocks
        bool tiles{};           ///< The game maps its own memory into it (216): no blocks
        std::vector<bool> tile_pages; ///< Pages a tile maps, once `tiles`
        bool first_block_logged{};
    };
    std::map<u64, SparseRegion> m_sparse_regions; ///< By GPU address
    u64 m_sparse_blocks{};       ///< Sparse blocks backed so far
    u64 m_sparse_backed_bytes{}; ///< Sparse region memory given out so far
    u64 m_sparse_zero_bytes{};   ///< Zeros written over unwritten sparse blocks, left out
    u64 m_gpu_only_zero_bytes{}; ///< The same over unwritten blocks of GPU-only regions
    u64 m_sparse_released_bytes{}; ///< Blocks given back once the game mapped tiles there
    struct SparseWriteStats {
        struct Backed {
            u64 blocks{};
            u64 write_bytes{}; ///< Of the writes that backed them, the part in those blocks
            u64 zero_bytes{};  ///< Of that, the parts that were all zero
        };
        std::chrono::steady_clock::time_point window{};
        std::chrono::steady_clock::duration time{};
        u64 calls{};
        Backed gpu_only{};
        Backed sparse{};
    } m_write_stats{}; ///< BackSparseWrite in the current 10 s, with m_region_mutex

    // nn::gpu::sdk::SparseMapRequest (216), filled from NvRm's nvgpu_as_remap_op.
    struct SparseMapRequest {
        u64 gpu_address;
        u64 size;
        u8 no_write;
        u8 cacheable;
        u8 unknown_12;
        u8 kind;          ///< Wanted: the compressible kind if there is one
        u8 fallback_kind; ///< The incompressible kind
        u8 unknown_15;    ///< Remap op flag bit 15
        u16 padding_16;
        u32 memory_handle; ///< Sub 101's handle; 0 unmaps the range
        u32 padding_1c;
        u64 offset; ///< Into that memory
    };
    static_assert(sizeof(SparseMapRequest) == 40);
    u64 m_tile_calls{};
    u64 m_tile_requests{};
    std::chrono::steady_clock::time_point m_tile_log_time{};
    u64 m_tile_maps{};
    u64 m_tile_unmaps{};
    u64 m_tile_kept{}; ///< Maps into GPU-only regions that keep their own memory, left out
    u64 m_tile_bytes{}; ///< Sparse region pages a tile maps right now

    void ApplySparseMapRequest(const SparseMapRequest& request) {
        ++m_tile_requests;
        if (request.size == 0) {
            return;
        }
        const u64 gpu_address = request.gpu_address;
        const u64 size = request.size;
        if (size > std::numeric_limits<u64>::max() - gpu_address) {
            LOG_WARNING(Service, "nx2:gpu 216 [{:#x}, +{:#x}] wraps the address space", gpu_address,
                        size);
            return;
        }
        auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
        auto it = m_sparse_regions.upper_bound(gpu_address);
        SparseRegion* region = nullptr;
        u64 start = 0;
        if (it != m_sparse_regions.begin()) {
            --it;
            if (size <= it->second.size && gpu_address - it->first <= it->second.size - size) {
                start = it->first;
                region = &it->second;
            }
        }
        if (const auto gpu_only = GpuOnlyRegionOf(gpu_address, size)) {
            ApplyGpuOnlyRequest(*gpu_only, request);
            return;
        }
        if (request.memory_handle == 0) {
            ++m_tile_unmaps;
            LOG_DEBUG(Service, "nx2:gpu 216 unmap [{:#x}, +{:#x}]", gpu_address, size);
            if (region != nullptr) {
                Tegra::Nx2SparseTilesMapped(start, gpu_address - start, size, false);
                registry.MapSparseTile(gpu_address, 0, size);
                MarkTiles(*region, gpu_address - start, size, false);
            } else {
                registry.Unmap(gpu_address, size);
            }
            return;
        }
        ++m_tile_maps;
        const auto memory = m_memory.find(request.memory_handle);
        if (memory == m_memory.end() || request.offset > memory->second.size || size > memory->second.size - request.offset) {
            LOG_WARNING(Service, "nx2:gpu 216 map [{:#x}, +{:#x}] of unknown memory {} offset {:#x}",
                        gpu_address, size, request.memory_handle, request.offset);
            return;
        }
        const u64 cpu_address = memory->second.address + request.offset;
        LOG_DEBUG(Service, "nx2:gpu 216 map [{:#x}, +{:#x}] -> memory {} offset {:#x} kind {:#x}",
                  gpu_address, size, request.memory_handle, request.offset, request.kind);
        if (region == nullptr) {
            registry.MapAt(gpu_address, cpu_address, size);
            return;
        }
        if (!region->tiles) {
            StartTiles(start, *region);
        }
        // The game's memory already holds what the tile shows (title 1 fills nine world-sized
        // masks, R8/RG16/R16 at 8192-16384 texels, in its own memory before mapping them): the
        // blocks count as written, so the texture cache gives the images over them memory and
        // loads them. Left unwritten, those images read zero and the static world was unlit.
        const u64 first_block = (gpu_address - start) / SparseBlockSize;
        const u64 last_block = (gpu_address + size - 1 - start) / SparseBlockSize;
        for (u64 block = first_block; block <= last_block; ++block) {
            if (region->blocks[block] == BlockEmpty) {
                region->blocks[block] = BlockWritten;
            }
            Tegra::Nx2SparseBlockWritten(start, block);
        }
        Tegra::Nx2SparseTilesMapped(start, gpu_address - start, size, true);
        registry.MapSparseTile(gpu_address, cpu_address, size);
        MarkTiles(*region, gpu_address - start, size, true);
    }

    // Start of the region 214 made without the sparse flag that holds the range (by its flags,
    // not by how it is backed).
    std::optional<u64> GpuOnlyRegionOf(u64 gpu_address, u64 size) const {
        auto it = m_gpu_only_ranges.upper_bound(gpu_address);
        if (it == m_gpu_only_ranges.begin()) {
            return std::nullopt;
        }
        --it;
        if (size > it->second || gpu_address - it->first > it->second - size) {
            return std::nullopt;
        }
        return it->first;
    }

    // 216 into a GPU-only region: render targets, and with remap support also some texture arrays
    // (title 1: 5.8 GB and 1.4 GB). The game backs those with its own memory too, which it fills
    // through its own address, so a map points the range at that memory the way 211 does: both
    // addresses reach the same device pages and the texture cache sees the writes. (Re-pointing the
    // region's own device pages instead, as for sparse tiles, left the arrays stale: black terrain.)
    // Left alone:
    // - Regions whose memory the game shares with another GPU-only region (render targets): the
    //   texture cache would flush and re-upload such overlapping render targets every frame. All of
    //   a group keep the memory 214 gave them.
    // - Regions 214 backs as they are written: the texture cache reads an image's memory in full
    //   when it uploads it, which makes the game's pages behind it resident on the host, while
    //   unwritten blocks read as zero for free (title 1: ~1 GB less resident at 150 s).
    void ApplyGpuOnlyRequest(u64 region, const SparseMapRequest& request) {
        auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
        const u64 gpu_address = request.gpu_address;
        const u64 size = request.size;
        if (request.memory_handle == 0) {
            ++m_tile_unmaps;
            DropGpuOnlyTiles(gpu_address, size, true);
            return;
        }
        ++m_tile_maps;
        if (m_sparse_regions.contains(region)) {
            ++m_tile_kept;
            return;
        }
        if (m_shared_regions.contains(region)) {
            m_shared_memory[{request.memory_handle, request.offset}] = region;
            ++m_tile_kept;
            return;
        }
        const auto memory = m_memory.find(request.memory_handle);
        if (memory == m_memory.end() || request.offset > memory->second.size || size > memory->second.size - request.offset) {
            LOG_WARNING(Service, "nx2:gpu 216 map [{:#x}, +{:#x}] of unknown memory {} offset {:#x}",
                        gpu_address, size, request.memory_handle, request.offset);
            return;
        }
        if (const auto other = RegionSharing(request.memory_handle, request.offset, size, region)) {
            ++m_tile_kept;
            for (const u64 shared : {region, *other}) {
                if (m_shared_regions.insert(shared).second) {
                    const auto range = m_gpu_only_ranges.find(shared);
                    if (range != m_gpu_only_ranges.end()) {
                        DropGpuOnlyTiles(shared, range->second, true);
                    }
                    LOG_INFO(Service, "nx2:gpu GPU-only region at {:#x} shares memory with another: "
                                      "it keeps its own",
                             shared);
                }
            }
            return;
        }
        DropGpuOnlyTiles(gpu_address, size, false);
        registry.MapAt(gpu_address, memory->second.address + request.offset, size);
        m_gpu_only_tiles[gpu_address] = {size, region, request.memory_handle, request.offset};
        m_gpu_only_memory[{request.memory_handle, request.offset}] = {request.offset + size, region};
        m_gpu_only_tile_max = (std::max)(m_gpu_only_tile_max, size);
        m_gpu_only_tile_bytes += size;
    }

    // Another GPU-only region whose tiles hold any of [offset, +size) of the memory, if one does.
    std::optional<u64> RegionSharing(u32 handle, u64 offset, u64 size, u64 region) const {
        auto it = m_gpu_only_memory.lower_bound({handle, offset + size});
        while (it != m_gpu_only_memory.begin()) {
            --it;
            const auto& [key, value] = *it;
            const auto& [end, owner] = value;
            if (key.first != handle || key.second + m_gpu_only_tile_max <= offset) {
                break;
            }
            if (end > offset && owner != region) {
                return owner;
            }
        }
        // Regions that already keep their own memory record no tiles: compare with their requests.
        if (const auto kept = m_shared_memory.find({handle, offset}); kept != m_shared_memory.end() &&
                                                                     kept->second != region) {
            return kept->second;
        }
        return std::nullopt;
    }

    // The memory 214 gave a GPU-only region at [gpu_address, +size), when it was backed whole.
    std::optional<u64> OwnBackingOf(u64 gpu_address, u64 size) const {
        auto it = m_gpu_only_regions.upper_bound(gpu_address);
        if (it == m_gpu_only_regions.begin()) {
            return std::nullopt;
        }
        --it;
        if (size > it->second.size || gpu_address - it->first > it->second.size - size) {
            return std::nullopt;
        }
        return it->second.cpu_address + (gpu_address - it->first);
    }

    // Forgets the game's tiles in [gpu_address, +size) of GPU-only regions; with `restore` (an
    // unmap) those ranges show the region's own memory again, or nothing where it has none.
    void DropGpuOnlyTiles(u64 gpu_address, u64 size, bool restore) {
        auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
        auto it = m_gpu_only_tiles.upper_bound(gpu_address);
        if (it != m_gpu_only_tiles.begin()) {
            const auto previous = std::prev(it);
            if (previous->first + previous->second.size > gpu_address) {
                it = previous;
            }
        }
        while (it != m_gpu_only_tiles.end() && it->first < gpu_address + size) {
            const auto& [tile_address, tile] = *it;
            if (restore) {
                if (const auto backing = OwnBackingOf(tile_address, tile.size)) {
                    registry.MapAt(tile_address, *backing, tile.size);
                } else {
                    registry.Unmap(tile_address, tile.size);
                }
            }
            if (const auto owner = m_gpu_only_memory.find({tile.memory_handle, tile.offset});
                owner != m_gpu_only_memory.end() && owner->second.second == tile.region) {
                if (m_shared_regions.contains(tile.region)) {
                    m_shared_memory[owner->first] = tile.region;
                }
                m_gpu_only_memory.erase(owner);
            }
            m_gpu_only_tile_bytes -= tile.size;
            it = m_gpu_only_tiles.erase(it);
        }
    }

    // The game maps tiles into this region itself (216): writes outside its tiles are dropped, as
    // on the console, so it gets no more blocks, and the blocks it got before (written while no
    // tile was mapped, which the console would have dropped) go back.
    void StartTiles(u64 start, SparseRegion& region) {
        using Core::Memory::YUZU_PAGESIZE;
        region.tiles = true;
        region.tile_pages.assign(Common::DivCeil(region.size, YUZU_PAGESIZE), false);
        std::shared_ptr<std::atomic<bool>> done;
        u64 released = 0;
        for (u64 block = 0; block < region.blocks.size(); ++block) {
            if (region.blocks[block] != BlockBacked) {
                continue;
            }
            region.blocks[block] = BlockWritten;
            const u64 block_address = start + block * SparseBlockSize;
            const auto backing = m_gpu_only_regions.find(block_address);
            if (backing == m_gpu_only_regions.end()) {
                continue;
            }
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapSparseTile(block_address, 0,
                                                                        backing->second.size);
            if (!done) {
                done = std::make_shared<std::atomic<bool>>(false);
                system.GPU().RunInOrder([done] { done->store(true, std::memory_order_release); });
            }
            released += backing->second.size;
            m_released_regions.push_back({std::move(backing->second), done});
            m_gpu_only_regions.erase(backing);
        }
        m_sparse_released_bytes += released;
        LOG_INFO(Service, "nx2:gpu sparse region at {:#x}: the game maps its tiles, {} MiB of blocks "
                          "released",
                 start, released >> 20);
    }

    // Counts the bytes of tiles mapped, page by page.
    void MarkTiles(SparseRegion& region, u64 offset, u64 size, bool mapped) {
        using Core::Memory::YUZU_PAGESIZE;
        if (!region.tiles) {
            return;
        }
        const u64 last = (std::min)(Common::DivCeil(offset + size, YUZU_PAGESIZE),
                                    static_cast<u64>(region.tile_pages.size()));
        for (u64 page = offset / YUZU_PAGESIZE; page < last; ++page) {
            if (region.tile_pages[page] != mapped) {
                region.tile_pages[page] = mapped;
                if (mapped) {
                    m_tile_bytes += YUZU_PAGESIZE;
                } else {
                    m_tile_bytes -= YUZU_PAGESIZE;
                }
            }
        }
    }

    u64 m_gpu_only_bytes{};      ///< Backing of GPU-only regions and sparse blocks right now
    u64 m_gpu_only_peak{};
    std::atomic<u64> m_alias_hint{}; ///< 2 MiB slot after the last backing in the alias code region
    /// Regions are created and destroyed on the service thread and backed on the GPU thread.
    std::recursive_mutex m_region_mutex;
    std::condition_variable_any m_region_cv; ///< Signalled when a pending sparse block resolves

    struct GpuOnlyRegion {
        u64 cpu_address;
        u64 size;
        std::unique_ptr<Kernel::KPageGroup> pages;
    };
    struct ReleasedRegion {
        GpuOnlyRegion region;
        std::shared_ptr<std::atomic<bool>> done; ///< Set by the GPU thread past the region's last use
        u32 unmap_attempts{};
    };
    static constexpr u32 MaxUnmapAttempts = 16; ///< Then its pages stay mapped for good
    std::map<u64, GpuOnlyRegion> m_gpu_only_regions; ///< By GPU address
    std::map<u64, u64> m_gpu_only_ranges; ///< 214 regions without the sparse flag: GPU address -> size
    struct GpuOnlyTile {
        u64 size;
        u64 region; ///< Start of its GPU-only region
        u32 memory_handle;
        u64 offset;
    };
    std::map<u64, GpuOnlyTile> m_gpu_only_tiles; ///< The game's memory 216 put in GPU-only regions
    /// (memory handle, offset) -> (end offset, region) of those tiles, to find shared memory
    std::map<std::pair<u32, u64>, std::pair<u64, u64>> m_gpu_only_memory;
    /// Memory mapped into regions that keep their own (handle, offset) -> region
    std::map<std::pair<u32, u64>, u64> m_shared_memory;
    std::set<u64> m_shared_regions; ///< GPU-only regions sharing memory: they keep their own
    u64 m_gpu_only_tile_max{};   ///< Largest such tile, bounds the search
    u64 m_gpu_only_tile_bytes{}; ///< GPU-only region memory the game maps right now
    std::vector<ReleasedRegion> m_released_regions;  ///< Service thread only

    // Outlives the session (jobs queued on the GPU thread hold a reference): they signal under
    // `mutex` and only while `alive`.
    struct SharedState {
        std::mutex mutex;
        bool alive{true};
    };
    std::shared_ptr<SharedState> m_shared = std::make_shared<SharedState>();

    struct GpuMemory {
        u64 address;
        u64 size;
    };

    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*, NumIds> m_events{};
    std::array<bool, 8> m_graphics_devices{}; ///< Ids handed out by CreateGraphicsDevice
    std::map<u32, GpuMemory> m_memory;
    u32 m_next_memory_handle{1};

    // Every 30 s: the backing of sparse blocks and GPU-only regions is marked cold. The game
    // writes it once (texture uploads by 2D blits: title 1 fills 2-4 GB of texture pools) and it
    // is read again only when the texture cache builds an image over it, so the OS may compress or
    // page it out before anything else. Last member: stops before the regions are destroyed.
    void HintColdBacking() {
        struct Range {
            u64 physical_offset;
            u64 virtual_address;
            u64 size;
        };
        std::vector<Range> ranges;
        {
            std::scoped_lock lk{m_region_mutex};
            for (const auto& [gpu_address, region] : m_gpu_only_regions) {
                u64 virtual_address = region.cpu_address;
                for (const auto& block : *region.pages) {
                    ranges.push_back({GetInteger(block.GetAddress()) - Core::DramMemoryMap::Base,
                                      virtual_address, block.GetSize()});
                    virtual_address += block.GetSize();
                }
            }
        }
        auto& buffer = system.DeviceMemory().buffer;
        for (const Range& range : ranges) {
            buffer.HintCold(range.physical_offset, range.size, range.virtual_address);
        }
    }
    std::jthread m_cold_hint_thread{[this](std::stop_token stop) {
        Common::SetCurrentThreadName("nx2:ColdHint");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        while (!stop.stop_requested()) {
            for (int i = 0; i < 30 && !stop.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds{1});
            }
            if (!stop.stop_requested()) {
                HintColdBacking();
            }
        }
    }};
};

// Native memory and images handed out by the Switch 2 graphics server (gsv 111/114/423/429/315),
// shared with the vi2 session so presents can find an image's memory. Command meanings follow
// the shared changes.patch (IGraphicsSupervisor), re-checked against title 4's SDK.
struct Nx2NativeObjects {
    struct Memory {
        u64 size{};
        u64 address{}; // CPU (== GPU) address once bound by 114/423
    };
    struct Image {
        u32 memory{};
        std::array<u32, 16> preference{};
        u64 size{};
        u32 count{1}; // images in the pool; handle id + i is image i
        u64 offset{}; // into its memory (images of a decoder pool share one)
        bool window_pool{};
        bool decoder{}; // a video decoder output image (gsv 911)
        bool gpu_mapped{}; // mapped for the GPU with gsv 426: sampled block-linear
        u32 gpu_maps{};     // outstanding 426 maps of a decoder image
        bool mapped_here{}; // 426 added the decoder image's GPU mapping (none covered it)
    };
    std::mutex mutex;
    std::map<u32, Memory> memories;
    std::map<u32, Image> images;
    u32 next_memory{1};
    u32 next_image{1};
};

// Block-linear layout of an InterModuleUserImagePreference (16 words): word 2 holds
// (width - 1) | (height - 1) << 16, word 4 the block height (log2 of GOB rows), word 5 the row
// pitch in 64-byte GOBs. Formula from changes.patch (native_image_layout.h); preferences outside
// it fall back to 4 bytes per pixel and 128-row blocks.
struct Nx2ImageLayout {
    u32 width;
    u32 height;
    u64 pitch;
    u32 block_height_log2;
    u64 size;
    u64 alignment;
};

inline Nx2ImageLayout Nx2CalculateImageLayout(const u32* preference) {
    Nx2ImageLayout layout{};
    layout.width = (preference[2] & 0xFFFF) + 1;
    layout.height = (preference[2] >> 16) + 1;
    const u64 pitch_gobs = preference[5];
    const u32 block_height_log2 = preference[4];
    if (block_height_log2 <= 8 && pitch_gobs >= (u64{layout.width} * 4 + 63) / 64 &&
        pitch_gobs <= 4096) {
        layout.pitch = pitch_gobs * 64;
        layout.block_height_log2 = block_height_log2;
    } else {
        layout.pitch = Common::AlignUp(u64{layout.width} * 4, 64);
        layout.block_height_log2 = 7;
    }
    const u64 block_rows = u64{1} << layout.block_height_log2;
    layout.size = layout.pitch * Common::AlignUp(u64{layout.height}, block_rows);
    layout.alignment = std::max<u64>(64 * block_rows, 0x1000);
    return layout;
}

// GameChat's graphics side (nn::chat::gsv), handed out by gsv 1202. Read from the SDK's
// nn::chat::gsv::Initialize: TIPC 1() returns a u32 id at word 3 (it aborts with 2162-0001 if
// the id is over 0x3ff), then TIPC 11(u32 id) returns an event (copy handle at word 3, result at
// word 4) that it attaches as a system event. Everything else falls through to the nx2 auto stub.
class INx2ChatGsvSession final : public ServiceFramework<INx2ChatGsvSession> {
public:
    explicit INx2ChatGsvSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:chat:gsv"}, m_context{system_, "nx2:chat:gsv"} {
        RegisterHandlersTipc({
            {1, &INx2ChatGsvSession::GetEventId, "GetEventId (guessed)"},
            {11, &INx2ChatGsvSession::GetEvent, "GetEvent (guessed)"},
        });
    }

    ~INx2ChatGsvSession() override {
        for (auto& [id, event] : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    void GetEventId(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:chat:gsv TIPC 1, returning id 0");
        TipcReplyWords(ctx, {0});
    }

    void GetEvent(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:chat:gsv TIPC 11 id={}, returning an event", id);
        auto*& event = m_events[id];
        if (event == nullptr) {
            event = m_context.CreateEvent(fmt::format("nx2:chat:gsv:Event{}", id));
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    KernelHelpers::ServiceContext m_context;
    std::map<u32, Kernel::KEvent*> m_events;
};

// Video decoder output images (nn::codec image pools, gsv 991/911/961). The preference
// (MakeDecodeImagePreference) holds an output layout code at word 1 and (width - 1) |
// (height - 1) << 16 at word 2. Codes come in pairs, one per CodecMode: 1/4 NV12 (format 3, what
// the movie players use), 2/5 BGRA, 3/6 RGBA; the HDR codes (10-13) are written as RGBA. The
// server picks the layout and describes it to the client with gsv 961: here pitch-linear rows,
// NV12's half-height chroma plane below the luma plane, each plane padded to 128 rows.
struct Nx2DecoderImageLayout {
    u32 code;
    u32 width;
    u32 height;
    Tegra::Host1x::Nx2PictureFormat format;
    u64 pitch;
    u64 chroma_offset;
    u64 size;
};

inline Nx2DecoderImageLayout Nx2CalculateDecoderImageLayout(const u32* preference) {
    using Tegra::Host1x::Nx2PictureFormat;
    Nx2DecoderImageLayout layout{};
    layout.code = preference[1];
    layout.width = (preference[2] & 0xFFFF) + 1;
    layout.height = (preference[2] >> 16) + 1;
    switch (layout.code) {
    case 1:
    case 4:
        layout.format = Nx2PictureFormat::Nv12;
        break;
    case 2:
    case 5:
        layout.format = Nx2PictureFormat::Bgra8;
        break;
    default:
        layout.format = Nx2PictureFormat::Rgba8;
        break;
    }
    const bool nv12 = layout.format == Nx2PictureFormat::Nv12;
    constexpr u64 BlockRows = 128;
    layout.pitch = Common::AlignUp(u64{layout.width} * (nv12 ? 1 : 4), 64);
    layout.chroma_offset = layout.pitch * Common::AlignUp(u64{layout.height}, BlockRows);
    layout.size = layout.chroma_offset +
                  (nv12 ? layout.pitch * Common::AlignUp((u64{layout.height} + 1) / 2, BlockRows) : 0);
    return layout;
}

// Placeholder for the Switch 2 display session (nn::vi2::sdk), handed out by gsv 305. Reply
// shapes read from the SDK: TIPC 1 -> u32 < 0x400, TIPC 3 -> u32 < 8, TIPC 11/31(u32) and
// 32(handle, u32) -> one handle at word 3 then the result. Handles returned here are events (a
// guess); everything else is auto-stubbed through the nx2: prefix.
//
// EDEN_NX2_VI2_CLIENT_STATE=0 leaves vi2 13/21/51/101/102/111/112/201/202/212/251/252 and gsv 115
// to the auto stub again, and 11 to an event that is never signalled.
inline bool Nx2Vi2ClientState() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_VI2_CLIENT_STATE");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

class INx2Vi2Session final : public ServiceFramework<INx2Vi2Session> {
public:
    explicit INx2Vi2Session(Core::System& system_, std::shared_ptr<Nx2NativeObjects> objects_)
        : ServiceFramework{system_, "nx2:vi2"}, m_context{system_, "nx2:vi2"},
          m_objects{std::move(objects_)} {
        if (Nx2Vi2ClientState()) {
            RegisterHandlersTipc({
                {11, &INx2Vi2Session::GetNotificationEvent, "GetNotificationEvent"},
                {13, &INx2Vi2Session::SucceedQuietly, "ReleaseNotificationEvent"},
                {21, &INx2Vi2Session::GetNotifications, "GetNotifications"},
                // Display/client setup: nothing to track while presents composite directly.
                {51, &INx2Vi2Session::SucceedQuietly, "ConfigureRequestGroups"},
                {101, &INx2Vi2Session::OpenDisplay, "OpenDisplay"},
                {102, &INx2Vi2Session::CloseDisplay, "CloseDisplay"},
                {111, &INx2Vi2Session::SucceedQuietly, "OpenEndpoint"},
                {112, &INx2Vi2Session::SucceedQuietly, "CloseEndpoint"},
                {201, &INx2Vi2Session::RegisterClientState, "RegisterClientState"},
                {202, &INx2Vi2Session::UnregisterClientState, "UnregisterClientState"},
                {212, &INx2Vi2Session::GetLayerBufferBits, "GetLayerBufferBits"},
                {251, &INx2Vi2Session::SucceedQuietly, "BindClientDisplay"},
                {252, &INx2Vi2Session::SucceedQuietly, "UnbindClientDisplay"},
            });
        } else {
            RegisterHandlersTipc({
                {11, &INx2Vi2Session::ReturnEvent, "Unknown11"},
            });
        }
        RegisterHandlersTipc({
            {1, &INx2Vi2Session::ReturnOne, "Unknown1"},
            {3, &INx2Vi2Session::ReturnOne, "GetDisplayCount (guessed)"},
            {5, &INx2Vi2Session::GetTimestampRecorderCount, "GetTimestampRecorderCount (guessed)"},
            {31, &INx2Vi2Session::GetVsyncEvent, "GetVsyncEvent (guessed)"},
            {32, &INx2Vi2Session::GetVsyncEvent, "GetDisplayVsyncEvent (guessed)"},
            // Title 1 calls these ~75 times a second with the display name "default"; the
            // auto stub's empty success reply works, but logged two lines per call (a 50 MB log
            // in an 18 minute session).
            {104, &INx2Vi2Session::SucceedQuietly, "Unknown104 (display \"default\")"},
            {105, &INx2Vi2Session::SucceedQuietly, "Unknown105 (display \"default\")"},
            {228, &INx2Vi2Session::CancelClientBuffer, "CancelClientBuffer"},
            {229, &INx2Vi2Session::PresentClientBuffer, "PresentClientBuffer"},
        });
    }

    ~INx2Vi2Session() override {
        // The vsync callback may be running on the HostTiming thread: empty its event list under
        // its lock first (it holds the state, not this session), then close the events.
        {
            std::scoped_lock lk{m_vsync->mutex};
            m_vsync->events.clear();
        }
        if (m_vsync_timer) {
            system.CoreTiming().UnscheduleEvent(m_vsync_timer);
        }
        if (m_release_timer) {
            // No release fires after this: signal the fences still due, so nothing waits on them
            // for good, then hand the syncpoint back.
            system.CoreTiming().UnscheduleEvent(m_release_timer);
            auto& syncpoints = system.Host1x().GetSyncpointManager();
            while (static_cast<s32>(m_release_value - syncpoints.GetGuestSyncpointValue(
                                                          m_release_syncpoint)) > 0) {
                syncpoints.IncrementGuest(m_release_syncpoint);
            }
            m_release_nvdrv->GetContainer().GetSyncpointManager().FreeSyncpoint(
                m_release_syncpoint);
        }
        for (auto& [key, event] : m_events) {
            if (event != nullptr) {
                m_context.CloseEvent(event);
            }
        }
        if (m_notification_event != nullptr) {
            m_context.CloseEvent(m_notification_event);
        }
    }

private:
    // 229(u64 client token, u32 slot, u32 image, ...fences): result, u64 mask of buffer slots the
    // client may acquire next, four zero words. Nothing is shown yet; every slot is handed back
    // at once (changes.patch's default mask is 0x7fff).
    void PresentClientBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        ++m_presents;
        Core::g_nx2_present_count.fetch_add(1, std::memory_order_relaxed);
        if (m_presents <= 4 || (m_presents % 600) == 0) {
            LOG_INFO(Service, "nx2:vi2 229 present #{} token={:#x} slot={} image={}", m_presents,
                     u64{w[2]} | (u64{w[3]} << 32), w[4], w[5]);
        }
        // Word 12 is (swap interval << 16) | refresh rate: how many vblanks the game wants this
        // frame shown for. Title 1 counts its time in vblanks and advances that many each frame.
        // The interval (bits 16-23, as ZendNevs' patch reads it) is clamped to 1-4 and a rate
        // outside 30-240 Hz read as 60.
        const u32 timing = w[12];
        const u32 swap_interval = std::clamp<u32>((timing >> 16) & 0xff, 1, 4);
        const u32 refresh_rate =
            (timing & 0xffff) >= 30 && (timing & 0xffff) <= 240 ? timing & 0xffff : 60;
        if (timing != std::exchange(m_swap_word, timing)) {
            m_swap_stable = 0;
            ++m_stats.swap_changes;
            // A game that picks its own frame rate can change it every few frames: log the first
            // changes, then count them in the 600-present line.
            if (m_swap_changes < 32) {
                ++m_swap_changes;
                LOG_INFO(Service, "nx2:vi2 present #{} word 12 = {:#x} (swap interval {}, {} Hz)",
                         m_presents, timing, (timing >> 16) & 0xff, timing & 0xffff);
            } else {
                LOG_DEBUG(Service, "nx2:vi2 present #{} word 12 = {:#x}", m_presents, timing);
            }
        } else if (m_swap_stable < StableSwapPresents) {
            ++m_swap_stable;
        }
        // Wait for the previous frame's fence instead of this one, so the game prepares frame N+1
        // while the GPU thread runs frame N (the console keeps several frames in flight). Waiting
        // for this frame serialized the two: 8-56 ms of every 34-105 ms frame on title 1.
        // EDEN_NX2_FRAMES_AHEAD=0 restores the wait for the current frame. =auto waits for this
        // frame, so the game doesn't refill command memory the GPU thread hasn't read yet, but
        // gives up after a few ms once a GPU channel was seen suspended on a semaphore acquire
        // (title 1's CPU releases some only after this present returns) and waits for the
        // previous frame instead.
        enum class FramesAhead { Current, Previous, Auto };
        static const std::optional<FramesAhead> frames_ahead_setting =
            []() -> std::optional<FramesAhead> {
            const char* value = std::getenv("EDEN_NX2_FRAMES_AHEAD");
            if (value == nullptr) {
                return std::nullopt;
            }
            if (std::strcmp(value, "auto") == 0) {
                return FramesAhead::Auto;
            }
            return value[0] == '0' ? FramesAhead::Current : FramesAhead::Previous;
        }();
        // Donkey Kong Bananza's channels wait on each other every frame. Running a frame ahead,
        // it rewrote command memory the GPU thread hadn't read yet: misread commands, then a
        // freeze or crash, and rubberbanding while moving (upstream #506). It waits for each of
        // its frames unless EDEN_NX2_FRAMES_AHEAD says otherwise.
        constexpr u64 DonkeyKongBananzaProgramId = 0x040074A01BF12000ULL;
        const FramesAhead frames_ahead =
            frames_ahead_setting ? *frames_ahead_setting
            : system.GetApplicationProcessProgramID() == DonkeyKongBananzaProgramId
                ? FramesAhead::Current
                : FramesAhead::Previous;
        // Running a frame ahead, Ocarina's UI storage is copied at each present (#286).
        const bool ui_snapshot = frames_ahead == FramesAhead::Previous &&
                                 system.GetApplicationProcessProgramID() ==
                                     Common::Nx2::OcarinaOfTimeProgramId;
        Tegra::g_nx2_ui_snapshot_enabled.store(ui_snapshot, std::memory_order_relaxed);
        const auto entered = std::chrono::steady_clock::now();
        if (frames_ahead == FramesAhead::Previous ||
            (frames_ahead == FramesAhead::Auto && !WaitForFrame(~w[22], w[24], true))) {
            LOG_DEBUG(Service, "nx2:vi2 present #{} fence ({}, {:#x}), waiting for ({}, {:#x}); now {:#x}",
                      m_presents, ~w[22], w[24], m_previous_fence_id, m_previous_fence_value,
                      ~w[22] < 192 ? system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(~w[22]) : 0U);
            // Only run a frame ahead while the GPU keeps up. With the previous frame still on the
            // GPU here, the GPU is the limit and running ahead gains nothing; and Splatoon Raiders,
            // which reuses its command memory as if the GPU were at most a frame behind, rewrote
            // command lists Eden had not read yet (misread commands, then crashes, with a slow
            // GPU). Wait for this frame too then. Not under =auto: a suspended channel brought us
            // here, and this frame may not finish before the present returns. Not for Ocarina of
            // Time with its UI copy either: its GPU thread is the limit, and waiting for this frame
            // too kept the two from ever overlapping.
            // EDEN_NX2_BEHIND_WAIT=0 (A/B tests) waits for the previous frame only: once the GPU
            // thread is the limit, the extra wait runs the game's CPU work and the GPU thread's one
            // after the other instead of side by side, every frame.
            static const bool behind_wait = [] {
                const char* value = std::getenv("EDEN_NX2_BEHIND_WAIT");
                return value == nullptr || value[0] != '0';
            }();
            const bool gpu_behind =
                frames_ahead == FramesAhead::Previous && !ui_snapshot && m_previous_fence_id < 192 &&
                !system.Host1x().GetSyncpointManager().IsReadyGuest(m_previous_fence_id,
                                                                    m_previous_fence_value);
            WaitForFrame(m_previous_fence_id, m_previous_fence_value);
            m_stats.gpu_behind += gpu_behind ? 1 : 0;
            if (gpu_behind && behind_wait) {
                WaitForFrame(~w[22], w[24]);
            }
        } else if (frames_ahead == FramesAhead::Current) {
            WaitForFrame(~w[22], w[24]);
        }
        m_previous_fence_id = ~w[22];
        m_previous_fence_value = w[24];
        const auto waited = std::chrono::steady_clock::now();
        if (ui_snapshot) {
            TakeUiSnapshot(~w[22], w[24]);
            m_stats.snapshot_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - waited)
                                       .count();
        }
        Composite(w[5], ~w[22], w[24], w[7]);
        const auto composited = std::chrono::steady_clock::now();
        std::array<u32, 4> release{};
        if (PresentQueue()) {
            release = QueueRelease(swap_interval, refresh_rate);
        } else {
            PaceToVblank(swap_interval, refresh_rate);
        }
        const auto paced = std::chrono::steady_clock::now();
        // Where each frame's time goes on the present thread, averaged per 600 presents: the
        // game is blocked here while Eden's GPU thread catches up (fence) and composites.
        {
            using us = std::chrono::microseconds;
            // long long like the stats: microseconds::rep is long on Linux (std::max needs one type).
            const auto to_us = [](auto d) -> long long {
                return std::chrono::duration_cast<us>(d).count();
            };
            m_stats.fence_us += to_us(waited - entered);
            m_stats.composite_us += to_us(composited - waited);
            m_stats.pace_us += to_us(paced - composited);
            m_stats.max_fence_us = (std::max<long long>)(m_stats.max_fence_us, static_cast<long long>(to_us(waited - entered)));
            if (m_stats.last_present != std::chrono::steady_clock::time_point{}) {
                const long long frame = to_us(entered - m_stats.last_present);
                ++m_stats.frames;
                m_stats.frame_us += frame;
                m_stats.game_us += 1'000'000LL * swap_interval / refresh_rate;
                m_stats.max_frame_us = (std::max)(m_stats.max_frame_us, frame);
                m_stats.hitches += frame > 100'000 ? 1 : 0;
                // The fence wait inside this frame is the previous present's.
                if (frame > 100'000 && m_held_logged < 256) {
                    ++m_held_logged;
                    LOG_INFO(Service, "nx2:vi2 present #{} held {} ms ({} ms on the GPU fence)",
                             m_presents, frame / 1000, m_last_fence_us / 1000);
                }
            }
            m_last_fence_us = to_us(waited - entered);
            m_stats.last_present = entered;
            m_stats.long_presents += swap_interval > 1 ? 1 : 0;
            if (++m_stats.presents == 600) {
                // Game speed: the time the frames asked for (interval / refresh rate each) per
                // real second. It holds for a game that advances by its interval every frame, as
                // title 1 does; one that times itself on the clock is not measured by it.
                LOG_INFO(Service,
                         "nx2:vi2 last 600 presents: frame {} us (max {}, {} over 100 ms), "
                         "blocked on the GPU fence {} us (max {}), composite {} us, vblank "
                         "pacing {} us; {} asked for 2+ vblanks, {} found the GPU thread a frame "
                         "behind, word 12 changed {} times, game speed {:.0f}%",
                         m_stats.frame_us / (std::max<long long>)(m_stats.frames, 1),
                         m_stats.max_frame_us, m_stats.hitches, m_stats.fence_us / 600,
                         m_stats.max_fence_us, m_stats.composite_us / 600, m_stats.pace_us / 600,
                         m_stats.long_presents, m_stats.gpu_behind, m_stats.swap_changes,
                         m_stats.frame_us > 0 ? 100.0 * static_cast<double>(m_stats.game_us) /
                                                    static_cast<double>(m_stats.frame_us)
                                              : 0.0);
                if (m_stats.queued != 0) {
                    LOG_INFO(Service,
                             "nx2:vi2 last 600 presents: {} release fences queued {} us ahead "
                             "(max {}), {} presents held at {} frames ahead",
                             m_stats.queued, m_stats.queue_ahead_us / m_stats.queued,
                             m_stats.max_queue_ahead_us, m_stats.queue_holds, MaxQueuedFrames());
                }
                if (ui_snapshot) {
                    auto& snapshot = Tegra::g_nx2_ui_snapshot;
                    u64 patched_uploads{}, patched_bytes{}, remarked{}, compared_bytes{};
                    size_t ranges{};
                    {
                        std::scoped_lock lock{snapshot.mutex};
                        patched_uploads = std::exchange(snapshot.patched_uploads, 0);
                        patched_bytes = std::exchange(snapshot.patched_bytes, 0);
                        remarked = std::exchange(snapshot.remarked, 0);
                        compared_bytes = std::exchange(snapshot.compared_bytes, 0);
                        ranges = snapshot.ranges.size();
                    }
                    LOG_INFO(Service,
                             "nx2:vi2 UI copy: {} of the last 600 presents copied "
                             "{} KiB of UI storage on average ({} ranges known) in {} us; {} "
                             "uploads took {} KiB from the copies (the game had rewritten it, {} "
                             "KiB compared), {} ranges marked for upload again",
                             m_stats.snapshots,
                             m_stats.snapshots ? m_stats.snapshot_bytes / m_stats.snapshots / 1024 : 0,
                             ranges, m_stats.snapshot_us / 600, patched_uploads,
                             patched_bytes / 1024, compared_bytes / 1024, remarked);
                }
                m_stats = {.last_present = entered};
            }
        }
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots),
                             static_cast<u32>(AvailableSlots >> 32), release[0], release[1],
                             release[2], release[3]});
    }

    // EDEN_NX2_PRESENT_QUEUE=1 (also per title, nx2_title_defaults.h): instead of holding the
    // present thread until the frame's vblank (PaceToVblank), reply with a release fence that a
    // timer signals then, as the console's compositor does (Eglantine #695). Games that time their
    // frames on the CPU no longer count the display wait: Super Mario Bros. Wonder NS2E fell to
    // 30 fps with it. A game that doesn't wait for the fence is held only once its fences are
    // MaxQueuedFrames frames ahead.
    static bool PresentQueue() {
        static Common::Nx2::SessionValue<bool> enabled{[] {
            const std::string_view value = Common::Nx2::Setting("EDEN_NX2_PRESENT_QUEUE", "0");
            return !value.empty() && value[0] != '0';
        }};
        return enabled.Get();
    }

    /// Returns the NativeSyncHandle {~syncpoint, 0, value, 0} of a fence signalled when the
    /// previous present's interval (PresentInterval) has passed, at once when the game is slower.
    std::array<u32, 4> QueueRelease(u32 swap_interval, u32 refresh_rate) {
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        if (!m_release_timer) {
            m_release_nvdrv =
                system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv", true)->GetModule();
            m_release_syncpoint =
                m_release_nvdrv->GetContainer().GetSyncpointManager().AllocateSyncpoint(true);
            m_release_value = syncpoints.GetGuestSyncpointValue(m_release_syncpoint);
            m_release_timer = Core::Timing::CreateEvent(
                "nx2:vi2 release",
                [&syncpoints, id = m_release_syncpoint](
                    s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                    syncpoints.IncrementGuest(id);
                    return std::nullopt;
                });
            LOG_INFO(Service, "nx2:vi2 present queue: release fences on syncpoint {}",
                     m_release_syncpoint);
        }
        const auto interval = PresentInterval(swap_interval, refresh_rate);
        const auto now = system.CoreTiming().GetGlobalTimeNs();
        m_release_time = interval.count() == 0 ? now : (std::max)(now, m_release_time);
        const auto ahead = m_release_time - now;
        const int max_queued = MaxQueuedFrames();
        if (interval.count() != 0 && ahead > max_queued * interval) {
            SleepPresentThread(std::chrono::steady_clock::now() + ahead - max_queued * interval);
            ++m_stats.queue_holds;
        }
        const long long ahead_us =
            std::chrono::duration_cast<std::chrono::microseconds>(ahead).count();
        ++m_stats.queued;
        m_stats.queue_ahead_us += ahead_us;
        m_stats.max_queue_ahead_us = (std::max)(m_stats.max_queue_ahead_us, ahead_us);
        system.CoreTiming().ScheduleEvent(m_release_time, m_release_timer, true);
        m_release_time += interval;
        return {~m_release_syncpoint, 0, ++m_release_value, 0};
    }

    /// EDEN_NX2_PRESENT_QUEUE_FRAMES=<n> (1-4, default 4): how many frames a game's release fences
    /// may run ahead of the display before its present is held. Each frame ahead is a frame of
    /// input lag: Splatoon Raiders' hub ran 34-49 ms ahead at 60 fps, never held at 4.
    static int MaxQueuedFrames() {
        static Common::Nx2::SessionValue<int> frames{[] {
            const std::string_view value =
                Common::Nx2::Setting("EDEN_NX2_PRESENT_QUEUE_FRAMES", "4");
            const int parsed = value.empty() ? 4 : std::atoi(std::string{value}.c_str());
            return std::clamp(parsed, 1, 4);
        }};
        return frames.Get();
    }
    std::shared_ptr<Nvidia::Module> m_release_nvdrv; ///< Owns m_release_syncpoint's allocation
    u32 m_release_syncpoint{};
    u32 m_release_value{}; ///< The last release fence handed out
    std::chrono::nanoseconds m_release_time{}; ///< When the next one is due (CoreTiming time)
    std::shared_ptr<Core::Timing::EventType> m_release_timer;

    // The console's compositor frees a buffer only once its frame's GPU fence has passed, which
    // is what keeps a game from running ahead of the GPU. This reply always reports every buffer
    // free, so pace the game here instead: wait (bounded, in case the GPU waits on the CPU) until
    // Eden's GPU thread has processed the frame. Otherwise the game laps the GPU thread and
    // rewrites command memory it hasn't read yet.
    u32 m_wait_timeouts{};
    struct PresentStats {
        long long frame_us{}, fence_us{}, composite_us{}, pace_us{}, max_fence_us{};
        long long max_frame_us{}, hitches{};
        long long frames{};  ///< Frame intervals in frame_us (599 in the first window)
        long long game_us{}; ///< The time those frames asked for in word 12
        u32 long_presents{}; ///< Presents asking for 2+ vblanks
        u32 gpu_behind{};    ///< Presents that found the previous frame still on the GPU thread
        u32 swap_changes{};  ///< Changes of word 12
        long long queue_ahead_us{}, max_queue_ahead_us{}; ///< Release fences' lead (QueueRelease)
        u32 queued{};        ///< Presents answered with a release fence
        u32 queue_holds{};   ///< Of those, held MaxQueuedFrames ahead
        u32 presents{};
        std::chrono::steady_clock::time_point last_present{};
        long long snapshot_us{};
        u64 snapshots{}, snapshot_bytes{};
    } m_stats{};
    u32 m_swap_word{};    ///< The last present's word 12
    u32 m_swap_changes{}; ///< Word 12 changes logged at Info (the first 32)
    u32 m_swap_stable{};  ///< Presents since word 12 last changed (up to StableSwapPresents)
    static constexpr u32 StableSwapPresents = 120;
    long long m_last_fence_us{}; ///< The last present's wait for the GPU fence
    u32 m_held_logged{};         ///< Presents held over 100 ms logged so far
    u32 m_previous_fence_id{~0U}; // none yet (WaitForFrame ignores ids >= 192)
    u32 m_previous_fence_value{};

    // Ocarina of Time's UI copy (see Tegra::Nx2UiSnapshotState): copies the storage ranges
    // UI draws read as this present returns, before the game writes its next frame into them.
    // The GPU thread uploads from the copy until it reaches this frame's fence. The previous
    // frame's fence has passed here, so its copy is no longer needed.
    std::vector<Tegra::Nx2UiRange> m_snapshot_ranges;
    std::vector<u8> m_snapshot_data;

    void TakeUiSnapshot(u32 fence_id, u32 fence_value) {
        static constexpr u64 MaxSnapshotBytes = 32ULL << 20;
        auto& state = Tegra::g_nx2_ui_snapshot;
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        const u64 present =
            Tegra::g_nx2_ui_snapshot_present.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto retire = [] {
            auto& snapshot = Tegra::g_nx2_ui_snapshot;
            std::scoped_lock lock{snapshot.mutex};
            snapshot.RetireLocked();
        };
        if (fence_id >= 192 || syncpoints.IsReadyGuest(fence_id, fence_value)) {
            // The GPU thread is done with this frame: nothing to protect.
            retire();
            return;
        }
        {
            std::scoped_lock lock{state.mutex};
            std::erase_if(state.ranges, [present](const Tegra::Nx2UiRange& range) {
                return range.tag + Tegra::Nx2UiRangeMaxAge < present;
            });
            m_snapshot_ranges = state.ranges;
        }
        u64 total = 0;
        for (auto& range : m_snapshot_ranges) {
            range.tag = total;
            total += range.size;
        }
        if (total == 0 || total > MaxSnapshotBytes) {
            retire();
            return;
        }
        m_snapshot_data.resize(total);
        auto& memory = system.Host1x().MemoryManager();
        for (const auto& range : m_snapshot_ranges) {
            memory.ReadBlockUnsafe(range.address, m_snapshot_data.data() + range.tag, range.size);
        }
        {
            std::scoped_lock lock{state.mutex};
            // The previous frame's fence has passed: its copy retires here.
            state.RetireLocked();
            state.copied.swap(m_snapshot_ranges);
            state.data.swap(m_snapshot_data);
            u64 copied_begin = ~0ULL, copied_end = 0;
            for (const auto& range : state.copied) {
                copied_begin = (std::min)(copied_begin, range.address);
                copied_end = (std::max)(copied_end, range.address + range.size);
            }
            state.copied_begin.store(copied_begin, std::memory_order_relaxed);
            state.copied_end.store(copied_end, std::memory_order_relaxed);
            state.syncpoints.store(&syncpoints, std::memory_order_relaxed);
            state.fence_id.store(fence_id, std::memory_order_relaxed);
            state.fence_value.store(fence_value, std::memory_order_relaxed);
            state.active.store(true, std::memory_order_release);
        }
        ++m_stats.snapshots;
        m_stats.snapshot_bytes += total;
    }

    // The console flips one frame per 60 Hz vblank and keeps the rest queued, so a game can't
    // present faster than that. Every slot is reported free here: hold each present until the
    // next vblank instead (title 4 ran at up to 160 fps, its timing and audio with it).
    std::chrono::steady_clock::time_point m_next_vblank{};

    // A present is shown for its swap interval in vblanks of its refresh rate (word 12). Fire
    // Emblem: Fortune's Weave sends 0x1003c during its boot logos and 0x2003c (30 fps) after:
    // held for one vblank, it ran at double speed. A game that changes the interval every few
    // frames would be held for two on each 2 (title 1 fell from ~40 to 20-30 fps that way), so
    // word 12 counts only once the game has kept it for StableSwapPresents presents, its rate too
    // (0x1001e asks for one 30 Hz vblank), and any change goes back to one 60 Hz vblank at once.
    // Title 1 with a frame-rate mode patched into its code (Loader::g_mkw_frame_rate) is paced by
    // that mode instead. EDEN_NX2_SWAP_INTERVAL=<n> holds all other presents for n 60 Hz vblanks
    // (1: as before).
    void PaceToVblank(u32 swap_interval, u32 refresh_rate) {
        const auto interval = PresentInterval(swap_interval, refresh_rate);
        if (interval.count() == 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now > m_next_vblank + interval) {
            // Running slower than that: show at once and don't bank the lost time.
            m_next_vblank = now;
        } else if (now < m_next_vblank) {
            SleepPresentThread(m_next_vblank);
        }
        m_next_vblank += interval;
    }

    /// How long a present is shown at the speed limit (see PaceToVblank); zero without a limit.
    /// Published for the host frame pacer (vk_host_pacer.h).
    std::chrono::nanoseconds PresentInterval(u32 swap_interval, u32 refresh_rate) {
        auto& published = VideoCommon::Nx2Perf::g_guest_frame_interval_ns;
        if (!Settings::values.use_speed_limit.GetValue() || Settings::SpeedLimit() == 0) {
            published.store(0, std::memory_order_relaxed);
            return {};
        }
        static const u32 forced_interval = [] {
            const char* value = std::getenv("EDEN_NX2_SWAP_INTERVAL");
            return value ? (std::min)(static_cast<u32>(std::strtoul(value, nullptr, 10)), 4U) : 0U;
        }();
        // Title 1 locked to 30 fps asks for 2 every frame and is held for what it asks at once.
        // Otherwise it gets one vblank: its dynamic mode picks 1 or 2 from how long its frames
        // take, and holding a frame for 2 would make every frame take 2 and keep it at 30 for
        // good. As released it can flip between 1 and 2 too, and a slow PC that kept 2 for 120
        // presents would be capped at 30.
        const bool mkw = system.GetApplicationProcessProgramID() == Loader::MarioKartWorldProgramId;
        const auto mkw_mode = mkw ? Loader::g_mkw_frame_rate.load(std::memory_order_relaxed)
                                  : Settings::MkwFrameRate::Off;
        u32 vblanks = 1;
        u32 rate = 60;
        if (mkw_mode == Settings::MkwFrameRate::Lock30) {
            vblanks = swap_interval;
        } else if (mkw_mode == Settings::MkwFrameRate::Off && forced_interval != 0 &&
                   !Common::VideoFramesRecent()) {
            // Not while a video plays: movies run on the game's own frame count, so a forced
            // interval slowed them down.
            vblanks = forced_interval;
        } else if (!mkw && m_swap_stable >= StableSwapPresents) {
            vblanks = swap_interval;
            rate = refresh_rate;
        }
        auto interval = std::chrono::nanoseconds{100'000'000'000LL * vblanks / rate /
                                                 Settings::SpeedLimit()};
        // Genlock: with the host frame pacer on, a frame period within 1% of a whole number of
        // display refreshes becomes exactly that many measured refreshes (a "120 Hz" display at
        // 119.98 Hz: 33.3389 ms instead of 33.3333). On its own clock the game drifted against
        // the display and missed a refresh about once a minute. The game runs at most 1% slower.
        if (const long long host_refresh =
                VideoCommon::Nx2Perf::g_host_refresh_ns.load(std::memory_order_relaxed);
            host_refresh > 0) {
            const long long refreshes = (interval.count() + host_refresh / 2) / host_refresh;
            const long long locked = refreshes * host_refresh;
            if (refreshes >= 1 && std::abs(locked - interval.count()) * 100 < interval.count()) {
                interval = std::chrono::nanoseconds{locked};
            }
        }
        published.store(interval.count(), std::memory_order_relaxed);
        return interval;
    }

    void SleepPresentThread(std::chrono::steady_clock::time_point until) {
        // Sleep this service thread in the guest kernel. On AM's server it runs on guest core 3,
        // and a host sleep held that core for the whole wait: game threads scheduled there ran
        // only between presents (Fortune's Weave's loader read one RomFS block per frame).
        // EDEN_NX2_PACE_HOST_SLEEP=0 opts into sleeping in the guest kernel. The graphics
        // system's own host thread (nx2_gsv_server.h) is a dummy thread on no guest core, so it
        // sleeps on the host either way.
        static const bool host_sleep = [] {
            const char* value = std::getenv("EDEN_NX2_PACE_HOST_SLEEP");
            return value == nullptr || value[0] != '0';
        }();
        if (host_sleep || Kernel::GetCurrentThread(kernel).IsDummyThread()) {
            std::this_thread::sleep_until(until);
        } else {
            const auto wait = std::chrono::duration_cast<std::chrono::nanoseconds>(
                until - std::chrono::steady_clock::now());
            if (wait.count() <= 0) {
                return;
            }
            static_cast<void>(Kernel::GetCurrentThread(kernel).Sleep(
                kernel, kernel.HardwareTimer().GetTick() + wait.count() + 2));
        }
    }

    /// Waits until the fence is reached; returns whether it was. With `unless_gpu_waits`, gives up
    /// after a few ms once a GPU channel was seen suspended on a semaphore acquire (it may wait for
    /// a release the game makes after the present returns).
    bool WaitForFrame(u32 fence_id, u32 fence_value, bool unless_gpu_waits = false) {
        if (fence_id >= 192) {
            return true;
        }
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        const auto start = std::chrono::steady_clock::now();
        // Long enough to cover shader/pipeline compile stalls: a game that gets more than a few
        // command buffers ahead misses the equal-value gates its other channels acquire. It keeps
        // waiting while the syncpoint moves: a fixed 4 s let title 1 lap a slow GPU thread
        // (paging on a 16 GB host), its command stream desynced and the host crashed on garbage
        // state. EDEN_NX2_PRESENT_WAIT_MS sets how long the syncpoint may stand still.
        static const std::chrono::milliseconds stall_limit{[] {
            const char* text = std::getenv("EDEN_NX2_PRESENT_WAIT_MS");
            return text ? (std::max)(std::strtoll(text, nullptr, 10), 250LL) : 8000LL;
        }()};
        constexpr std::chrono::seconds MaxWait{60};
        auto last_progress = start;
        u32 last_value = syncpoints.GetGuestSyncpointValue(fence_id);
        if (!syncpoints.IsReadyGuest(fence_id, fence_value)) {
            // Woken by the GPU thread as it reaches the fence. Polling with 200 us sleeps slept
            // about 1 ms per check on Windows, added to every frame. An action left registered
            // after a timeout fires later on a state nobody waits on.
            struct Wake {
                std::mutex mutex;
                std::condition_variable cv;
                bool done{};
            };
            auto wake = std::make_shared<Wake>();
            syncpoints.RegisterGuestAction(fence_id, fence_value, [wake] {
                {
                    std::scoped_lock lk{wake->mutex};
                    wake->done = true;
                }
                wake->cv.notify_all();
            });
            std::unique_lock lk{wake->mutex};
            // Title 1's channels resume and suspend again while they wait on the game, so any
            // suspension counts, not only an unbroken one.
            bool gpu_waited = false;
            while (!wake->done && system.IsPoweredOn()) {
                const auto now = std::chrono::steady_clock::now();
                if (unless_gpu_waits) {
                    gpu_waited = gpu_waited || system.GPU().HasSuspendedChannels();
                    if (gpu_waited && now - start > std::chrono::milliseconds{3}) {
                        return false;
                    }
                }
                if (const u32 value = syncpoints.GetGuestSyncpointValue(fence_id);
                    value != last_value) {
                    last_value = value;
                    last_progress = now;
                } else if (now - last_progress >= stall_limit) {
                    break;
                }
                if (now - start >= MaxWait) {
                    break;
                }
                wake->cv.wait_for(lk, std::chrono::milliseconds{unless_gpu_waits ? 1 : 50});
            }
        }
        if (!syncpoints.IsReadyGuest(fence_id, fence_value) && m_wait_timeouts++ < 64) {
            const auto now = std::chrono::steady_clock::now();
            LOG_WARNING(Service,
                        "nx2:vi2 present #{} fence ({}, {}) not reached in {} ms, {} ms without "
                        "progress (now {})",
                        m_presents, fence_id, fence_value,
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count(),
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress)
                            .count(),
                        syncpoints.GetGuestSyncpointValue(fence_id));
        }
        if (m_presents <= 8 || (m_presents % 600) == 0) {
            LOG_INFO(Service, "nx2:vi2 present #{} fence ({}, {}) now {}, waited {} us",
                     m_presents, fence_id, fence_value, syncpoints.GetGuestSyncpointValue(fence_id),
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count());
        }
        return syncpoints.IsReadyGuest(fence_id, fence_value);
    }

    // Shows a presented native image: word 5 is the image id, ~word 22 / word 24 the GPU fence
    // (syncpoint, value) to wait for (layout from changes.patch). The image is a block-linear
    // RGBA8 surface in the console's native GOB layout at GPU address == CPU address.
    // shown_size: the present's word 7, ((height - 1) << 16) | (width - 1) of the area the game drew
    // into from the image's top-left corner (0 when it gives none).
    void Composite(u32 image_id, u32 fence_id, u32 fence_value, u32 shown_size) {
        u64 address{};
        Nx2ImageLayout layout{};
        u32 image_format{};
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(image_id);
            if (image == m_objects->images.end()) {
                return;
            }
            const auto memory = m_objects->memories.find(image->second.memory);
            if (memory == m_objects->memories.end() || memory->second.address == 0) {
                return;
            }
            address = memory->second.address;
            layout = Nx2CalculateImageLayout(image->second.preference.data());
            image_format = image->second.preference[6];
        }
        const auto device = Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(address);
        if (!device) {
            LOG_WARNING(Service, "nx2:vi2 present: image {} at {:#x} has no device mapping",
                        image_id, address);
            return;
        }
        u32 crop_width = layout.width;
        u32 crop_height = layout.height;
        // The present says how much of the image the game drew: Kirby Air Riders draws 1920x1080
        // into 2560x1440 images, Mario Kart World 2560x1440 into 3840x2160 ones. Showing the whole
        // image put the frame in the top-left corner.
        const u32 shown_width = (shown_size & 0xFFFF) + 1, shown_height = (shown_size >> 16) + 1;
        const bool has_shown_size = shown_size != 0 && shown_width <= layout.width &&
                                    shown_height <= layout.height;
        if (has_shown_size) {
            crop_width = shown_width;
            crop_height = shown_height;
        }
        // Handheld titles keep their docked present images (up to 3840x2160) and draw the frame
        // into the top-left 1920x1080, the handheld panel's resolution; the console's display
        // scans out only that region. Showing whole images put the frame in a corner inside black
        // margins that scaled with the window (seen on Mario Kart World and Splatoon Raiders).
        if (!Settings::IsDockedMode()) {
            crop_width = (std::min)(crop_width, 1920U);
            crop_height = (std::min)(crop_height, 1080U);
        } else if (!has_shown_size &&
                   system.GetApplicationProcessProgramID() == 0x0400C3F00006E000ULL &&
                   layout.width == 3840 && layout.height == 2160) {
            // Mario Kart World docked draws 2560x1440 into the top-left of its 4K images.
            crop_width = 2560;
            crop_height = 1440;
        }
        static const u32 env_crop_w = [] {
            const char* v = std::getenv("EDEN_NX2_CROP_WIDTH");
            return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0U;
        }();
        static const u32 env_crop_h = [] {
            const char* v = std::getenv("EDEN_NX2_CROP_HEIGHT");
            return v ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 0U;
        }();
        if (env_crop_w != 0 && env_crop_h != 0) {
            crop_width = env_crop_w;
            crop_height = env_crop_h;
        }
        const u64 shown = (u64{layout.width} << 48) | (u64{layout.height} << 32) |
                          (u64{crop_width} << 16) | crop_height;
        if (shown != m_shown_layout) {
            m_shown_layout = shown;
            LOG_INFO(Service,
                     "nx2:vi2 composite image {} layout {}x{}, drawn area {:#x}, crop {}x{}",
                     image_id, layout.width, layout.height, shown_size, crop_width, crop_height);
        }
        std::vector<Tegra::FramebufferConfig> layers{Tegra::FramebufferConfig{
            .address = *device,
            .offset = 0,
            .width = layout.width,
            .height = layout.height,
            .stride = static_cast<u32>(layout.pitch / 4),
            // Preference word 6: 1 = RGBA8, 4 = RGB10A2.
            .pixel_format = image_format == 4 ? android::PixelFormat::Rgba1010102
                                              : android::PixelFormat::Rgba8888,
            .transform_flags = android::BufferTransformFlags::Unset,
            .crop_rect = {0, 0, static_cast<int>(crop_width), static_cast<int>(crop_height)},
            .blending = Tegra::BlendMode::Opaque,
            .native_tiling = true,
        }};
        std::vector<Nvidia::NvFence> fences;
        if (fence_id < 192) {
            fences.push_back({static_cast<s32>(fence_id), fence_value});
        }
        system.GPU().RequestComposite(std::move(layers), std::move(fences));
        CaptureFrame();
        DumpRanges();
        system.SpeedLimiter().DoSpeedLimiting(system.CoreTiming().GetGlobalTimeUs());
        system.GetPerfStats().EndSystemFrame();
        system.GetPerfStats().BeginSystemFrame();
    }

    // EDEN_NX2_FRAME_CAPTURE=<dir>: every 120th present, save what the renderer shows as
    // nx2-frame-<n>.tga in <dir> (lets hidden test runs check the output; idea from changes.patch).
    void CaptureFrame() {
        static const char* const directory = std::getenv("EDEN_NX2_FRAME_CAPTURE");
        if (directory == nullptr || (m_presents % 120) != 1 || m_capture_pending->exchange(true)) {
            return;
        }
        const auto layout = Layout::DefaultFrameLayout(1280, 720);
        auto pixels = std::make_shared<std::vector<u8>>(layout.width * layout.height * 4);
        const std::string path =
            fmt::format("{}/nx2-frame-{:05}.tga", directory, m_presents);
        auto pending = m_capture_pending; // the screenshot callback can outlive the session
        system.GPU().Renderer().RequestScreenshot(
            pixels->data(),
            [pixels, layout, path, pending](bool invert_y) {
                std::array<u8, 18> header{};
                header[2] = 2; // uncompressed true colour
                header[12] = static_cast<u8>(layout.width);
                header[13] = static_cast<u8>(layout.width >> 8);
                header[14] = static_cast<u8>(layout.height);
                header[15] = static_cast<u8>(layout.height >> 8);
                header[16] = 32;
                // Descriptor bit 5: rows top to bottom. invert_y means the data is bottom-up.
                header[17] = invert_y ? 0x08 : 0x28;
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(header.data(), 1, header.size(), file);
                    std::fwrite(pixels->data(), 1, pixels->size(), file);
                    std::fclose(file);
                }
                pending->store(false);
            },
            layout);
    }

    // EDEN_NX2_DUMP=<present>:<hex address>:<hex size>[,<hex address>:<hex size>...]: at that
    // present, flush those GPU ranges back from the host GPU and save their bytes as
    // nx2-dump-<address>.bin in the EDEN_NX2_FRAME_CAPTURE directory (to inspect what a pass
    // rendered, e.g. a 3D volume, offline).
    void DumpRanges() {
        static const std::string spec = [] {
            const char* text = std::getenv("EDEN_NX2_DUMP");
            return text ? std::string{text} : std::string{};
        }();
        if (spec.empty() || m_presents != std::strtoull(spec.c_str(), nullptr, 10)) {
            return;
        }
        const char* const directory = std::getenv("EDEN_NX2_FRAME_CAPTURE");
        size_t position = spec.find(':');
        while (position != std::string::npos) {
            char* end{};
            const u64 address = std::strtoull(spec.c_str() + position + 1, &end, 16);
            const u64 size = *end == ':' ? std::strtoull(end + 1, &end, 16) : 0;
            position = spec.find(',', position + 1); // the next "address:size"
            const std::string path = fmt::format("{}/nx2-dump-{:x}.bin", directory ? directory : ".", address);
            // On the GPU thread: waiting for it here can deadlock with a GPU wait on this present.
            static_cast<void>(system.GPU().RunOnGpuThread([&system = system, address, size, path] {
                std::vector<u8> bytes(size);
                for (u64 current = address; current < address + size;) {
                    const u64 chunk = std::min((current | 0xfff) + 1, address + size) - current;
                    if (const auto device = Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(current)) {
                        system.GPU().Renderer().ReadRasterizer()->FlushRegion(*device, chunk);
                        system.Host1x().MemoryManager().ReadBlock(*device, bytes.data() + (current - address), chunk);
                    }
                    current += chunk;
                }
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(bytes.data(), 1, bytes.size(), file);
                    std::fclose(file);
                }
                LOG_INFO(Service, "nx2: dumped {:#x} bytes at {:#x} to {}", size, address, path);
            }));
        }
    }

    // 228(u64 token, u32 slot): return a buffer without presenting it.
    void CancelClientBuffer(HLERequestContext& ctx) {
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots),
                             static_cast<u32>(AvailableSlots >> 32), 0, 0, 0, 0});
    }

    // 212(u64 layer): u64 bits of the layer's buffer slots that are ready. The client polls it once
    // a second until some are set; the auto stub's reply was the request's leftovers (Bananza read
    // 0x2_00000000). Report the same slots 228/229 hand back.
    void GetLayerBufferBits(HLERequestContext& ctx) {
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots),
                             static_cast<u32>(AvailableSlots >> 32)});
    }

    // 11: the notification event the SDK's render thread waits on while it tears a client down.
    // One event per session, signalled as the notification bits fill (201/202, 101/102); an event
    // that never fired left that thread blocked after 202.
    void GetNotificationEvent(HLERequestContext& ctx) {
        if (m_notification_event == nullptr) {
            m_notification_event = m_context.CreateEvent("nx2:vi2:Notify");
        }
        if (AnyNotification()) {
            m_notification_event->Signal(system.Kernel());
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, m_notification_event->GetReadableEvent());
    }

    // 21(W: 32 u32 notification bitmaps): hand them over and clear.
    void GetNotifications(HLERequestContext& ctx) {
        const auto size = std::min<std::size_t>(ctx.GetWriteBufferSize(), sizeof(m_notifications));
        if (size != 0) {
            ctx.WriteBuffer(m_notifications.data(), size);
        }
        if (size >= sizeof(m_notifications)) {
            m_notifications.fill(0);
            if (m_notification_event != nullptr) {
                m_notification_event->Clear(system.Kernel());
            }
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 101(u64 display "default", u32 notification): remember the display's notification index.
    void OpenDisplay(HLERequestContext& ctx) {
        m_display_notification = ctx.CommandBuffer()[4];
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 102(u64 display): the display retired; wake whoever waits for its notification.
    void CloseDisplay(HLERequestContext& ctx) {
        Notify(m_display_notification);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 201(u64 token, u32 retire notification, u32 available notification).
    void RegisterClientState(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 token = u64{w[2]} | (u64{w[3]} << 32);
        const size_t notification_bits = m_notifications.size() * 32;
        if (token != 0 && w[4] < notification_bits && w[5] < notification_bits && w[4] != w[5]) {
            m_clients[token] = w[4];
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 202(u64 token): the client retired; the SDK's render thread blocks on this notification.
    void UnregisterClientState(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 token = u64{w[2]} | (u64{w[3]} << 32);
        const auto client = m_clients.find(token);
        if (client != m_clients.end()) {
            LOG_INFO(Service, "nx2:vi2 client {:#x} retired, notification {}", token,
                     client->second);
            Notify(client->second);
            m_clients.erase(client);
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    bool AnyNotification() const {
        return std::ranges::any_of(m_notifications, [](u32 bits) { return bits != 0; });
    }

    void Notify(u32 index) {
        if (index >= m_notifications.size() * 32) {
            return;
        }
        m_notifications[index / 32] |= 1U << (index % 32);
        if (m_notification_event != nullptr) {
            m_notification_event->Signal(system.Kernel());
        }
    }

    std::map<u64, u32> m_clients; ///< Client token -> retire notification index
    std::array<u32, 32> m_notifications{};
    u32 m_display_notification{};
    Kernel::KEvent* m_notification_event{};

    void ReturnOne(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC {}, returning 1", ctx.GetCommand());
        TipcReplyWords(ctx, {1});
    }

    // The same reply as the auto stub (success, no data), without its log lines.
    void SucceedQuietly(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Must be < 8; nn::vi::detail::LayerHistory creates 7 timestamp recorders per layer.
    void GetTimestampRecorderCount(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC 5, returning 7");
        TipcReplyWords(ctx, {7});
    }

    // The event for a request, made on its first use: a repeated (command, argument) gets the same
    // one, so a client asking again (per frame or layer) doesn't add an event, and a vsync event
    // the timer signals, every time. The argument is the first data word, past the copy handle
    // 32 sends.
    Kernel::KEvent*& CachedEvent(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        u32 argument = w[2];
        if ((w[1] & 0x80000000) != 0) {
            const u32 desc = w[2];
            argument = w[3 + ((desc & 1) ? 2 : 0) + ((desc >> 1) & 0xF) + ((desc >> 5) & 0xF)];
        }
        return m_events[{ctx.GetCommand(), argument}];
    }

    void ReturnEvent(HLERequestContext& ctx) {
        auto*& event = CachedEvent(ctx);
        if (event == nullptr) {
            LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC {} [2]={:#x} [3]={:#x}, returning an event",
                        ctx.GetCommand(), ctx.CommandBuffer()[2], ctx.CommandBuffer()[3]);
            event = m_context.CreateEvent(fmt::format("nx2:vi2:Event{}", m_events.size()));
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    // 31(u32 kind) and 32(u32 display): events signalled on every vblank. Title 1's main thread
    // waits on the 32 event until it has fired twice before it submits any GPU work, and hung
    // there while it never fired.
    void GetVsyncEvent(HLERequestContext& ctx) {
        auto*& event = CachedEvent(ctx);
        if (event != nullptr) {
            LOG_DEBUG(Service, "nx2:vi2 {} vsync event again, [2]={:#x}", ctx.GetCommand(),
                      ctx.CommandBuffer()[2]);
            IPC::ResponseBuilder rb{ctx, 2, 1, 0};
            rb.Push(ResultSuccess);
            rb.PushCopyObjects(ctx, event->GetReadableEvent());
            return;
        }
        LOG_INFO(Service, "nx2:vi2 {} vsync event, [2]={:#x}", ctx.GetCommand(), ctx.CommandBuffer()[2]);
        event = m_context.CreateEvent(fmt::format("nx2:vi2:Vsync{}", m_events.size()));
        {
            std::scoped_lock lk{m_vsync->mutex};
            m_vsync->events.push_back(event);
        }
        if (!m_vsync_timer) {
            m_vsync_timer = Core::Timing::CreateEvent(
                "nx2:vi2 vsync",
                [vsync_state = m_vsync, &kernel = system.Kernel()](
                    s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                    std::scoped_lock lk{vsync_state->mutex};
                    for (auto* vsync : vsync_state->events) {
                        vsync->Signal(kernel);
                    }
                    return std::nullopt;
                });
            // EDEN_NX2_VSYNC_HZ=<rate> (experiment): the game starts frames on this event, so a
            // frame that misses a 60 Hz tick idles until the next one.
            static const std::chrono::nanoseconds VblankInterval{[] {
                const char* text = std::getenv("EDEN_NX2_VSYNC_HZ");
                const long long hz = text ? (std::max)(std::strtoll(text, nullptr, 10), 1LL) : 60LL;
                return 1'000'000'000LL / hz;
            }()};
            system.CoreTiming().ScheduleLoopingEvent(VblankInterval, VblankInterval, m_vsync_timer);
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    KernelHelpers::ServiceContext m_context;
    std::shared_ptr<Nx2NativeObjects> m_objects;
    std::map<std::pair<u32, u32>, Kernel::KEvent*> m_events; ///< By (command, argument)
    // Shared with the vsync timer callback, which can outlive the session by one call.
    struct VsyncState {
        std::mutex mutex;
        std::vector<Kernel::KEvent*> events;
    };
    std::shared_ptr<VsyncState> m_vsync = std::make_shared<VsyncState>();
    std::shared_ptr<Core::Timing::EventType> m_vsync_timer;
    u64 m_presents{};
    u64 m_shown_layout{}; // image and crop size last logged
    std::shared_ptr<std::atomic<bool>> m_capture_pending = std::make_shared<std::atomic<bool>>(false);
};

// True the first time a Switch 2 video command is seen, so each one is logged once in full and new
// or unexpected commands stand out.
inline bool Nx2FirstVideoCommand(std::string_view service, u32 command) {
    static std::mutex mutex;
    static std::set<std::pair<std::string, u32>> seen;
    std::scoped_lock lk{mutex};
    return seen.emplace(std::string{service}, command).second;
}

// The Switch 2 video decoder (nn::codec::video::detail::ipc). gsv 902/906/1002 hand out a session
// whose TIPC 1 (CreateVideoDecoderService: handle at word 3, result at word 4) creates a decoder
// service, another session of this class. Layouts read from the SDK's nn::codec VP9 decoder:
// - 200(copy handle: work buffer transfer memory, u64 size, u32, u32, u32): initialize. 201 adds a
//   fourth u32 (a flag that 200 leaves at 1) and 202 that and a u64: the same initialization.
//   210(copy handle, u32 x4) replies only a result.
// - 101(A: codec name, "video/vp9\0"; u32 width, u32 height, u32 rate, u32): configure.
// - 3(): the submission-ready event (copy handle at word 3, result at word 4).
// - 6 / 207 / 8: start / stop / flush; 115: finalize.
// - 311(u32 image id): register an output image (gsv 911 creates it); reply its NativeSyncHandle,
//   signalled once a frame has been decoded into the image. The client acquires output images in
//   registration/release order from a ring of its own (no IPC), so frames fill them in that order.
// - 314(u32 image id, u32, NativeSyncHandle): give an output image back; the image may be
//   rewritten once that handle (the client's reads) has passed. Reply the image's next handle.
// - 212(u32 image id): unregister it.
// - 109(A: one compressed frame, u64 pts in us, u8 end of stream): submit input; reply a u64.
//   400 is the same with a second A buffer after the frame, a 0x838-byte block of frame
//   information; the decoder here only needs the frame.
// - 213(): the next finished frame (16 words): u32 image id, u32 width, u32 height, u32 width,
//   u32 height, u32 colour space, TimeSpan pts (ns), u8 end of stream.
// A NativeSyncHandle is {u32 SyncCounterId (~syncpoint), u32 mode, u64 value}; clients wait on it
// with nx2:syncpt 103/201. Each decoder service owns a syncpoint that counts its finished frames.
// FFmpeg decodes on a thread of the session's own (a 1440p VP9 frame takes milliseconds, and the
// service thread also serves presents and syncpoint waits) into the layout gsv 961 describes.
class INx2VideoSession final : public ServiceFramework<INx2VideoSession> {
public:
    explicit INx2VideoSession(Core::System& system_, std::shared_ptr<Nx2NativeObjects> objects_,
                              const char* name)
        : ServiceFramework{system_, name}, m_context{system_, name},
          m_objects{std::move(objects_)} {
        RegisterHandlersTipc({
            {1, &INx2VideoSession::CreateDecoderService, "CreateVideoDecoderSession"},
            {3, &INx2VideoSession::GetEvent, "GetInputEvent"},
            {6, ReadyGate() ? &INx2VideoSession::Start : &INx2VideoSession::Succeed, "Start"},
            {8, &INx2VideoSession::Flush, "Flush"},
            {101, &INx2VideoSession::Configure, "Configure"},
            {109, &INx2VideoSession::SubmitInput, "Decode"},
            {115, &INx2VideoSession::Finalize, "FinalizeVideoDecoder"},
            {200, &INx2VideoSession::Succeed, "InitializeVideoDecoderWithTransferMemory"},
            {201, &INx2VideoSession::Succeed, "InitializeWithFlag"},
            {202, &INx2VideoSession::Succeed, "InitializeWithFlagAndValue"},
            {207, &INx2VideoSession::Flush, "Stop"},
            {210, &INx2VideoSession::Succeed, "InitializeVideoDecoderWithRemoteMemory"},
            {212, &INx2VideoSession::UnregisterBuffer, "UnbindFrame"},
            {213, &INx2VideoSession::GetOutputInfo, "GetFrame"},
            {311, &INx2VideoSession::RegisterBuffer, "BindFrame"},
            {314, &INx2VideoSession::ReleaseBuffer, "DestroyFrame"},
            {400, &INx2VideoSession::SubmitInput, "Decode2"},
        });
    }

    ~INx2VideoSession() override {
        if (m_worker.joinable()) {
            {
                std::scoped_lock lk{m_mutex};
                m_quit = true;
            }
            m_cv.notify_all();
            m_worker.join();
        }
        // The ready timer runs on the HostTiming thread: clear its event under its lock first.
        {
            std::scoped_lock lk{m_ready->mutex};
            m_ready->event = nullptr;
        }
        if (m_ready_timer) {
            system.CoreTiming().UnscheduleEvent(m_ready_timer);
        }
        if (m_event != nullptr) {
            m_context.CloseEvent(m_event);
        }
        if (m_syncpoint != NoSyncpoint) {
            NvdrvSyncpoints().FreeSyncpoint(m_syncpoint);
        }
    }

private:
    struct Input {
        std::vector<u8> data;
        u64 pts; // us
        bool end;
    };
    struct Decoded {
        std::optional<Tegra::Host1x::Nx2Picture> picture; // none: an empty end-of-stream frame
        u64 pts;
        bool end;
    };
    struct Output {
        u32 image;
        u32 width;
        u32 height;
        u64 pts;
        bool end;
    };
    struct Fence {
        u32 syncpoint;
        u32 value;
    };

    static constexpr u32 NoSyncpoint = ~0U;
    static constexpr size_t MaxQueuedInputs = 8;
    static constexpr size_t WarnInputBytes = size_t{256} << 20; // logged once, never dropped
    static constexpr size_t MaxHeldOutputs = 64; // finished frames kept for 213
    static constexpr size_t MaxDecodedAhead = 2; // decoded frames held while no image is free
    static constexpr u32 HostSyncpoints = 192;   // Tegra::Host1x::SyncpointManager
    static constexpr u32 MaxDroppedInARow = 30;  // see DropOvertakenFrames

    void Log(HLERequestContext& ctx, std::string_view what) {
        const auto* w = ctx.CommandBuffer();
        if (Nx2FirstVideoCommand(GetServiceName(), ctx.GetCommand())) {
            LOG_INFO(Service, "{} TIPC {} {} (first use): words {:08x}", GetServiceName(),
                     ctx.GetCommand(), what, fmt::join(std::span<const u32>{w, 12}, " "));
        } else if (static const bool trace = std::getenv("EDEN_NX2_VDEC_TRACE") != nullptr; trace) {
            // EDEN_NX2_VDEC_TRACE=1: every video decoder call at Info level.
            LOG_INFO(Service, "{} TIPC {} {}: [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x}",
                     GetServiceName(), ctx.GetCommand(), what, w[2], w[3], w[4], w[5]);
        } else {
            LOG_DEBUG(Service, "{} TIPC {} {}: [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x}",
                      GetServiceName(), ctx.GetCommand(), what, w[2], w[3], w[4], w[5]);
        }
    }

    Nvidia::NvCore::SyncpointManager& NvdrvSyncpoints() {
        return system.ServiceManager()
            .GetService<Nvidia::NVDRV>("nvdrv", true)
            ->GetModule()
            ->GetContainer()
            .GetSyncpointManager();
    }

    void CreateDecoderService(HLERequestContext& ctx) {
        Log(ctx, "create decoder service");
        ReturnClientOwnedRawSession(
            system, ctx, std::make_shared<INx2VideoSession>(system, m_objects, "nx2:vdecsvc"));
    }

    void Succeed(HLERequestContext& ctx) {
        Log(ctx, "");
        TipcReplyWords(ctx, {});
    }

    void Start(HLERequestContext& ctx) {
        Log(ctx, "start");
        m_ready->SetRunning(system.Kernel(), true);
        m_ready->Signal(system.Kernel());
        TipcReplyWords(ctx, {});
    }

    void Finalize(HLERequestContext& ctx) {
        Log(ctx, "finalize");
        m_ready->SetRunning(system.Kernel(), false);
        {
            std::scoped_lock lk{m_mutex};
            CompleteHandedFences();
            m_clock.Reset();
            m_stream_start.reset();
        }
        TipcReplyWords(ctx, {});
    }

    void Configure(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        std::string codec;
        if (!ctx.BufferDescriptorA().empty()) {
            const auto name = ctx.ReadBufferA(0);
            codec.assign(reinterpret_cast<const char*>(name.data()),
                         strnlen(reinterpret_cast<const char*>(name.data()), name.size()));
        }
        Log(ctx, "configure");
        LOG_INFO(Service, "{} configure codec \"{}\" {}x{} rate {}", GetServiceName(), codec, w[5],
                 w[6], w[7]);
        {
            std::scoped_lock lk{m_mutex};
            m_codec = std::move(codec);
            m_width = w[5];
            m_height = w[6];
            m_clock.Reset();
            m_stream_start.reset();
        }
        if (!m_worker.joinable()) {
            m_worker = std::thread{[this] { DecodeLoop(); }};
        }
        TipcReplyWords(ctx, {});
    }

    void GetEvent(HLERequestContext& ctx) {
        Log(ctx, "event");
        if (m_event == nullptr) {
            m_event = m_context.CreateEvent(fmt::format("{}:Ready", GetServiceName()));
            // The client attaches it auto-clearing, so one signal is consumed by the first wait:
            // while the input queue has room, re-signal every 4 ms; a spurious wake only makes it
            // check again.
            {
                std::scoped_lock lk{m_ready->mutex};
                m_ready->event = m_event;
            }
            m_ready_timer = Core::Timing::CreateEvent(
                "nx2:vdec ready",
                [ready = m_ready, &kernel = system.Kernel()](
                    s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                    ready->Signal(kernel);
                    return std::nullopt;
                });
            constexpr std::chrono::nanoseconds ReadyInterval{std::chrono::milliseconds{4}};
            system.CoreTiming().ScheduleLoopingEvent(ReadyInterval, ReadyInterval, m_ready_timer);
        }
        if (ReadyGate()) {
            m_ready->Signal(system.Kernel());
        } else {
            m_event->Signal(system.Kernel());
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, m_event->GetReadableEvent());
    }

    // Queues an output image for the next decoded frame; returns the NativeSyncHandle words that
    // signal once that frame is in it.
    std::array<u32, 4> HandOut(u32 image, std::optional<Fence> release_fence) {
        if (m_syncpoint == NoSyncpoint) {
            m_syncpoint = NvdrvSyncpoints().AllocateSyncpoint(false);
            m_sync_base = system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(m_syncpoint);
            LOG_INFO(Service, "{} counts decoded frames on syncpoint {} from {}",
                     GetServiceName(), m_syncpoint, m_sync_base);
        }
        u32 value;
        {
            std::scoped_lock lk{m_mutex};
            m_free.push_back(image);
            if (release_fence) {
                m_release_fences[image] = *release_fence;
            }
            // EDEN_NX2_VDEC_EOS_RELEASE_NOW=1: after the end of a stream, a release completes at
            // once with the syncpoint's current value, since nothing fills the image again. Mario
            // Kart World waits for those fences after its title movie, but Donkey Kong Bananza
            // acquires a frame for every completed fence and aborts on an empty ring, so by
            // default they stay pending until Stop or Finalize.
            static const bool release_now = [] {
                const char* env_value = std::getenv("EDEN_NX2_VDEC_EOS_RELEASE_NOW");
                return env_value != nullptr && env_value[0] != '0';
            }();
            // With EosDrain, the image also gets a frame to acquire: a completed fence without one
            // makes the client's acquire find an empty ring (2111-3604).
            const bool drain = EosDrain();
            if ((drain || release_now) && release_fence && m_stream_ended && m_inputs.empty() &&
                m_decoded.empty()) {
                const u32 reached =
                    drain ? DrainFreeAsEndOfStream()
                          : system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(
                                m_syncpoint);
                m_sync_base = reached;
                m_handed = 0;
                value = reached;
            } else {
                value = m_sync_base + ++m_handed;
            }
            if (VideoTrace()) {
                LOG_INFO(Service,
                         "{} trace hand out: image {} release {} fence {} (syncpoint {} at {}), "
                         "eos {} inputs {} decoded {} outputs {} free {}",
                         GetServiceName(), image, release_fence.has_value(), value, m_syncpoint,
                         system.Host1x().GetSyncpointManager().GetGuestSyncpointValue(m_syncpoint),
                         m_stream_ended, m_inputs.size(), m_decoded.size(), m_outputs.size(),
                         m_free.size());
            }
        }
        m_cv.notify_one();
        return {~m_syncpoint, 0, value, 0};
    }

    void RegisterBuffer(HLERequestContext& ctx) {
        Log(ctx, "register output");
        const auto handle = HandOut(ctx.CommandBuffer()[2], std::nullopt);
        TipcReplyWords(ctx, {handle[0], handle[1], handle[2], handle[3]});
    }

    void ReleaseBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        Log(ctx, "release output");
        const auto handle = HandOut(w[2], Fence{~w[4], w[6]});
        TipcReplyWords(ctx, {handle[0], handle[1], handle[2], handle[3]});
    }

    void UnregisterBuffer(HLERequestContext& ctx) {
        Log(ctx, "unregister output");
        const u32 image = ctx.CommandBuffer()[2];
        {
            std::scoped_lock lk{m_mutex};
            std::erase(m_free, image);
            m_release_fences.erase(image);
        }
        TipcReplyWords(ctx, {});
    }

    void SubmitInput(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        // Words 2-4 are the A descriptor of the compressed frame (400: 5-7 that of its information
        // block), the data words follow.
        const size_t words = 2 + 3 * ctx.BufferDescriptorA().size();
        const u64 pts = u64{w[words]} | (u64{w[words + 1]} << 32);
        const bool end = (w[words + 2] & 0xFF) != 0;
        Log(ctx, "submit");
        Input input{{}, pts, end};
        if (!ctx.BufferDescriptorA().empty()) {
            const auto data = ctx.ReadBufferA(0);
            input.data.assign(data.begin(), data.end());
        }
        {
            std::scoped_lock lk{m_mutex};
            m_input_bytes += input.data.size();
            m_inputs.push_back(std::move(input));
            m_stream_ended = false;
            // Title 1 queues a whole movie after Start without waiting on the ready event. Every
            // frame is needed to decode the ones after it, so none is dropped.
            if (m_input_bytes > WarnInputBytes && !m_inputs_large) {
                m_inputs_large = true;
                LOG_WARNING(Service, "{} input queue holds {} MiB in {} frames", GetServiceName(),
                            m_input_bytes >> 20, m_inputs.size());
            }
            m_ready->room.store(m_inputs.size() < MaxQueuedInputs, std::memory_order_relaxed);
        }
        m_cv.notify_one();
        const u64 id = ++m_submitted;
        TipcReplyWords(ctx, {static_cast<u32>(id), static_cast<u32>(id >> 32)});
    }

    // Flush (8) and Stop (207): drop queued input and frames not yet in an image. On Flush, images
    // already handed out keep their place, so the sync values stay in step with the client's ring;
    // Stop completes them.
    void Flush(HLERequestContext& ctx) {
        Log(ctx, ctx.GetCommand() == 8 ? "flush" : "stop");
        if (ctx.GetCommand() == 207) {
            m_ready->SetRunning(system.Kernel(), false);
        }
        {
            std::scoped_lock lk{m_mutex};
            m_inputs.clear();
            m_input_bytes = 0;
            m_flush = true;
            m_stream_ended = false;
            m_clock.Reset();
            m_stream_start.reset();
            m_ready->room.store(true, std::memory_order_relaxed);
            // Only Stop: Donkey Kong Bananza flushes (8) after its movies end and treats every
            // completed fence as a frame to acquire.
            if (ctx.GetCommand() == 207) {
                CompleteHandedFences();
            }
        }
        m_cv.notify_one();
        TipcReplyWords(ctx, {});
    }

    s64 HostTimeUs() const {
        return system.CoreTiming().GetGlobalTimeUs().count();
    }

    // When emulation last resumed from a pause: the movie's audio stopped meanwhile.
    s64 PauseEndUs() const {
        return system.CoreTiming().pause_end_time.load(std::memory_order_relaxed) / 1000;
    }

    // EDEN_NX2_VDEC_PACE=0 keeps every decoded frame, however late.
    static bool PaceMovies() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_VDEC_PACE");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    // Games such as Ocarina of Time show one decoded frame per frame they draw and never skip
    // one, while the movie's audio plays in real time: below the movie's frame rate the picture
    // falls further behind the sound. Drop decoded frames a later frame has already overtaken on
    // the movie clock (m_mutex held). Returns true when the only frame left is late and more input
    // is queued: decode on before filling an image.
    bool DropOvertakenFrames() {
        if (m_decoded.empty() || !PaceMovies()) {
            return false;
        }
        const auto due = m_clock.Due(HostTimeUs(), PauseEndUs());
        if (!due) {
            m_dropped_in_a_row = 0;
            return false;
        }
        while (m_decoded.size() >= 2 && m_dropped_in_a_row < MaxDroppedInARow) {
            const Decoded& front = m_decoded[0];
            const Decoded& next = m_decoded[1];
            if (front.end || !front.picture || next.pts <= front.pts || next.pts > *due) {
                break;
            }
            m_decoded.pop_front();
            ++m_frames_dropped;
            ++m_dropped_in_a_row;
        }
        const Decoded& front = m_decoded.front();
        return m_decoded.size() == 1 && !m_inputs.empty() && !front.end &&
               m_dropped_in_a_row < MaxDroppedInARow && front.pts + m_frame_interval < *due;
    }

    // Advances the decoder's syncpoint to the last value handed out (m_mutex held), so a client
    // waiting on images it gave back after a movie ended is released when it stops the decoder.
    // Those images count as filled: they leave the free list, and an image the decode thread is
    // writing right now doesn't increment the syncpoint again.
    void CompleteHandedFences() {
        if (m_syncpoint == NoSyncpoint) {
            return;
        }
        m_free.clear();
        m_release_fences.clear();
        ++m_fence_generation;
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        const u32 target = m_sync_base + m_handed;
        u32 current = syncpoints.GetGuestSyncpointValue(m_syncpoint);
        while (static_cast<s32>(target - current) > 0) {
            syncpoints.IncrementGuest(m_syncpoint);
            ++current;
        }
    }

    void GetOutputInfo(HLERequestContext& ctx) {
        Log(ctx, "output info");
        std::optional<Output> output;
        {
            std::scoped_lock lk{m_mutex};
            if (!m_outputs.empty()) {
                output = m_outputs.front();
                m_outputs.pop_front();
                m_clock.Acquired(HostTimeUs(), output->pts, PauseEndUs());
            }
        }
        if (!output) {
            if (VideoTrace()) {
                LOG_INFO(Service, "{} trace output info: none finished, 0x1c286f (2111-3604)",
                         GetServiceName());
            }
            // Nothing finished: the code the client's own Acquire uses for an empty ring.
            IPC::ResponseBuilder rb{ctx, 2};
            rb.PushRaw<u32>(0x1c286f);
            return;
        }
        if (VideoTrace()) {
            LOG_INFO(Service, "{} trace output info: image {} {}x{} pts {} eos {}",
                     GetServiceName(), output->image, output->width, output->height, output->pts,
                     output->end);
        }
        const u64 pts_ns = output->pts * 1000;
        TipcReplyWords(ctx, {output->image, output->width, output->height, output->width,
                             output->height, 0, static_cast<u32>(pts_ns),
                             static_cast<u32>(pts_ns >> 32), output->end ? 1U : 0U, 0, 0, 0, 0,
                             0, 0, 0});
    }

    // The decode thread: turns queued input into decoded frames and decoded frames into output
    // images, in order.
    void DecodeLoop() {
        Common::SetCurrentThreadName("nx2:vdec");
        std::unique_ptr<Tegra::Host1x::Nx2VideoDecoder> decoder;
        std::vector<Tegra::Host1x::Nx2Picture> pictures;
        std::vector<u8> bytes;
        std::unique_lock lk{m_mutex};
        while (true) {
            m_cv.wait(lk, [&] {
                return m_quit || m_flush || (!m_decoded.empty() && !m_free.empty()) ||
                       (!m_inputs.empty() && m_decoded.size() < MaxDecodedAhead);
            });
            if (m_quit) {
                return;
            }
            if (m_flush) {
                m_flush = false;
                m_decoded.clear();
                m_last_decoded_pts = 0;
                m_dropped_in_a_row = 0;
                if (decoder) {
                    lk.unlock();
                    decoder->Reset();
                    lk.lock();
                }
                continue;
            }
            if (!m_decoded.empty() && !m_free.empty() && !DropOvertakenFrames()) {
                m_dropped_in_a_row = 0;
                Decoded frame = std::move(m_decoded.front());
                m_decoded.pop_front();
                const u32 image = m_free.front();
                m_free.pop_front();
                const u64 generation = m_fence_generation;
                std::optional<Fence> fence;
                if (const auto it = m_release_fences.find(image); it != m_release_fences.end()) {
                    fence = it->second;
                    m_release_fences.erase(it);
                }
                lk.unlock();
                if (fence) {
                    WaitForRelease(*fence);
                }
                const auto [width, height] = WriteImage(decoder.get(), image, frame, bytes);
                lk.lock();
                // Stop or Finalize already completed this image's fence.
                if (generation != m_fence_generation) {
                    continue;
                }
                // Even an image written during a flush counts: the client waits for its value.
                m_outputs.push_back({image, width, height, frame.pts, frame.end});
                if (frame.end) {
                    m_stream_ended = true;
                }
                // A client acquiring from its own ring never asks with 213: keep the newest only.
                if (m_outputs.size() > MaxHeldOutputs) {
                    m_outputs.pop_front();
                    if (!m_outputs_dropped) {
                        m_outputs_dropped = true;
                        LOG_INFO(Service, "{} keeps only the last {} finished frames for 213",
                                 GetServiceName(), MaxHeldOutputs);
                    }
                }
                system.Host1x().GetSyncpointManager().IncrementGuest(m_syncpoint);
                m_last_output = {image, width, height, frame.pts, frame.end};
                if (frame.end && m_inputs.empty() && m_decoded.empty() && EosDrain()) {
                    DrainFreeAsEndOfStream();
                }
                ++m_frames_out;
                Common::NoteVideoFrame();
                const s64 written_us = HostTimeUs();
                if (!m_stream_start) {
                    m_stream_start = {written_us, frame.pts};
                }
                if (m_frames_out == 1 || (m_frames_out % 600) == 0 || frame.end) {
                    // How far the frames written lag their timestamps since the stream's first.
                    const s64 behind_us =
                        (written_us - m_stream_start->first) -
                        (static_cast<s64>(frame.pts) - static_cast<s64>(m_stream_start->second));
                    LOG_INFO(Service,
                             "{} frame {} ({}x{}, pts {} us{}) in image {}, {} late dropped, {} ms "
                             "behind",
                             GetServiceName(), m_frames_out, width, height, frame.pts,
                             frame.end ? ", end of stream" : "", image, m_frames_dropped,
                             behind_us / 1000);
                }
                if (frame.end) {
                    m_stream_start.reset();
                }
                continue;
            }
            Input input = std::move(m_inputs.front());
            m_inputs.pop_front();
            // Its frames are on the way: a release while it decodes isn't after the end.
            m_stream_ended = false;
            m_input_bytes -= input.data.size();
            m_ready->room.store(m_inputs.size() < MaxQueuedInputs, std::memory_order_relaxed);
            std::string codec = m_codec;
            if (codec.empty() && Tegra::Host1x::IsH264Stream(input.data)) {
                codec = "video/avc";
                m_codec = codec;
                LOG_INFO(Service, "{} detected H.264 for an unnamed codec", GetServiceName());
            }
            lk.unlock();
            if (!decoder || decoder->Name() != codec) {
                decoder = std::make_unique<Tegra::Host1x::Nx2VideoDecoder>(codec);
            }
            pictures.clear();
            decoder->Decode(input.data, input.pts, pictures);
            if (input.end) {
                decoder->Drain(pictures);
            }
            lk.lock();
            if (m_flush) {
                continue;
            }
            for (auto& picture : pictures) {
                const u64 pts = picture.pts;
                if (pts > m_last_decoded_pts && pts - m_last_decoded_pts <= 100'000) {
                    m_frame_interval = pts - m_last_decoded_pts;
                }
                m_last_decoded_pts = pts;
                m_decoded.push_back({std::move(picture), pts, false});
            }
            if (input.end) {
                // The client stops at the frame marked end of stream; give it one even when the
                // last input held no picture. Only a picture from this input may carry the mark:
                // marking an earlier one still waiting for an image hands out one frame fewer
                // than the client counts, and its next 213 finds the ring empty (0x1c286f), which
                // Donkey Kong Bananza panics on as a video ends.
                if (pictures.empty()) {
                    m_decoded.push_back({std::nullopt, input.pts, true});
                } else {
                    m_decoded.back().end = true;
                }
            }
        }
    }

    // EDEN_NX2_VDEC_EOS_DRAIN (off by default): once a stream has ended and nothing is queued,
    // every free image, and every image released after that, goes back as another end-of-stream
    // frame with its own syncpoint increment. Mario Kart World releases its images after a movie
    // and acquires a frame for each completed fence; without one it gets the empty ring code
    // (2111-3604) and panics. Donkey Kong Bananza aborts when given frames it didn't ask for, so
    // =1 does this for Mario Kart World only and =2 for every title.
    bool EosDrain() const {
        static const int mode = [] {
            const char* value = std::getenv("EDEN_NX2_VDEC_EOS_DRAIN");
            return value == nullptr ? 0 : value[0] - '0';
        }();
        return mode == 2 || (mode == 1 && system.GetApplicationProcessProgramID() ==
                                              Loader::MarioKartWorldProgramId);
    }

    static bool VideoTrace() {
        static const bool enabled = std::getenv("EDEN_NX2_VDEC_TRACE") != nullptr;
        return enabled;
    }

    // Hands every free image back as an end-of-stream frame (m_mutex held, no image being
    // written); returns the syncpoint value reached. The images were handed out in m_free's
    // order, so each increment reaches the value given for the next one.
    u32 DrainFreeAsEndOfStream() {
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        if (VideoTrace()) {
            LOG_INFO(Service, "{} trace end of stream: {} free images handed back",
                     GetServiceName(), m_free.size());
        }
        while (!m_free.empty()) {
            const u32 image = m_free.front();
            m_free.pop_front();
            m_release_fences.erase(image);
            m_outputs.push_back(
                {image, m_last_output.width, m_last_output.height, m_last_output.pts, true});
            if (m_outputs.size() > MaxHeldOutputs) {
                m_outputs.pop_front();
            }
            syncpoints.IncrementGuest(m_syncpoint);
        }
        return syncpoints.GetGuestSyncpointValue(m_syncpoint);
    }

    // Waits (bounded) until the client's reads of an image it gave back have passed.
    void WaitForRelease(const Fence& fence) {
        if (fence.syncpoint >= HostSyncpoints) {
            return;
        }
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
        while (!syncpoints.IsReadyGuest(fence.syncpoint, fence.value) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    // Writes a decoded frame into an output image; returns the size the frame reports.
    std::pair<u32, u32> WriteImage(Tegra::Host1x::Nx2VideoDecoder* decoder, u32 image_id,
                                   const Decoded& frame, std::vector<u8>& bytes) {
        u64 address{};
        Nx2DecoderImageLayout layout{};
        std::optional<Nx2ImageLayout> tiled;
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(image_id);
            if (image == m_objects->images.end()) {
                return {m_width, m_height};
            }
            layout = Nx2CalculateDecoderImageLayout(image->second.preference.data());
            if (image->second.gpu_mapped &&
                layout.format != Tegra::Host1x::Nx2PictureFormat::Nv12) {
                // The texture NVN builds from gsv 426's descriptor is block-linear A8B8G8R8
                // (Welcome Tour's code 5 video image).
                tiled = Nx2CalculateImageLayout(image->second.preference.data());
                layout.format = Tegra::Host1x::Nx2PictureFormat::Rgba8;
            }
            const auto memory = m_objects->memories.find(image->second.memory);
            if (memory != m_objects->memories.end() && memory->second.address != 0) {
                address = memory->second.address + image->second.offset;
            }
        }
        if (!frame.picture || decoder == nullptr || address == 0 ||
            system.ApplicationProcess() == nullptr) {
            return {std::min(m_width, layout.width), std::min(m_height, layout.height)};
        }
        // A frame larger than the image is scaled down to it.
        const u32 width = std::min(frame.picture->width, layout.width);
        const u32 height = std::min(frame.picture->height, layout.height);
        bytes.resize(layout.size);
        decoder->Render(*frame.picture, layout.format, width, height, layout.pitch,
                        layout.chroma_offset, bytes);
        DumpFrame(bytes, layout, width, height);
        {
            // The image may have been destroyed or moved while the frame rendered: its memory
            // could hold something else by now.
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(image_id);
            const auto memory = image != m_objects->images.end()
                                    ? m_objects->memories.find(image->second.memory)
                                    : m_objects->memories.end();
            if (memory == m_objects->memories.end() ||
                memory->second.address + image->second.offset != address) {
                static std::atomic<u32> logged{};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
                    LOG_WARNING(Service, "{}: image {} went away while its frame rendered; not "
                                "written", GetServiceName(), image_id);
                }
                return {width, height};
            }
        }
        auto& memory = system.ApplicationMemory();
        if (tiled && tiled->block_height_log2 >= 3 && tiled->pitch == layout.pitch) {
            // Blocks of 2^block_height_log2 rows in the console's GOB layout, over the whole
            // pitch (the rows Render wrote are layout.pitch bytes apart).
            m_tiled_bytes.resize(tiled->size);
            Tegra::Texture::SwizzleTexture(m_tiled_bytes, bytes, 4,
                                           static_cast<u32>(layout.pitch / 4), layout.height, 1,
                                           tiled->block_height_log2 - 3, 0, 1, true);
            memory.WriteBlock(address, m_tiled_bytes.data(), tiled->size);
        } else {
            memory.WriteBlock(address, bytes.data(), layout.pitch * height);
        }
        if (layout.format == Tegra::Host1x::Nx2PictureFormat::Nv12) {
            memory.WriteBlock(address + layout.chroma_offset, bytes.data() + layout.chroma_offset,
                              layout.pitch * ((height + 1) / 2));
        }
        // The game samples the image on the GPU: drop the texture cache's copy of the old frame.
        if (const auto device =
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(address)) {
            system.GPU().InvalidateRegion(*device, layout.size);
        }
        return {width, height};
    }

    // EDEN_NX2_VDEC_DUMP=<dir>: save the luma of every 120th decoded frame, a quarter of its size,
    // as nx2-vdec-<frame>.pgm (to check what the decoder wrote, apart from how the game shows it).
    void DumpFrame(const std::vector<u8>& bytes, const Nx2DecoderImageLayout& layout, u32 width,
                   u32 height) {
        static const char* const directory = std::getenv("EDEN_NX2_VDEC_DUMP");
        if (directory == nullptr || (m_frames_out % 120) != 0) {
            return;
        }
        const u32 bpp = layout.format == Tegra::Host1x::Nx2PictureFormat::Nv12 ? 1 : 4;
        const u32 w = width / 4;
        const u32 h = height / 4;
        const std::string path = fmt::format("{}/nx2-vdec-{:05}.pgm", directory, m_frames_out + 1);
        if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fprintf(file, "P5 %u %u 255\n", w, h);
            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    std::fputc(bytes[y * 4 * layout.pitch + x * 4 * bpp], file);
                }
            }
            std::fclose(file);
        }
    }

    // EDEN_NX2_VIDEO_READY_GATE=0 keeps signalling the ready event after Stop and Finalize, and
    // answers Start with a plain success, as before.
    static bool ReadyGate() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_VIDEO_READY_GATE");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    struct ReadyState {
        // Signals the event while the decoder runs and the input queue has room.
        void Signal(Kernel::KernelCore& kernel_core) {
            std::scoped_lock lk{mutex};
            if (event != nullptr && running && room.load(std::memory_order_relaxed)) {
                event->Signal(kernel_core);
            }
        }

        // Stop (207) and Finalize (115) stop the signals and clear a pending one, so a client
        // tearing the decoder down is not woken again; Start (6) resumes them.
        void SetRunning(Kernel::KernelCore& kernel_core, bool value) {
            if (!ReadyGate()) {
                return;
            }
            std::scoped_lock lk{mutex};
            running = value;
            if (event != nullptr && !running) {
                event->Clear(kernel_core);
            }
        }

        std::mutex mutex;
        Kernel::KEvent* event{};
        std::atomic<bool> room{true}; ///< The input queue takes more frames
        bool running{true};
    };

    KernelHelpers::ServiceContext m_context;
    std::shared_ptr<Nx2NativeObjects> m_objects;
    Kernel::KEvent* m_event{};
    std::shared_ptr<ReadyState> m_ready = std::make_shared<ReadyState>();
    std::shared_ptr<Core::Timing::EventType> m_ready_timer;
    u64 m_submitted{};
    u32 m_syncpoint{NoSyncpoint}; ///< Counts finished frames (allocated at the first register)
    u32 m_sync_base{};            ///< Its value before the first frame

    // Shared with the decode thread.
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_codec;
    u32 m_width{};
    u32 m_height{};
    u32 m_handed{}; ///< Output images handed out (registered or released) so far
    u64 m_fence_generation{}; ///< Bumped each time Stop or Finalize completes the handed fences
    u64 m_frames_out{};
    u64 m_frames_dropped{}; ///< Decoded frames DropOvertakenFrames skipped
    Nx2MovieClock m_clock;
    u64 m_frame_interval{16667}; ///< us, from consecutive decoded frames
    u64 m_last_decoded_pts{};
    u32 m_dropped_in_a_row{};
    std::optional<std::pair<s64, u64>> m_stream_start; ///< Host time (us) and pts of its first frame
    std::vector<u8> m_tiled_bytes; ///< WriteImage's block-linear copy of a GPU-mapped frame
    std::deque<Input> m_inputs;
    std::deque<Decoded> m_decoded;     ///< Waiting for a free output image
    std::deque<u32> m_free;            ///< Output images, in the order the client acquires them
    std::map<u32, Fence> m_release_fences;
    std::deque<Output> m_outputs;      ///< Finished, for 213
    Output m_last_output{};            ///< The last frame put in an image (EosDrain copies it)
    size_t m_input_bytes{};            ///< Compressed bytes in m_inputs
    bool m_inputs_large{};             ///< Logged that m_inputs passed WarnInputBytes
    bool m_outputs_dropped{};          ///< Logged that 213 isn't keeping up
    bool m_stream_ended{};             ///< End-of-stream frame handed out, nothing submitted since
    bool m_flush{};
    bool m_quit{};
    std::thread m_worker;
};

// Placeholder for the Switch 2 graphics-system server that nn::gsv::BootWithRemoteMemory talks
// to over TIPC. Nothing behind it is emulated: commands succeed and are logged, so boot can show
// what comes next. Commands 202 and 1402 hand out the syncpt and nvsrv sessions the SDK uses.
class INx2GraphicsSystemSession final : public ServiceFramework<INx2GraphicsSystemSession> {
public:
    explicit INx2GraphicsSystemSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gsv"}, m_context{system_, "nx2:gsv"} {
        RegisterHandlersTipc({
            {4, &INx2GraphicsSystemSession::Stub, "BootWithRemoteMemory"},
            {5, &INx2GraphicsSystemSession::Stub, "Unknown5"},
            {101, &INx2GraphicsSystemSession::Stub, "CreateContext"},
            {202, &INx2GraphicsSystemSession::OpenSyncptSession, "InitializeSyncptContext"},
            {204, &INx2GraphicsSystemSession::AttachSyncptMemory, "AttachSyncptMemory (guessed)"},
            {305, &INx2GraphicsSystemSession::OpenViSession, "InitializeDisplayContext"},
            {497, &INx2GraphicsSystemSession::OpenGpuSession, "CreateGpuSessionWithRemoteMemory"},
            {111, &INx2GraphicsSystemSession::CreateNativeMemory, "CreateNativeMemory"},
            {112, &INx2GraphicsSystemSession::DestroyNativeMemory, "DestroyNativeMemory"},
            {114, &INx2GraphicsSystemSession::BindNativeMemory, "CreateNativeMemoryBlock"},
            {314, &INx2GraphicsSystemSession::ConfigureWindowImagePool, "DestroyNativeWindowAliasImagePool"},
            {315, &INx2GraphicsSystemSession::ConfigureWindowImagePool, "CreateNativeWindowAliasImagePool"},
            {423, &INx2GraphicsSystemSession::BindNativeGpuMemory, "BindNativeGpuMemory"},
            {429, &INx2GraphicsSystemSession::CreateNativeImage, "CreateNativeImage"},
            {430, &INx2GraphicsSystemSession::DestroyNativeImage, "DestroyNativeImage"},
            {426, &INx2GraphicsSystemSession::QueryNativeImage, "MapNativeImage"},
            {427, &INx2GraphicsSystemSession::UnmapNativeImage, "UnmapNativeImage"},
            {428, &INx2GraphicsSystemSession::QueryNativeImage, "QueryNativeImage"},
            {435, &INx2GraphicsSystemSession::CalculateNativeImageSize, "CalculateNativeImageSize"},
            {1402, &INx2GraphicsSystemSession::OpenNvsrvSession, "CreateNvDrv2UserSessionWithRemoteMemory"},
            {1510, &INx2GraphicsSystemSession::CalculateRequirements, "CalculateRequirements"},
            {991, &INx2GraphicsSystemSession::CalculateDecoderImagePoolBuffer,
             "CalculateDecoderImagePoolBuffer"},
            {901, &INx2GraphicsSystemSession::Stub, "CreateVideoDecoderContext"},
            {902, &INx2GraphicsSystemSession::OpenVideoSession, "InitializeVideoDecoderContext"},
            {905, &INx2GraphicsSystemSession::OpenVideoSession, "InitializeVideoDecoderContext2"},
            {906, &INx2GraphicsSystemSession::OpenVideoSession, "OpenVideoDecoderSessionWithVersion"},
            {1001, &INx2GraphicsSystemSession::Stub, "CreateVideoEncoderContext"},
            {1002, &INx2GraphicsSystemSession::OpenVideoSession, "InitializeVideoEncoderContext"},
            {601, &INx2GraphicsSystemSession::Stub, "CreateCameraContext"},
            {602, &INx2GraphicsSystemSession::OpenCameraSession, "InitializeCameraContext"},
            {1121, &INx2GraphicsSystemSession::OpenSession1121, "OpenSession1121 (guessed)"},
            {1201, &INx2GraphicsSystemSession::Stub, "CreateChatContext"},
            {1202, &INx2GraphicsSystemSession::OpenChatSession, "InitializeChatContext"},
            // Same request as 429: nn::chat::gsv::CreateImagePool (GameChat camera images).
            {1211, &INx2GraphicsSystemSession::CreateNativeImage, "CreateImagePool"},
            {1291, &INx2GraphicsSystemSession::CalculateChatImagePoolSize,
             "CalculateRequiredBufferMemory"},
            {911, &INx2GraphicsSystemSession::CreateDecoderImage, "InitializeDecoderImagePool"},
            // 912(u32 image id): the video group's x12, freeing one decoder output image (Switch
            // Sports frees its four when its movie ends). Unlisting it stops the decode thread
            // writing frames into memory the game reuses.
            {912, &INx2GraphicsSystemSession::DestroyNativeImage, "FinalizeDecoderImagePool"},
            {961, &INx2GraphicsSystemSession::GetCpuImageDescriptor, "GetCpuUserImageDescriptor"},
        });
        if (Nx2Vi2ClientState()) {
            RegisterHandlersTipc({
                {115, &INx2GraphicsSystemSession::ConfigureNativeMemory, "BootNativeMemoryManagement"},
            });
        }
    }

    ~INx2GraphicsSystemSession() override {
        if (m_syncpt_poll) {
            system.CoreTiming().UnscheduleEvent(m_syncpt_poll);
        }
        // Slot actions of the syncpt sessions can still run: stop their page writes.
        {
            std::scoped_lock lk{m_syncpt_page->mutex};
            m_syncpt_page->memory = nullptr;
        }
        for (auto* event : m_syncpt_events) {
            if (event != nullptr) {
                m_context.CloseEvent(event);
            }
        }
    }

private:
    // Index of the first raw data word of a TIPC request: header (2 words), then an optional
    // special header with pid and copy/move handle words.
    static size_t TipcDataOffset(const u32* cmd_buf) {
        if ((cmd_buf[1] & 0x80000000) == 0) {
            return 2;
        }
        const u32 desc = cmd_buf[2];
        return 3 + ((desc & 1) ? 2 : 0) + ((desc >> 1) & 0xF) + ((desc >> 5) & 0xF);
    }

    void Stub(HLERequestContext& ctx) {
        const auto* cmd_buf = ctx.CommandBuffer();
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC {} [2]={:#x} [3]={:#x} [4]={:#x} [5]={:#x}",
                    ctx.GetCommand(), cmd_buf[2], cmd_buf[3], cmd_buf[4], cmd_buf[5]);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // 115(u32 mode, u32): replies a u64 optional memory mode; 0 keeps the nvdrv-compatible path.
    // The auto stub left the request's words where the SDK reads it (Bananza read
    // 0x000E81E6_00000000).
    void ConfigureNativeMemory(HLERequestContext& ctx) {
        TipcReplyWords(ctx, {0, 0});
    }

    // nn::codec::video::detail::ipc::Initialize sends 901, 902, 1001, 1002. 902 and 1002 return
    // a session handle at word 3 and the result at word 4; left to the auto stub, the handle was 0
    // and InitializeVp9Decoder failed with 2001-0114 (title 1 aborts when a movie starts).
    // Newer SDKs (nn::codec::video::detail::InitializeServerContext) send 906(u8, u64 decoder
    // library version) instead of 902; title 4 sends version 0x3'00030000 and aborted with
    // InvalidHandle at its first movie while 906 was auto-stubbed. The SDK of later titles
    // (ipc::Initialize(bool)) sends 905(u8) instead of 902 when its flag is set; the reply is the
    // same. 1001/1002 open the encoder's creator session (CreateVideoEncoderService uses it).
    void OpenVideoSession(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const bool second = ctx.GetCommand() == 1002;
        LOG_INFO(Service, "nx2:gsv TIPC {} [2]={:#x} [3]={:#x} [4]={:#x}, returning a video decoder session",
                 ctx.GetCommand(), w[2], w[3], w[4]);
        ReturnClientOwnedRawSession(
            system, ctx,
            std::make_shared<INx2VideoSession>(system, m_objects, second ? "nx2:vdec2" : "nx2:vdec"));
    }

    // nn::camera::sdk::CreateCameraContext sends 601() then 602(u64 1), which returns the camera
    // context session (handle at word 3, result at word 4). Auto-stubbed, 602 had no handle, the
    // SDK sent 603 (destroy) and title 1 (1.8.0) aborted with 2034-0000. Its commands fall
    // through to the nx2 auto stub.
    void OpenCameraSession(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 602, returning a nx2:camera session");
        ReturnClientOwnedRawSession(system, ctx,
                                    std::make_shared<INx2StubInterface>(
                                        system, INx2StubInterface::InternName("nx2:camera")));
    }

    void OpenSession1121(HLERequestContext& ctx) {
        LOG_INFO(Service, "nx2:gsv TIPC 1121, returning a session");
        ReturnRawSession(system, ctx, std::make_shared<INx2GsvSession1121>(system));
    }

    // nn::chat::gsv::Initialize sends 1201 then 1202, the same pair as 901/902 and 1001/1002.
    // Left to the auto stub it aborted with 2162-0001 (GameChat), so 1202 gets a session too.
    void OpenChatSession(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 1202, returning a nx2:chat session");
        ReturnRawSession(system, ctx, std::make_shared<INx2ChatGsvSession>(system));
    }

    void OpenSyncptSession(HLERequestContext& ctx) {
        // Used by nn::syncpt::sdk::InitializeEx; handle at word 3, result at word 4.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 202, returning a nx2:syncpt session");
        ReturnRawSession(system, ctx, std::make_shared<INx2SyncptSession>(system, m_syncpt_page));
    }

    // nn::syncpt::sdk::InitializeEx sends two transfer memories (0x1000 bytes RW, then 0x2000
    // bytes the client maps read-only) and expects the server to have written a header into the
    // second one: u32 version (must be 1) at +0, u32 base id at +4, u32 count at +8 (the client
    // requires ((count + 0x40) >> 6) - 1 < 0x40), then offsets of: the u64 syncpoint values (RO,
    // +0xc), the RW "armed" summary word (+0x10), the RO summary word (+0x14), the RW armed bits
    // (+0x18) and the RO acknowledge bits (+0x1c).
    // To wait, the client makes its RW bit differ from the RO bit, signals the first returned
    // event (it attaches that handle as writable), and waits on the second until the server makes
    // the bits equal again; it then rereads the value and rearms if the threshold isn't reached.
    // Reply: the two handles (words 3 and 4), then the result.
    static constexpr u32 SyncptValuesOffset = Nx2SyncptPage::ValuesOffset; // RO
    static constexpr u32 SyncptRoSummaryOffset = 0x1040;
    static constexpr u32 SyncptRoBitsOffset = 0x1100; // RO, up to 0x40 u64 words
    static constexpr u32 SyncptRwSummaryOffset = 0x0;
    static constexpr u32 SyncptRwBitsOffset = 0x100;  // RW, up to 0x40 u64 words
    static constexpr u32 HostSyncpoints = 192;        // Tegra::Host1x::SyncpointManager

    void AttachSyncptMemory(HLERequestContext& ctx) {
        // RequestParser doesn't skip TIPC handle words, so read the raw data words.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        const u32 version_requested = cmd_buf[data];
        const u32 max_syncpoints = std::min<u32>(cmd_buf[data + 1], (0x1000 - 0x40) / 8);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 204, version={} max_syncpoints={:#x}",
                    version_requested, max_syncpoints);

        auto rw = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(0));
        auto ro = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(1));
        if (rw.IsNotNull() && ro.IsNotNull() && ro->GetSize() >= 0x2000) {
            m_syncpt_memory = &ro->GetOwner()->GetMemory();
            m_syncpt_rw = GetInteger(rw->GetSourceAddress());
            m_syncpt_ro = GetInteger(ro->GetSourceAddress());
            m_syncpt_count = max_syncpoints;
            auto& memory = *m_syncpt_memory;
            memory.Write32(m_syncpt_ro + 0x00, 1); // version
            memory.Write32(m_syncpt_ro + 0x04, 0); // base id (guess)
            memory.Write32(m_syncpt_ro + 0x08, max_syncpoints);
            memory.Write32(m_syncpt_ro + 0x0c, SyncptValuesOffset);
            memory.Write32(m_syncpt_ro + 0x10, SyncptRwSummaryOffset);
            memory.Write32(m_syncpt_ro + 0x14, SyncptRoSummaryOffset);
            memory.Write32(m_syncpt_ro + 0x18, SyncptRwBitsOffset);
            memory.Write32(m_syncpt_ro + 0x1c, SyncptRoBitsOffset);
            {
                std::scoped_lock lk{m_syncpt_page->mutex};
                m_syncpt_page->memory = m_syncpt_memory;
                m_syncpt_page->ro = m_syncpt_ro;
                m_syncpt_page->count = m_syncpt_count;
            }
            UpdateSyncptValues();
            if (!m_syncpt_poll) {
                m_syncpt_poll = Core::Timing::CreateEvent(
                    "nx2:gsv:SyncptPoll",
                    [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                        PollSyncpts();
                        return std::nullopt;
                    });
                system.CoreTiming().ScheduleLoopingEvent(SyncptPollInterval, SyncptPollInterval,
                                                         m_syncpt_poll);
            }
        } else {
            LOG_ERROR(Service, "nx2:gsv TIPC 204: missing syncpt transfer memory");
        }

        if (m_syncpt_events[0] == nullptr) {
            m_syncpt_events[0] = m_context.CreateEvent("nx2:gsv:SyncptRequest");
            m_syncpt_events[1] = m_context.CreateEvent("nx2:gsv:SyncptNotify");
        }
        IPC::ResponseBuilder rb{ctx, 2, 2, 0};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, *m_syncpt_events[0]); // signalled by the client
        rb.PushCopyObjects(ctx, m_syncpt_events[1]->GetReadableEvent());
    }

    static bool SyncptValuesFirst() {
        static const bool values_first = [] {
            const char* value = std::getenv("EDEN_NX2_SYNCPT_VALUES_FIRST");
            return value != nullptr && value[0] == '1';
        }();
        return values_first;
    }

    // Test only: EDEN_NX2_SYNCPT_POLL_DELAY_US=<us> (at most 400, under the poll interval) holds
    // each poll that long between its two reads, to widen the window PollSyncpts describes. It
    // needs multicore, where the poll runs on the HostTiming thread, and delays every wait it
    // answers, so frame times from such a run mean nothing.
    static void SyncptPollTestDelay() {
        static const u32 delay_us = [] {
            const char* value = std::getenv("EDEN_NX2_SYNCPT_POLL_DELAY_US");
            const unsigned long parsed = value != nullptr ? std::strtoul(value, nullptr, 10) : 0;
            return static_cast<u32>((std::min)(parsed, 400UL));
        }();
        if (delay_us == 0) [[likely]] {
            return;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds{delay_us};
        while (std::chrono::steady_clock::now() < until) {
        }
    }

    // Mirrors Eden's guest syncpoint values into the shared RO page. Returns true if any changed.
    bool UpdateSyncptValues() {
        auto& manager = system.Host1x().GetSyncpointManager();
        std::scoped_lock lk{m_syncpt_page->mutex};
        bool changed = false;
        for (u32 id = 0; id < std::min(m_syncpt_count, HostSyncpoints); ++id) {
            if (m_syncpt_page->UpdateLocked(manager, id)) {
                changed = true;
            }
        }
        return changed;
    }

    // Answers every armed wait: the value page is current, so the client rechecks its threshold
    // and rearms if it still has to wait. Acknowledging without a value change costs the client a
    // wakeup per poll, which keeps waits whose syncpoint we never see from hanging.
    // The armed bits are read before the values, so every answer carries values read after the
    // wait was armed; a wait armed later is answered by the next poll. The other way round, a
    // wait armed between the two reads got values from before it: title 1's movie player arms
    // right after its slot event fires, takes the miss as fatal and aborted (2162-0001) about
    // once per two minutes of its 640x360 movie. EDEN_NX2_SYNCPT_VALUES_FIRST=1 reads the values
    // first, as before.
    void PollSyncpts() {
        if (m_syncpt_memory == nullptr) {
            return;
        }
        if (SyncptValuesFirst()) {
            UpdateSyncptValues();
            SyncptPollTestDelay();
        }
        const u32 words = (m_syncpt_count + 63) / 64;
        std::array<u64, 0x40> armed{};
        for (u32 w = 0; w < words; ++w) {
            armed[w] = m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwBitsOffset + w * 8);
        }
        const u64 armed_summary = m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwSummaryOffset);
        if (!SyncptValuesFirst()) {
            SyncptPollTestDelay();
            std::atomic_thread_fence(std::memory_order_seq_cst);
            UpdateSyncptValues();
        }
        bool acked = false;
        for (u32 w = 0; w < words; ++w) {
            const u64 ro_address = m_syncpt_ro + SyncptRoBitsOffset + w * 8;
            if (m_syncpt_memory->Read64(ro_address) != armed[w]) {
                m_syncpt_memory->Write64(ro_address, armed[w]);
                acked = true;
            }
        }
        m_syncpt_memory->Write64(m_syncpt_ro + SyncptRoSummaryOffset, armed_summary);
        if (acked) {
            m_syncpt_events[1]->Signal(system.Kernel());
        }
    }

    // nn::gsv::shim::CalculateRequirements(u64* size, u64* alignment, preference, u32): the 0x40-byte
    // InterModuleUserImagePreference (words 2..17) and a u32 (word 18). A block-linear preference
    // (MakeInterModuleBlockUserImagePreference) holds kind 2 at word 3, (width - 1) | (height - 1)
    // << 16 at word 4, two u32s at words 6-7 and the pixel format at word 8. Reply: result, u64
    // size, u64 alignment; the client raises the alignment to at least 4 KiB and rounds the size.
    void CalculateRequirements(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        auto layout = Nx2CalculateImageLayout(w + 2);
        // Star Fox's NVN window textures use 4 KiB pool offsets. Reporting the block-linear
        // layout's 8 KiB alignment makes NativeWindow::Acquire reject those textures.
        constexpr u64 StarFoxProgramId = 0x0400E88025B1A000ULL;
        if (system.GetApplicationProcessProgramID() == StarFoxProgramId) {
            layout.alignment = 0x1000;
        }
        LOG_DEBUG(Service, "nx2:gsv 1510 kind={} {}x{} block={} pitch={:#x} format={} -> size={:#x} align={:#x}",
                  w[3], layout.width, layout.height, layout.block_height_log2, layout.pitch, w[8],
                  layout.size, layout.alignment);
        TipcReplyWords(ctx, {static_cast<u32>(layout.size), static_cast<u32>(layout.size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    // nn::codec::CalculateDecoderImagePoolBuffer(size*, alignment*, VideoDecoderImagePreference,
    // u32 count) sends 991(preference[16], u32 count) and reads result, u64 size, u64 alignment;
    // the client raises the alignment to at least 4 KiB and rounds the size up to it. The
    // preference (MakeDecodeImagePreference) holds an output layout code at word 1 (1/4 for
    // format 3, which the movie player uses, read here as NV12) and (width - 1) | (height - 1) << 16
    // at word 2. Left to the auto stub, the reply words keep the request's, so the size became
    // 0x167027f'00000001 and title 1's movie player aborted (main+0x19e5b58) when its first movie
    // started. Sizes are block-linear NV12 (luma, then half-height chroma, 128-row blocks) or 4
    // bytes per pixel for the other codes; nothing is decoded into them yet.
    void CalculateDecoderImagePoolBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateDecoderImageLayout(w + 2);
        const u32 count = std::max<u32>(w[18], 1);
        const u64 size = layout.size * count;
        const u64 alignment = 64 * 128;
        LOG_INFO(Service, "nx2:gsv 991 decoder image pool code={} {}x{} count={} -> size={:#x}",
                 layout.code, layout.width, layout.height, count, size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32),
                             static_cast<u32>(alignment), static_cast<u32>(alignment >> 32)});
    }

    // 435(preference[16], u32 count): result, u64 size of a GPU native image.
    void CalculateNativeImageSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Nx2CalculateImageLayout(w + 2).size * std::max<u32>(w[18], 1);
        LOG_DEBUG(Service, "nx2:gsv 435 count={} -> size={:#x}", w[18], size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    static u64 Pair(const u32* w, size_t i) {
        return u64{w[i]} | (u64{w[i + 1]} << 32);
    }

    // 111(u8 mode at word 2, u64 size): result, u32 native memory id.
    void CreateNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Pair(w, 3);
        std::scoped_lock lk{m_objects->mutex};
        const u32 id = m_objects->next_memory++;
        m_objects->memories[id] = {size, 0};
        LOG_INFO(Service, "nx2:gsv 111 CreateNativeMemory mode={} size={:#x} -> id {}", w[2] & 0xFF,
                 size, id);
        TipcReplyWords(ctx, {id});
    }

    void DestroyNativeMemory(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        std::vector<std::pair<u64, u64>> unmaps;
        {
            std::scoped_lock lk{m_objects->mutex};
            for (auto& [image_id, image] : m_objects->images) {
                if (image.memory == id) {
                    TakeDecoderMapping(image, unmaps);
                }
            }
            m_objects->memories.erase(id);
        }
        DropDecoderMappings(unmaps);
        LOG_DEBUG(Service, "nx2:gsv 112 DestroyNativeMemory id {}", id);
        TipcReplyWords(ctx, {});
    }

    // 114(u32 id, u64 address, u64 size, u64 flags, u64 offset): back native memory with process
    // memory.
    void BindNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "114");
        TipcReplyWords(ctx, {});
    }

    // 423(u32 id, u64 address, u64 size, u32 parent GpuMemoryHandle, u64 offset, ...): back
    // native memory with part of GPU-accessible memory.
    void BindNativeGpuMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_DEBUG(Service, "nx2:gsv 423 parent={} offset={:#x}", w[7], Pair(w, 8));
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "423");
        TipcReplyWords(ctx, {});
    }

    void BindMemory(u32 id, u64 address, u64 size, const char* command) {
        std::scoped_lock lk{m_objects->mutex};
        auto& memory = m_objects->memories[id];
        memory.address = address;
        if (memory.size == 0) {
            memory.size = size;
        }
        LOG_INFO(Service, "nx2:gsv {} bind native memory {} at {:#x}+{:#x}", command, id, address,
                 size);
    }

    // 429(preference[16], u32 count, u32 memory id): result, u32 native image id.
    void CreateNativeImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        std::scoped_lock lk{m_objects->mutex};
        const u32 id = m_objects->next_image;
        auto& image = m_objects->images[id];
        image.count = std::max<u32>(w[18], 1);
        // nn::gs::sdk::CalculateNativeImageHandle(pool, i) is pool + i, so keep those free.
        m_objects->next_image += image.count;
        image.memory = w[19];
        std::copy_n(w + 2, image.preference.size(), image.preference.begin());
        image.size = Nx2CalculateImageLayout(w + 2).size;
        LOG_INFO(Service, "nx2:gsv 429 CreateNativeImage memory={} count={} size={:#x} -> id {}",
                 w[19], w[18], image.size, id);
        TipcReplyWords(ctx, {id});
    }

    // nn::chat::gsv::CalculateRequiredBufferMemory: 1291(preference[16], u32 count), the request
    // of 1510, replies result, u64 size, u64 alignment. Auto-stubbed, the size became 0x1000 and
    // the camera pool (1211) got memory for none of its images.
    void CalculateChatImagePoolSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateImageLayout(w + 2);
        const u64 size = layout.size * std::max<u32>(w[18], 1);
        LOG_DEBUG(Service, "nx2:gsv 1291 {}x{} count={} -> size={:#x}", layout.width,
                  layout.height, w[18], size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    // 428(u32 native image handle) nn::gpu::sdk::QueryNativeImage and 426(u32 handle, u32, u32
    // address space, u8 permission) nn::gpu::sdk::MapNativeImage, used by NVN for images another
    // server created (GameChat's camera pool from 1211). Reply words 3-4 the GPU address of the
    // image (426 only; GPU == CPU address, see Nx2NativeObjects::Memory), 5-6 the u64 offset of
    // the image in its memory, 7-38 a GpuOunceUserImageDescriptor and 39 the flags. The SDK
    // requires descriptor byte 0x00 == 1, byte 0x20 == 1 and a non-zero size at 0x2c (offset at
    // 0x28); NVN reads (width - 1) | (height - 1) << 16 at 0x08 and the layout kind at 0x24 (1
    // takes RGBA formats 0x25/0x37/0x38, 2 the 0x99-0x9c formats). Block height and pitch use
    // the 429 preference words (0x10, 0x14), a guess.
    void QueryNativeImage(HLERequestContext& ctx) {
        const u32 handle = ctx.CommandBuffer()[2];
        std::array<u32, 16> preference{};
        u64 image_offset = 0;
        u64 memory_address = 0;
        std::optional<std::pair<u64, u64>> decoder_mapping;
        {
            std::scoped_lock lk{m_objects->mutex};
            auto it = m_objects->images.upper_bound(handle);
            if (it != m_objects->images.begin()) {
                --it;
                if (handle - it->first < it->second.count) {
                    preference = it->second.preference;
                    // Each image of a decoder pool has its own entry and offset (911).
                    image_offset = it->second.decoder ? it->second.offset
                                                      : (handle - it->first) * it->second.size;
                    if (const auto memory = m_objects->memories.find(it->second.memory);
                        memory != m_objects->memories.end()) {
                        memory_address = memory->second.address;
                    }
                    if (ctx.GetCommand() == 426) {
                        it->second.gpu_mapped = true;
                        if (it->second.decoder && memory_address != 0) {
                            decoder_mapping =
                                MapDecoderImage(it->second, memory_address + image_offset);
                        }
                    }
                }
            }
        }
        if (decoder_mapping) {
            {
                // The GPU thread takes the registry's lock too (BackSparse): queue its work after.
                Tegra::GPU::RunInOrderBatch batch;
                Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Register(decoder_mapping->first,
                                                                      decoder_mapping->second);
            }
            LOG_INFO(Service, "nx2:gsv 426 decoder image {} at {:#x}+{:#x} mapped for the GPU",
                     handle, decoder_mapping->first, decoder_mapping->second);
        }
        const auto layout = Nx2CalculateImageLayout(preference.data());
        const u64 gpu_address = ctx.GetCommand() == 426 ? memory_address + image_offset : 0;
        std::array<u32, 37> words{};
        words[0] = static_cast<u32>(gpu_address);
        words[1] = static_cast<u32>(gpu_address >> 32);
        words[2] = static_cast<u32>(image_offset);
        words[3] = static_cast<u32>(image_offset >> 32);
        u32* const desc = words.data() + 4;
        desc[0] = 1;
        desc[1] = preference[1];
        desc[2] = preference[2];
        desc[4] = layout.block_height_log2;
        desc[5] = static_cast<u32>(layout.pitch / 64);
        desc[8] = 1;
        desc[9] = 1;
        desc[11] = static_cast<u32>(layout.size);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv {} handle={} {}x{} size={:#x} gpu={:#x}",
                    ctx.GetCommand(), handle, layout.width, layout.height, layout.size,
                    gpu_address);
        IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
        rb.PushRaw<u32>(ResultSuccess.raw);
        for (const u32 w : words) {
            rb.Push<u32>(w);
        }
    }

    // A decoder pool's memory is process memory (114) that no GPU mapping may cover, while 426
    // hands out its CPU address as the GPU address: the first 426 of an image maps it there for
    // the GPU unless a mapping already covers it (Eglantine 9f22054c63: Fortune's Weave's movies
    // were black). Returns the range to register; called with m_objects->mutex held.
    // EDEN_NX2_DECODER_IMAGE_MAP=0 leaves the image unmapped, as before.
    std::optional<std::pair<u64, u64>> MapDecoderImage(Nx2NativeObjects::Image& image,
                                                       u64 address) {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_DECODER_IMAGE_MAP");
            return value == nullptr || value[0] != '0';
        }();
        if (image.gpu_maps++ != 0 || !enabled || image.size == 0 ||
            Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(address)) {
            return std::nullopt;
        }
        image.mapped_here = true;
        return std::pair{address, image.size};
    }

    // Adds the range 426 mapped for an image, if it did, to `unmaps`; called with
    // m_objects->mutex held.
    void TakeDecoderMapping(Nx2NativeObjects::Image& image,
                            std::vector<std::pair<u64, u64>>& unmaps) {
        image.gpu_maps = 0;
        if (!std::exchange(image.mapped_here, false)) {
            return;
        }
        const auto memory = m_objects->memories.find(image.memory);
        if (memory != m_objects->memories.end() && memory->second.address != 0) {
            unmaps.emplace_back(memory->second.address + image.offset, image.size);
        }
    }

    static void DropDecoderMappings(const std::vector<std::pair<u64, u64>>& unmaps) {
        if (unmaps.empty()) {
            return;
        }
        Tegra::GPU::RunInOrderBatch batch;
        auto& registry = Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
        for (const auto& [address, size] : unmaps) {
            registry.Unmap(address, size);
            registry.ReleaseBacking(address, size);
        }
    }

    // 427(u32 handle, u32 address space, u64 address, u64 last address): undoes a 426. The last
    // unmap of a decoder image drops the GPU mapping 426 made.
    void UnmapNativeImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 handle = w[2];
        std::vector<std::pair<u64, u64>> unmaps;
        {
            std::scoped_lock lk{m_objects->mutex};
            if (const auto image = m_objects->images.find(handle);
                image != m_objects->images.end() && image->second.gpu_maps != 0 &&
                --image->second.gpu_maps == 0) {
                TakeDecoderMapping(image->second, unmaps);
            }
        }
        LOG_DEBUG(Service, "nx2:gsv 427 image {} [{:#x}, {:#x}]", handle, Pair(w, 4), Pair(w, 6));
        DropDecoderMappings(unmaps);
        TipcReplyWords(ctx, {});
    }

    // 911(preference[16], u32 count, u32 memory id) (nn::codec::InitializeDecoderImagePool):
    // result, the id of the first of `count` decoder output images; the client names image i of
    // the pool id + i. They share the pool's memory, one image size apart.
    void CreateDecoderImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u32 count = std::max<u32>(w[18], 1);
        const auto layout = Nx2CalculateDecoderImageLayout(w + 2);
        std::scoped_lock lk{m_objects->mutex};
        const u32 first = m_objects->next_image;
        m_objects->next_image += count;
        for (u32 i = 0; i < count; ++i) {
            auto& image = m_objects->images[first + i];
            image.memory = w[19];
            std::copy_n(w + 2, image.preference.size(), image.preference.begin());
            image.size = layout.size;
            image.offset = layout.size * i;
            image.decoder = true;
        }
        // Destroying the pool's handle (430) removes all of its images.
        m_objects->images[first].count = count;
        LOG_INFO(Service, "nx2:gsv 911 decoder images {}..{} code={} {}x{} in memory {}", first,
                 first + count - 1, layout.code, layout.width, layout.height, w[19]);
        TipcReplyWords(ctx, {first});
    }

    // 961(u32 image id): result, the 128-byte nn::gsv::CpuUserImageDescriptor that
    // nn::gs::sdk::ReadUserImage* (behind nn::codec::ReadDecoderOutputImage*) copy an image out
    // with: u8 kind at +0; for the pitch-linear kinds (8 RGBA, 9 BGRA, 10 NV12) the plane's
    // offset into the pool memory (u32 +0x8) and row pitch (u64 +0x10), NV12's chroma plane at
    // +0x18/+0x20. 11-13 are the block-linear kinds, not used here.
    void GetCpuImageDescriptor(HLERequestContext& ctx) {
        using Tegra::Host1x::Nx2PictureFormat;
        const u32 id = ctx.CommandBuffer()[2];
        std::array<u32, 32> words{};
        {
            std::scoped_lock lk{m_objects->mutex};
            const auto image = m_objects->images.find(id);
            if (image != m_objects->images.end() && image->second.decoder) {
                const auto layout = Nx2CalculateDecoderImageLayout(image->second.preference.data());
                const u64 offset = image->second.offset;
                words[0] = layout.format == Nx2PictureFormat::Nv12    ? 0xa
                           : layout.format == Nx2PictureFormat::Bgra8 ? 0x9
                                                                      : 0x8;
                words[2] = static_cast<u32>(offset);
                words[4] = static_cast<u32>(layout.pitch);
                if (layout.format == Nx2PictureFormat::Nv12) {
                    words[6] = static_cast<u32>(offset + layout.chroma_offset);
                    words[8] = static_cast<u32>(layout.pitch);
                }
            }
        }
        if (Nx2FirstVideoCommand("nx2:gsv", 961) || words[0] == 0) {
            LOG_INFO(Service, "nx2:gsv 961 image {} -> kind {:#x} offset {:#x} pitch {:#x}", id,
                     words[0], words[2], words[4]);
        }
        TipcReplyWords(ctx, words);
    }

    // A pool's handle (429, or the first image of a 911 decoder pool) takes the pool's ids with it.
    void DestroyNativeImage(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        std::vector<std::pair<u64, u64>> unmaps;
        {
            std::scoped_lock lk{m_objects->mutex};
            auto& images = m_objects->images;
            if (auto it = images.find(id); it != images.end()) {
                const u32 count = it->second.count;
                do {
                    TakeDecoderMapping(it->second, unmaps);
                    it = images.erase(it);
                } while (it != images.end() && it->first - id < count);
            }
        }
        DropDecoderMappings(unmaps);
        TipcReplyWords(ctx, {});
    }

    // 315(u32 image, u32 count) / 314(u32 image): add/remove a window alias image pool.
    void ConfigureWindowImagePool(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        const bool create = ctx.GetCommand() == 315;
        std::scoped_lock lk{m_objects->mutex};
        if (auto it = m_objects->images.find(id); it != m_objects->images.end()) {
            it->second.window_pool = create;
        }
        LOG_INFO(Service, "nx2:gsv {} window image pool image {}", ctx.GetCommand(), id);
        TipcReplyWords(ctx, {});
    }

    void OpenViSession(HLERequestContext& ctx) {
        // Used by nn::vi2::sdk::Initialize; takes one u64, returns a session (handle at word 3,
        // result at word 4).
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 305 arg={:#x}, returning a nx2:vi2 session",
                    u64{cmd_buf[data]} | (u64{cmd_buf[data + 1]} << 32));
        ReturnRawSession(system, ctx, std::make_shared<INx2Vi2Session>(system, m_objects));
    }

    void OpenGpuSession(HLERequestContext& ctx) {
        // Used by nn::gpu::sdk::BootWithRemoteMemory; takes one u64, returns a session (handle at
        // word 3, result at word 4) that the GPU SDK keeps.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 497 arg={:#x}, returning a nx2:gpu session",
                    u64{cmd_buf[data]} | (u64{cmd_buf[data + 1]} << 32));
        ReturnRawSession(system, ctx, std::make_shared<INx2GpuSession>(system));
    }

    void OpenNvsrvSession(HLERequestContext& ctx) {
        // The SDK reads the handle at word 3 and the result at word 4.
        // nvsrv speaks the nvdrv INvDrvServices protocol (Open/Ioctl/Close/...,
        // 13 = SetGraphicsFirmwareMemoryMarginEnabled), so serve it with Eden's nvdrv on the shared
        // module. The nx2: name keeps unknown commands auto-stubbed and logged.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 1402, returning an nvdrv-backed nx2:nvsrv session");
        auto module = system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv", true)->GetModule();
        auto nvsrv = std::make_shared<Nvidia::NVDRV>(system, module, "nx2:nvsrv");
        nvsrv->InitializeForProcess(ctx.GetThread().GetOwnerProcess());
        ReturnRawSession(system, ctx, std::move(nvsrv));
    }

    static constexpr std::chrono::nanoseconds SyncptPollInterval{std::chrono::microseconds{500}};

    std::shared_ptr<Nx2NativeObjects> m_objects{std::make_shared<Nx2NativeObjects>()};
    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*, 2> m_syncpt_events{};
    Core::Memory::Memory* m_syncpt_memory{};
    u64 m_syncpt_rw{};
    u64 m_syncpt_ro{};
    u32 m_syncpt_count{};
    std::shared_ptr<Nx2SyncptPage> m_syncpt_page{std::make_shared<Nx2SyncptPage>()};
    std::shared_ptr<Core::Timing::EventType> m_syncpt_poll;
};

} // namespace Service
