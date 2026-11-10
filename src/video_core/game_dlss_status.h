// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCore {

/// What draws a game's own DLSS evaluation (Fire Emblem: Fortune's Weave's network) this session.
enum class GameDlssState : u8 {
    None,        ///< No DLSS evaluation seen yet.
    Network,     ///< The game's own network is shown.
    Replacement, ///< The replacement upscale is shown; the reason says why.
};

/// Why the replacement upscale draws instead of the game's own network.
enum class GameDlssReason : u8 {
    None,
    NvidiaDlss,      ///< NVIDIA DLSS is turned on and runs in its place.
    TurnedOff,       ///< The "Use the game's own DLSS" setting is off.
    PassesCannotRun, ///< Some of the network's programs can't be translated (rejected_passes).
};

struct GameDlssStatus {
    GameDlssState state{};
    GameDlssReason reason{};
    u32 rejected_passes{};
};

/// For the settings dialog; safe to call from any thread.
[[nodiscard]] GameDlssStatus GetGameDlssStatus();
void SetGameDlssStatus(const GameDlssStatus& status);

/// The reason in English, for the log.
[[nodiscard]] const char* GameDlssReasonText(GameDlssReason reason);

} // namespace VideoCore
