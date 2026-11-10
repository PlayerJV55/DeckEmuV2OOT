// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <vector>

#include <QString>

namespace OtherEmulators {

struct Folder {
    QString emulator;
    std::filesystem::path path;
};

/// The folders of other emulators that match `ours`, a folder inside this emulator's data
/// folder, when they exist.
std::vector<Folder> Equivalents(const std::filesystem::path& ours);

} // namespace OtherEmulators
