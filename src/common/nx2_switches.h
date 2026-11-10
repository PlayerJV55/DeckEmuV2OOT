// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Common {

struct Nx2Switch {
    std::string name;
    std::string value;
};

/// Sets the EDEN_NX2_* switches written in nx2_switches.txt files ("NAME=VALUE" per line, '#'
/// comments), so a launch from the GUI can use them too. Call it at the top of main(), before any
/// switch is read. A variable already in the real environment wins; does nothing without a file.
void LoadNx2Switches();

/// The files LoadNx2Switches reads, in its order (a later one wins). They need not exist.
[[nodiscard]] std::vector<std::filesystem::path> GetNx2SwitchFiles();

/// The file for this install's own switches: the portable user folder's when there is one, else
/// the one in the nemyu data folder. Never the copy next to the executable. Empty on Android.
[[nodiscard]] std::filesystem::path GetUserNx2SwitchFile();

/// The EDEN_NX2_* assignments of a switch file in file order, read as LoadNx2Switches reads them.
/// Empty if the file can't be read.
[[nodiscard]] std::vector<Nx2Switch> ReadNx2SwitchFile(const std::filesystem::path& file);

/// Writes `switches` into a switch file. Its comments and other lines stay where they are, an
/// assignment of a listed switch gets the new value, any other assignment is dropped, and the rest
/// is appended. Creates the file and its folder if needed; returns false if it can't be written.
bool WriteNx2SwitchFile(const std::filesystem::path& file, std::span<const Nx2Switch> switches);

/// Whether the variable came from the process environment rather than from a switch file or a game
/// profile. Such a variable wins over every file.
[[nodiscard]] bool IsSetInRealEnvironment(const std::string& name);

/// The switches a game profile puts into the environment: the last assignment of each switch in
/// `profile`, except those set in the real environment.
[[nodiscard]] std::vector<Nx2Switch> EffectiveNx2ProfileSwitches(
    std::span<const Nx2Switch> profile);

/// Puts a game profile's switches, as EffectiveNx2ProfileSwitches gives them, into the environment
/// over the switch files' values and logs them. Undoes an earlier profile first, and remembers what
/// each variable held for RestoreNx2Profile. `name` is for the log.
void ApplyNx2Profile(std::string_view name, std::span<const Nx2Switch> switches);

/// Puts every variable the applied profile changed back as it was, value or absence, so the
/// environment holds the switch files' values again. Does nothing without an applied profile.
void RestoreNx2Profile();

/// Removes the variables LoadNx2Switches and the applied profile set, leaving the real environment,
/// so a Nemyu process started from this one reads the switch files itself.
void UnloadNx2Switches();

} // namespace Common
