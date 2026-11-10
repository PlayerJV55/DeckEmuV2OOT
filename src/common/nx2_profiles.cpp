// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <system_error>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/nx2_profiles.h"
#include "common/nx2_title_defaults.h"
#include "common/string_util.h"

namespace Common {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view PROFILE_DIR = "nx2_profiles";
constexpr std::string_view PROFILE_EXTENSION = ".txt";
constexpr std::string_view GAMES_FILE = "games.ini";
constexpr size_t MAX_NAME_LENGTH = 64;
/// FileSys::GetBaseTitleID's mask
constexpr u64 BASE_TITLE_ID_MASK = 0xFFFFFFFFFFFFE000;

/// Device names Windows reserves, with any extension.
constexpr std::array<std::string_view, 24> RESERVED_NAMES{
    "CON",  "PRN",  "AUX",  "NUL",  "COM0", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT0", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
};

fs::path GamesFile() {
    const fs::path directory = GetNx2ProfileDirectory();
    return directory.empty() ? fs::path{} : directory / GAMES_FILE;
}

std::map<u64, std::string> ReadGameProfiles() {
    std::map<u64, std::string> games;
    const fs::path file = GamesFile();
    if (file.empty()) {
        return games;
    }
    std::ifstream stream{file};
    std::string line;
    while (std::getline(stream, line)) {
        const std::string text = StripSpaces(line);
        const size_t eq = text.find('=');
        if (text.empty() || text.front() == '#' || eq == std::string::npos) {
            continue;
        }
        const std::string id = StripSpaces(std::string_view{text}.substr(0, eq));
        u64 program_id{};
        const auto [end, error] =
            std::from_chars(id.data(), id.data() + id.size(), program_id, 16);
        std::string name = StripSpaces(std::string_view{text}.substr(eq + 1));
        if (error == std::errc{} && end == id.data() + id.size() &&
            IsValidNx2ProfileName(name)) {
            games[program_id] = std::move(name);
        }
    }
    return games;
}

bool WriteGameProfiles(const std::map<u64, std::string>& games) {
    const fs::path file = GamesFile();
    if (file.empty()) {
        return false;
    }
    std::string text = "# The switch profile of each game (title ID=profile), set in the game's "
                       "Properties.\n";
    for (const auto& [program_id, name] : games) {
        text += fmt::format("{:016X}={}\n", program_id, name);
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream stream{temporary, std::ios::trunc};
        stream << text;
        stream.close();
        if (!stream) {
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, file, ec);
    if (ec) {
        std::error_code remove_ec;
        fs::remove(temporary, remove_ec);
        return false;
    }
    return true;
}

} // Anonymous namespace

bool IsValidNx2ProfileName(std::string_view name) {
    // Windows drops a trailing dot or space from file names, and a leading space is easy to miss
    if (name.empty() || name.size() > MAX_NAME_LENGTH || name.front() == ' ' ||
        name.back() == ' ' || name.back() == '.') {
        return false;
    }
    constexpr std::string_view forbidden = "<>:\"/\\|?*";
    const bool bad_character = std::ranges::any_of(name, [&](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7F || forbidden.find(c) != std::string_view::npos;
    });
    if (bad_character) {
        return false;
    }
    std::string base{name.substr(0, name.find('.'))};
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    std::ranges::transform(base, base.begin(), [](char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    });
    return std::ranges::find(RESERVED_NAMES, std::string_view{base}) == RESERVED_NAMES.end();
}

fs::path GetNx2ProfileDirectory() {
    const fs::path switch_file = GetUserNx2SwitchFile();
    return switch_file.empty() ? fs::path{} : switch_file.parent_path() / PROFILE_DIR;
}

fs::path GetNx2ProfileFile(std::string_view name) {
    const fs::path directory = GetNx2ProfileDirectory();
    if (directory.empty() || !IsValidNx2ProfileName(name)) {
        return {};
    }
    fs::path file = directory / FS::ToU8String(name);
    file += PROFILE_EXTENSION;
    return file;
}

std::vector<std::string> ListNx2Profiles() {
    std::vector<std::string> names;
    const fs::path directory = GetNx2ProfileDirectory();
    if (directory.empty()) {
        return names;
    }
    std::error_code ec;
    for (fs::directory_iterator entry{directory, ec}; !ec && entry != fs::directory_iterator{};
         entry.increment(ec)) {
        const fs::path& path = entry->path();
        std::error_code type_ec;
        if (path.extension() != fs::path{PROFILE_EXTENSION} || !entry->is_regular_file(type_ec)) {
            continue;
        }
        std::string name = FS::PathToUTF8String(path.stem());
        if (IsValidNx2ProfileName(name)) {
            names.push_back(std::move(name));
        }
    }
    std::ranges::sort(names);
    return names;
}

bool CreateNx2Profile(std::string_view name, std::span<const Nx2Switch> switches) {
    const fs::path file = GetNx2ProfileFile(name);
    std::error_code ec;
    if (file.empty() || fs::exists(file, ec)) {
        return false;
    }
    fs::create_directories(file.parent_path(), ec);
    {
        std::ofstream stream{file};
        stream << "# Nemyu switch profile: one NAME=VALUE per line, '#' starts a comment. A game "
                  "that uses it\n"
                  "# (its Properties) gets these switches over the global ones while it runs.\n";
        stream.close();
        if (!stream) {
            fs::remove(file, ec);
            return false;
        }
    }
    return WriteNx2SwitchFile(file, switches);
}

bool RenameNx2Profile(std::string_view from, std::string_view to) {
    const fs::path from_file = GetNx2ProfileFile(from);
    const fs::path to_file = GetNx2ProfileFile(to);
    std::error_code ec;
    if (from_file.empty() || to_file.empty() || !fs::exists(from_file, ec)) {
        return false;
    }
    // A change of case only is the same file on Windows
    if (fs::exists(to_file, ec) && !fs::equivalent(from_file, to_file, ec)) {
        return false;
    }
    fs::rename(from_file, to_file, ec);
    if (ec) {
        return false;
    }
    std::map<u64, std::string> games = ReadGameProfiles();
    bool changed = false;
    for (auto& [program_id, name] : games) {
        if (name == from) {
            name = to;
            changed = true;
        }
    }
    return !changed || WriteGameProfiles(games);
}

bool DeleteNx2Profile(std::string_view name) {
    const fs::path file = GetNx2ProfileFile(name);
    std::error_code ec;
    if (file.empty() || !fs::remove(file, ec)) {
        return false;
    }
    std::map<u64, std::string> games = ReadGameProfiles();
    if (std::erase_if(games, [&](const auto& game) { return game.second == name; }) == 0) {
        return true;
    }
    return WriteGameProfiles(games);
}

std::string GetGameNx2Profile(u64 program_id) {
    const std::map<u64, std::string> games = ReadGameProfiles();
    const auto game = games.find(program_id);
    return game != games.end() ? game->second : std::string{};
}

bool SetGameNx2Profile(u64 program_id, std::string_view name) {
    std::map<u64, std::string> games = ReadGameProfiles();
    if (name.empty()) {
        games.erase(program_id);
    } else if (IsValidNx2ProfileName(name)) {
        games[program_id] = std::string{name};
    } else {
        return false;
    }
    return WriteGameProfiles(games);
}

Nx2GameProfile LoadGameNx2Profile(u64 program_id) {
    std::string name = GetGameNx2Profile(program_id);
    if (name.empty()) {
        // Another program of a multi-program game: its base game's profile
        name = GetGameNx2Profile(program_id & BASE_TITLE_ID_MASK);
    }
    if (name.empty()) {
        return {};
    }
    const fs::path file = GetNx2ProfileFile(name);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        LOG_WARNING(Common, "Switch profile \"{}\" of {:016X} is missing: {}", name, program_id,
                    FS::PathToUTF8String(file));
        return {};
    }
    return {std::move(name), EffectiveNx2ProfileSwitches(ReadNx2SwitchFile(file))};
}

std::vector<std::string> Nx2ProcessSwitchChanges(std::span<const Nx2Switch> before,
                                                 std::span<const Nx2Switch> after) {
    const auto value = [](std::span<const Nx2Switch> switches,
                          const std::string& name) -> std::optional<std::string> {
        if (const auto entry = std::ranges::find(switches, name, &Nx2Switch::name);
            entry != switches.end()) {
            return entry->value;
        }
        const char* const current = std::getenv(name.c_str());
        return current != nullptr ? std::optional<std::string>{current} : std::nullopt;
    };
    std::vector<std::string> changed;
    const auto compare = [&](std::span<const Nx2Switch> switches) {
        for (const Nx2Switch& entry : switches) {
            const bool per_session = std::ranges::find(Nx2::SessionSwitches,
                                                       std::string_view{entry.name}) !=
                                     Nx2::SessionSwitches.end();
            if (per_session || std::ranges::find(changed, entry.name) != changed.end()) {
                continue;
            }
            if (value(before, entry.name) != value(after, entry.name)) {
                changed.push_back(entry.name);
            }
        }
    };
    compare(before);
    compare(after);
    return changed;
}

} // namespace Common
