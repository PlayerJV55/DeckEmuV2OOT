// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2018 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "discord.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace DiscordRPC {

class DiscordImpl : public DiscordInterface {
public:
    DiscordImpl(Core::System& system_);
    ~DiscordImpl() override;

    void Pause() override;
    void Update() override;

private:
    enum class State { Cleared, Idle, InGame };

    struct Request {
        State state{};
        std::string title;
        std::string cover_slug;              ///< Box art name
        std::vector<std::string> cover_urls; ///< Box art to look for, in order
        s64 start_time{};
    };

    void Post(Request request);
    void Run(std::stop_token stop_token);
    std::string FindCover(const Request& request, std::stop_token stop_token);

    Core::System& system;

    // Box art lookups block on the network, so presence updates are sent from a worker thread.
    // Only the latest request matters, so a newer one replaces any still pending.
    std::mutex mutex;
    std::condition_variable_any request_cv;
    std::optional<Request> pending;
    std::unordered_map<std::string, std::string> covers; ///< Worker only: first URL -> found image
    std::jthread worker;
};

} // namespace DiscordRPC
