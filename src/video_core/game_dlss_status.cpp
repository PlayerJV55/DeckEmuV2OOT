// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <mutex>

#include "video_core/game_dlss_status.h"

namespace VideoCore {
namespace {
std::mutex status_mutex;
GameDlssStatus status;
} // Anonymous namespace

GameDlssStatus GetGameDlssStatus() {
    std::scoped_lock lock{status_mutex};
    return status;
}

void SetGameDlssStatus(const GameDlssStatus& new_status) {
    std::scoped_lock lock{status_mutex};
    status = new_status;
}

const char* GameDlssReasonText(GameDlssReason reason) {
    switch (reason) {
    case GameDlssReason::None:
        return "";
    case GameDlssReason::NvidiaDlss:
        return "NVIDIA DLSS is turned on and runs in its place";
    case GameDlssReason::TurnedOff:
        return "it is turned off";
    case GameDlssReason::PassesCannotRun:
        return "some of its passes can't run here";
    }
    return "";
}

} // namespace VideoCore
