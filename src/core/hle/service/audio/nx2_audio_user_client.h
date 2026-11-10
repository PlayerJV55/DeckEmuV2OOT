// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/service/audio/audio_in_manager.h"
#include "core/hle/service/audio/audio_out_manager.h"
#include "core/hle/service/audio/audio_renderer_manager.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"

namespace Service::Audio {

// Switch 2 "aud:u", nn::audio::IUserClientSession (nn::audio::CreateUserClientSessionByHipc).
// The SDK reaches the Switch 1 audio interfaces through it: command 2 returns an object used like
// IAudioRendererManager (vtable slot 1 = GetWorkBufferSize with a Switch 1 parameter block), so
// it is backed by Eden's audren:u implementation. Command 1 is nn::audio::OpenAudioOut's
// manager, called like IAudioOutManager (slot 1 = OpenAudioOut), backed by audout:u, and command
// 0 the audio input one, backed by audin:u. The nx2: name keeps unknown commands auto-stubbed and
// logged.
class IUserClientSession final : public ServiceFramework<IUserClientSession> {
public:
    // The managers are the audin:u, audout:u and audren:u instances: their sessions point into the
    // manager that opened them, so it has to outlive them like those named services do.
    explicit IUserClientSession(Core::System& system_,
                                std::shared_ptr<IAudioInManager> audio_in_manager_,
                                std::shared_ptr<IAudioOutManager> audio_out_manager_,
                                std::shared_ptr<IAudioRendererManager> audio_renderer_manager_)
        : ServiceFramework{system_, "nx2:aud:u"}, audio_in_manager{std::move(audio_in_manager_)},
          audio_out_manager{std::move(audio_out_manager_)},
          audio_renderer_manager{std::move(audio_renderer_manager_)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IUserClientSession::GetAudioInManager, "GetAudioInManager"},
            {1, &IUserClientSession::GetAudioOutManager, "GetAudioOutManager (guessed)"},
            {2, &IUserClientSession::GetAudioRendererManager, "GetAudioRendererManager (guessed)"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void GetAudioInManager(HLERequestContext& ctx) {
        LOG_INFO(Service_Audio, "Switch 2 aud:u 0, returning audin:u");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface(ctx, audio_in_manager);
    }

    void GetAudioOutManager(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 arg = rp.Pop<u32>();
        LOG_INFO(Service_Audio, "Switch 2 aud:u 1 arg={}, returning audout:u", arg);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface(ctx, audio_out_manager);
    }

    void GetAudioRendererManager(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 arg = rp.Pop<u32>();
        LOG_INFO(Service_Audio, "Switch 2 aud:u 2 arg={}, returning audren:u", arg);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface(ctx, audio_renderer_manager);
    }

    std::shared_ptr<IAudioInManager> audio_in_manager;
    std::shared_ptr<IAudioOutManager> audio_out_manager;
    std::shared_ptr<IAudioRendererManager> audio_renderer_manager;
};

} // namespace Service::Audio
