// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/kernel/k_event.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/ngc/guest_hid.h"
#include "core/hle/service/service.h"

namespace Service {

// User service returned by stpl:u 0 (nn::streamplay::detail::GetUserService). Command 300 is
// guessed to hand out an event the SDK attaches to; with EDEN_NX2_STREAMPLAY_300=1 it returns the
// guest HID shared memory like the registered stpl:u (ngc.cpp). Everything else falls through to
// the nx2 auto stub (zero sizes, nothing streaming).
class INx2StreamPlayUserService final : public ServiceFramework<INx2StreamPlayUserService> {
public:
    explicit INx2StreamPlayUserService(Core::System& system_)
        : ServiceFramework{system_, "nx2:stpl:u::0"}, service_context{system_, "stpl:u"} {
        static const FunctionInfo functions[] = {
            {300, &INx2StreamPlayUserService::GetEvent, "GetEvent (guessed)"},
        };
        static const FunctionInfo guest_hid_functions[] = {
            {300, &INx2StreamPlayUserService::GetGuestHidSharedMemory, "GetGuestHidSharedMemory"},
        };
        if (NGC::StreamPlayGuestHid()) {
            RegisterHandlers(guest_hid_functions);
        } else {
            RegisterHandlers(functions);
            event = service_context.CreateEvent("INx2StreamPlayUserService:Event");
        }
    }

    ~INx2StreamPlayUserService() override {
        if (event != nullptr) {
            service_context.CloseEvent(event);
        }
        if (guest_hid_memory != nullptr) {
            guest_hid_memory->Close(kernel);
        }
    }

private:
    void GetEvent(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) stpl:u::0 {}, returning an event", ctx.GetCommand());
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, event->GetReadableEvent());
    }

    void GetGuestHidSharedMemory(HLERequestContext& ctx) {
        NGC::ReplyGuestHidSharedMemory(ctx, system, guest_hid_memory);
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* event{};
    Kernel::KSharedMemory* guest_hid_memory{};
};

// Switch 2 stream play (GameShare), stpl:u. Command 0 (u32 0x12) opens the user service the SDK
// keeps for the process; without an object the first nn::streamplay call jumps through null.
class INx2StreamPlayService final : public ServiceFramework<INx2StreamPlayService> {
public:
    explicit INx2StreamPlayService(Core::System& system_)
        : ServiceFramework{system_, "nx2:stpl:u"} {
        static const FunctionInfo functions[] = {
            {0, &INx2StreamPlayService::GetUserService, "GetUserService (guessed)"},
        };
        RegisterHandlers(functions);
    }

private:
    void GetUserService(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) stpl:u 0 [8]={:#x}, returning a user service",
                    ctx.CommandBuffer()[8]);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<INx2StreamPlayUserService>(ctx, system);
    }
};

} // namespace Service
