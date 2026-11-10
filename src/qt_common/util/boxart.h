// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <QImage>
#include "common/common_types.h"

class QObject;

// Box art is downloaded once into <data dir>/boxart/<name>.png, so screens that cannot wait on
// the network, such as the loading screen, can show it.
namespace QtCommon::Boxart {

/// Where the box art named slug is kept, or an empty path if slug is not a valid box art name.
[[nodiscard]] std::filesystem::path GetCachePath(std::string_view slug);

/// Where to look for the box art named slug, in order: the user's box art URL, then Eden's.
[[nodiscard]] std::vector<std::string> GetCoverUrls(std::string_view slug);

/// The game's downloaded box art, or a null image if there is none yet.
[[nodiscard]] QImage LoadCachedCover(u64 program_id, const std::string& title);

/**
 * Downloads the game's box art in the background if it has a known one that is not on disk yet:
 * Switch 2 titles in the title lists, and Switch 1 titles by name. An urgent request is downloaded
 * before the queued ones.
 */
void Prefetch(u64 program_id, const std::string& title, bool urgent = false);

/// The name of the game's box art, or an empty string if it has none.
[[nodiscard]] std::string GetSlug(u64 program_id, const std::string& title);

/// Downloads the box art named slug from url in the background, unless it is already on disk.
void Save(std::string slug, std::string url);

/**
 * Calls on_saved on context's thread with the name of each box art once it has been downloaded.
 * Each context has one listener; an empty on_saved removes it, after which it is not called.
 */
void OnSaved(QObject* context, std::function<void(const std::string& slug)> on_saved);

/// Cancels the downloads and waits for them to end. Later requests are ignored.
void Shutdown();

} // namespace QtCommon::Boxart
