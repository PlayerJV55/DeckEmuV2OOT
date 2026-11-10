// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>

class QWidget;

namespace PortableMode {

/// The folder whose presence makes Nemyu portable at startup: "user" next to the executable on
/// Windows, in the working directory elsewhere (Common::FS Reinitialize).
std::filesystem::path PortableDir();

/// Whether this session keeps its data in PortableDir().
bool IsActive();

/// What to do once Nemyu has exited: move `from` to `to` when `from` is set, then start it again,
/// booting `game` when it is set.
struct Restart {
    std::filesystem::path from;
    std::filesystem::path to;
    std::filesystem::path game;
};

/// Asks to turn portable mode on or off and prepares the data folder. Returns the restart to run
/// after closing, or nothing when the user cancelled or the preparation failed.
std::optional<Restart> Configure(QWidget* parent);

/// Starts a detached helper that waits for this process to exit, does the move and starts Nemyu.
bool LaunchRestart(const Restart& restart);

} // namespace PortableMode
