// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "core/core.h"
#include "core/hle/kernel/k_shared_memory.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/service/ipc_helpers.h"

namespace Service::NGC {

// EDEN_NX2_STREAMPLAY_300=1 answers stream play (GameShare) user service command 300 with the
// guest HID shared memory below. Off by default: Bananza calls 300 and runs on PokeDinner with
// the auto stub's reply.
inline bool StreamPlayGuestHid() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_STREAMPLAY_300");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

struct GuestHidMemoryConfig {
    u64 size;
    u64 offset;
};
static_assert(sizeof(GuestHidMemoryConfig) == 0x10);

// Command 300: {u64 size, u64 offset} and a read-only shared memory the SDK reads guest
// controllers from. 18 mouse slots of 0x1400 bytes after 0x84000 (Star Fox's SDK layout) end at
// 0x9a800, rounded up to 0x9b000. No guest ever connects, so it stays zero.
inline void ReplyGuestHidSharedMemory(HLERequestContext& ctx, Core::System& system,
                                      Kernel::KSharedMemory*& guest_hid_memory) {
    constexpr u64 GuestHidMemorySize = 0x9b000;
    auto& kernel = system.Kernel();
    if (guest_hid_memory == nullptr) {
        auto* memory = Kernel::KSharedMemory::Create(kernel);
        const Result result =
            memory != nullptr
                ? memory->Initialize(kernel, system.DeviceMemory(), nullptr,
                                     Kernel::Svc::MemoryPermission::ReadWrite,
                                     Kernel::Svc::MemoryPermission::Read, GuestHidMemorySize)
                : Kernel::ResultOutOfResource;
        if (result.IsError()) {
            if (memory != nullptr) {
                memory->Close(kernel);
            }
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(result);
            return;
        }
        Kernel::KSharedMemory::Register(kernel, memory);
        guest_hid_memory = memory;
        LOG_INFO(Service_NGC, "GameShare guest HID shared memory: size={:#x}, no guests",
                 GuestHidMemorySize);
    }
    IPC::ResponseBuilder rb{ctx, 6, 1};
    rb.Push(ResultSuccess);
    rb.PushRaw(GuestHidMemoryConfig{GuestHidMemorySize, 0});
    rb.PushCopyObjects(ctx, *guest_hid_memory);
}

} // namespace Service::NGC
