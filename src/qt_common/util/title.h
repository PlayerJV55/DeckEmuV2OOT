// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <QString>

namespace Loader {
class AppLoader;
}

namespace QtCommon::Title {

/**
 * Names a loose ExeFS dump (<game>/exefs/main, <game>/base/exefs/main, updates in
 * <game>/updates/<title id>/exefs/main) after its game folder, as it has no control data.
 * Empty for any other file.
 */
[[nodiscard]] QString LooseExeFSName(const std::string& physical_name);

/**
 * Display name of a game: from its control data or the title lists (AppLoader::ReadTitle), else
 * its loose dump folder. Empty if neither is known.
 */
[[nodiscard]] std::string GetName(Loader::AppLoader& loader, const std::string& physical_name);

/// GetName for the file the loader was opened from, such as the running game's.
[[nodiscard]] std::string GetName(Loader::AppLoader& loader);

/// Display version ("1.8.0") of the game the loader opened: its installed update's, else its own.
[[nodiscard]] std::string GetDisplayVersion(Loader::AppLoader& loader);

} // namespace QtCommon::Title
