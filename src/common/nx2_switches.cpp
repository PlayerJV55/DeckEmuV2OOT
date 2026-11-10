// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nx2_switches.h"
#include "common/nx2_title_defaults.h"

namespace Common {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view SWITCH_FILE = "nx2_switches.txt";
constexpr std::string_view SWITCH_PREFIX = "EDEN_NX2_";
// Selevet's switches without that prefix, so its nx2_switches.txt works unchanged.
constexpr std::array<std::string_view, 5> OTHER_SWITCHES{
    "EDEN_CPU_ACCURATE_FLOAT",
    "EDEN_CPU_FASTMEM_CHECK",
    "EDEN_CPU_FUSED_FMA",
    "EDEN_CPU_GLOBAL_MONITOR",
    "EDEN_TIMING_COARSE",
};

std::string_view Trim(std::string_view text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

/// The name and value a line assigns; nothing for comments, other lines and other variables.
std::optional<std::pair<std::string_view, std::string_view>> ParseLine(std::string_view line) {
    const std::string_view text = Trim(line);
    if (text.empty() || text.front() == '#') {
        return std::nullopt;
    }
    const size_t eq = text.find('=');
    if (eq == std::string_view::npos) {
        return std::nullopt;
    }
    // Only the emulator's own switches: the file cannot change PATH or a driver variable.
    const std::string_view name = Trim(text.substr(0, eq));
    if (!name.starts_with(SWITCH_PREFIX) &&
        std::ranges::find(OTHER_SWITCHES, name) == OTHER_SWITCHES.end()) {
        return std::nullopt;
    }
    return std::pair{name, Trim(text.substr(eq + 1))};
}

/// The switches LoadNx2Switches set from a file.
std::vector<std::string>& SwitchesSetFromFiles() {
    static std::vector<std::string> names;
    return names;
}

/// A variable the applied game profile set, and what it held before.
struct ProfileChange {
    std::string name;
    std::optional<std::string> previous;
};

std::vector<ProfileChange>& ProfileChanges() {
    static std::vector<ProfileChange> changes;
    return changes;
}

void SetVariable(const std::string& name, const std::optional<std::string>& value) {
#ifdef _WIN32
    // An empty value removes it
    _putenv_s(name.c_str(), value ? value->c_str() : "");
#else
    if (value) {
        setenv(name.c_str(), value->c_str(), 1);
    } else {
        unsetenv(name.c_str());
    }
#endif
}

} // Anonymous namespace

std::vector<fs::path> GetNx2SwitchFiles() {
    std::vector<fs::path> files;
#if defined(_WIN32)
    // The copy next to the executable comes first because a session-launched process can see a
    // virtualized %APPDATA%; the data folder copies (nemyu, then the portable user folder)
    // override it. Eden's folder is not read: other Eden forks keep their own switches there.
    const fs::path exe_dir = FS::GetExeDirectory();
    const fs::path appdata = FS::GetAppDataRoamingDirectory();
    if (!exe_dir.empty()) {
        files.push_back(exe_dir / SWITCH_FILE);
    }
    if (!appdata.empty()) {
        files.push_back(appdata / EDEN_DIR / SWITCH_FILE);
    }
    if (!exe_dir.empty()) {
        files.push_back(exe_dir / PORTABLE_DIR / SWITCH_FILE);
    }
#elif !defined(__ANDROID__)
    const fs::path config = FS::GetDataDirectory("XDG_CONFIG_HOME");
    if (!config.empty()) {
        files.push_back(config / EDEN_DIR / SWITCH_FILE);
    }
    std::error_code ec;
    const fs::path current = fs::current_path(ec);
    if (!ec) {
        files.push_back(current / PORTABLE_DIR / SWITCH_FILE);
    }
#endif
    return files;
}

fs::path GetUserNx2SwitchFile() {
#if defined(_WIN32)
    // A portable install is one with a user folder next to the executable (PathManagerImpl).
    std::error_code ec;
    const fs::path exe_dir = FS::GetExeDirectory();
    if (!exe_dir.empty() && fs::is_directory(exe_dir / PORTABLE_DIR, ec)) {
        return exe_dir / PORTABLE_DIR / SWITCH_FILE;
    }
    const fs::path appdata = FS::GetAppDataRoamingDirectory();
    if (!appdata.empty()) {
        return appdata / EDEN_DIR / SWITCH_FILE;
    }
#elif !defined(__ANDROID__)
    std::error_code ec;
    const fs::path current = fs::current_path(ec);
    if (!ec && fs::is_directory(current / PORTABLE_DIR, ec)) {
        return current / PORTABLE_DIR / SWITCH_FILE;
    }
    const fs::path config = FS::GetDataDirectory("XDG_CONFIG_HOME");
    if (!config.empty()) {
        return config / EDEN_DIR / SWITCH_FILE;
    }
#endif
    return {};
}

std::vector<Nx2Switch> ReadNx2SwitchFile(const fs::path& file) {
    std::vector<Nx2Switch> switches;
    if (file.empty()) {
        return switches;
    }
    std::ifstream stream{file};
    if (!stream) {
        return switches;
    }
    std::string line;
    while (std::getline(stream, line)) {
        if (const auto assignment = ParseLine(line)) {
            switches.push_back({std::string{assignment->first}, std::string{assignment->second}});
        }
    }
    return switches;
}

bool WriteNx2SwitchFile(const fs::path& file, std::span<const Nx2Switch> switches) {
    if (file.empty()) {
        return false;
    }
    std::vector<std::string> lines;
    if (std::ifstream stream{file}; stream) {
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
    }

    std::string text;
    if (lines.empty()) {
        text = "# Nemyu NX2 switches: one NAME=VALUE per line, '#' starts a comment. Read when\n"
               "# Nemyu starts; Configure > Switch 2 > Advanced Switches edits this file.\n";
    }
    const auto append = [&text](const Nx2Switch& entry) {
        text += entry.name;
        text += '=';
        text += entry.value;
        text += '\n';
    };
    std::vector<bool> written(switches.size());
    for (const std::string& line : lines) {
        const auto assignment = ParseLine(line);
        if (!assignment) {
            text += line;
            text += '\n';
            continue;
        }
        // The first assignment of a switch takes its new value; repeats and removed ones go.
        for (size_t i = 0; i < switches.size(); ++i) {
            if (!written[i] && switches[i].name == assignment->first) {
                append(switches[i]);
                written[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < switches.size(); ++i) {
        if (!written[i]) {
            append(switches[i]);
        }
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

void LoadNx2Switches() {
#ifndef __ANDROID__
    std::vector<Nx2Switch> entries;
    for (const fs::path& file : GetNx2SwitchFiles()) {
        std::vector<Nx2Switch> found = ReadNx2SwitchFile(file);
        entries.insert(entries.end(), std::make_move_iterator(found.begin()),
                       std::make_move_iterator(found.end()));
    }
    // Decide against the real environment once, so a later file can still replace an earlier one.
    std::vector<bool> from_environment(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        from_environment[i] = std::getenv(entries[i].name.c_str()) != nullptr;
    }
    std::vector<std::string>& set_from_files = SwitchesSetFromFiles();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (from_environment[i]) {
            continue;
        }
        const auto& [name, value] = entries[i];
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
        set_from_files.push_back(name);
    }
#endif
}

bool IsSetInRealEnvironment(const std::string& name) {
    if (std::getenv(name.c_str()) == nullptr) {
        return false;
    }
    const std::vector<ProfileChange>& changes = ProfileChanges();
    if (std::ranges::find(changes, name, &ProfileChange::name) != changes.end()) {
        return false;
    }
    const std::vector<std::string>& set_from_files = SwitchesSetFromFiles();
    return std::find(set_from_files.begin(), set_from_files.end(), name) == set_from_files.end();
}

std::vector<Nx2Switch> EffectiveNx2ProfileSwitches(std::span<const Nx2Switch> profile) {
    std::vector<Nx2Switch> switches;
    for (const Nx2Switch& entry : profile) {
        if (IsSetInRealEnvironment(entry.name)) {
            continue;
        }
        if (const auto existing = std::ranges::find(switches, entry.name, &Nx2Switch::name);
            existing != switches.end()) {
            existing->value = entry.value;
        } else {
            switches.push_back(entry);
        }
    }
    return switches;
}

void ApplyNx2Profile(std::string_view name, std::span<const Nx2Switch> switches) {
    RestoreNx2Profile();
    LOG_INFO(Common, "Switch profile \"{}\": {} switches", name, switches.size());
    std::vector<ProfileChange>& changes = ProfileChanges();
    for (const auto& [switch_name, value] : switches) {
        const char* const previous = std::getenv(switch_name.c_str());
        LOG_INFO(Common, "Switch profile \"{}\": {}={} (global: {})", name, switch_name, value,
                 previous != nullptr ? previous : "not set");
        changes.push_back({switch_name, previous != nullptr ? std::optional<std::string>{previous}
                                                            : std::nullopt});
        SetVariable(switch_name, value);
    }
    // Values worked out for the running program read the new switches
    Nx2::g_program_generation.fetch_add(1, std::memory_order_release);
}

void RestoreNx2Profile() {
    std::vector<ProfileChange>& changes = ProfileChanges();
    if (changes.empty()) {
        return;
    }
    // Backwards, so a switch set twice gets back the value it had before the first time
    for (auto change = changes.rbegin(); change != changes.rend(); ++change) {
        SetVariable(change->name, change->previous);
    }
    changes.clear();
    LOG_INFO(Common, "Switch profile undone: the global switches apply again");
    Nx2::g_program_generation.fetch_add(1, std::memory_order_release);
}

void UnloadNx2Switches() {
    RestoreNx2Profile();
    std::vector<std::string>& set_from_files = SwitchesSetFromFiles();
    for (const std::string& name : set_from_files) {
        SetVariable(name, std::nullopt);
    }
    set_from_files.clear();
}

} // namespace Common
