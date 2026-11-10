// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "common/nx2_switches.h"

// Switch profiles: named sets of switches in <data>/nx2_profiles/<name>.txt, written like
// nx2_switches.txt. A game's Properties can pick one; while that game runs, the profile's switches
// go over the global ones (ApplyNx2Profile). nx2_profiles/games.ini maps title IDs to profiles.
namespace Common {

/// Whether `name` can name a profile: a file name that works on every platform.
[[nodiscard]] bool IsValidNx2ProfileName(std::string_view name);

/// The profile folder, next to this install's switch file. Empty on Android.
[[nodiscard]] std::filesystem::path GetNx2ProfileDirectory();

/// A profile's file, whether or not it exists. Empty if the name is not valid.
[[nodiscard]] std::filesystem::path GetNx2ProfileFile(std::string_view name);

/// The profiles in the profile folder, sorted by name.
[[nodiscard]] std::vector<std::string> ListNx2Profiles();

/// Creates a profile holding `switches`. False if the name is not valid or taken, or the file
/// can't be written.
bool CreateNx2Profile(std::string_view name, std::span<const Nx2Switch> switches);

/// Renames a profile and the games' assignments to it. False if `to` is not valid or taken.
bool RenameNx2Profile(std::string_view from, std::string_view to);

/// Deletes a profile; the games that used it go back to the global switches.
bool DeleteNx2Profile(std::string_view name);

/// The profile assigned to a game, empty for none.
[[nodiscard]] std::string GetGameNx2Profile(u64 program_id);

/// Assigns a profile to a game; an empty name takes it away.
bool SetGameNx2Profile(u64 program_id, std::string_view name);

struct Nx2GameProfile {
    std::string name; ///< Empty when the game has no profile
    std::vector<Nx2Switch> switches; ///< As EffectiveNx2ProfileSwitches gives them
};

/// The profile assigned to a game, else to its base title ID, read for ApplyNx2Profile. A profile
/// whose file is gone counts as none.
[[nodiscard]] Nx2GameProfile LoadGameNx2Profile(u64 program_id);

/// The switches read once per process (not in Nx2::SessionSwitches) that differ between two
/// profiles' switches, as EffectiveNx2ProfileSwitches gives them. A switch only one of them sets is
/// compared with the environment, so call it while no profile is applied.
[[nodiscard]] std::vector<std::string> Nx2ProcessSwitchChanges(std::span<const Nx2Switch> before,
                                                               std::span<const Nx2Switch> after);

} // namespace Common
