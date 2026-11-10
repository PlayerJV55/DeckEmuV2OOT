// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <memory>

#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/hle/service/server_manager.h"

namespace Service {

// The server of the Switch 2 graphics system sessions: nx2:gsv (ISelfController 2000) and every
// session handed out from it (vi2, gpu, syncpt, nvsrv, video decoder, ...), on a host thread of
// its own. They used to be served by AM's server, a guest service thread on emulated core 3: vi2's
// present (229) waits there for the previous frame's GPU fence and then for the next vblank, and
// for that time core 3 ran no game thread and no other graphics request was answered. One thread,
// so these handlers stay serialized among themselves as before. Set while the server runs and
// cleared by ~ServerManager (ForgetNx2GsvServer) before it goes away.
inline std::atomic<ServerManager*> g_nx2_gsv_server{};

// EDEN_NX2_GSV_HOST_THREAD=1 opts into the host thread; keep AM's server by default.
inline bool Nx2GsvHostThreadSwitch() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_GSV_HOST_THREAD");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// Whether the graphics system gets its own host thread. Not with single-core CPU emulation: a
// present's fence wait and vblank pacing hold the one core thread there and guest time with it
// (vi composes and speed-limits from a CoreTiming event on it); on a thread of their own the core
// would idle meanwhile and CoreTiming skip ahead to its next event.
inline bool Nx2GsvHostThread(const Core::System& system) {
    return Nx2GsvHostThreadSwitch() && system.IsMulticore();
}

/// Runs the graphics system server on the calling host thread until emulation stops.
inline void RunNx2GsvServer(Core::System& system) {
    // The game's presents wait on this thread. They ran on a guest core thread before (Critical,
    // performance cores): placed like vi's vsync thread.
    Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
    Common::SetCurrentThreadToPerformanceCores();
    auto server_manager = std::make_unique<ServerManager>(system);
    g_nx2_gsv_server.store(server_manager.get(), std::memory_order_release);
    ServerManager::RunServer(std::move(server_manager));
}

/// Called by ~ServerManager: new nx2:gsv sessions no longer go to `server` if it is that server.
/// Only if it still is, so a late call can't clear the server of the next boot.
inline void ForgetNx2GsvServer(ServerManager* server) {
    g_nx2_gsv_server.compare_exchange_strong(server, nullptr, std::memory_order_acq_rel);
}

/// The server for a new nx2:gsv session: the graphics system's own while it runs, else `fallback`
/// (AM's). The sessions it hands out go to the server of the session that asks (CreateRawSession),
/// so each nx2:gsv session and all of its sessions are served by one thread.
inline ServerManager& PickNx2GsvServer(Core::System& system, ServerManager& fallback) {
    ServerManager* const server =
        system.IsPoweredOn() ? g_nx2_gsv_server.load(std::memory_order_acquire) : nullptr;
    if (server != nullptr) {
        LOG_INFO(Service, "nx2:gsv: graphics system sessions served on a host thread of their own");
        return *server;
    }
    LOG_INFO(Service, "nx2:gsv: graphics system sessions served on AM's server ({})",
             !Nx2GsvHostThreadSwitch() ? "host-thread experiment disabled"
             : !system.IsMulticore()   ? "single-core CPU emulation"
                                       : "the host thread's server is not running");
    return fallback;
}

} // namespace Service
