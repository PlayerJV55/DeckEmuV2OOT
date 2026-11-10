// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string_view>
#include <QString>
#include "common/common_types.h"

/// The environment switches the emulator reads (EDEN_NX2_* and a few other EDEN_* ones), described
/// for the Advanced Switches page. Add an entry next to every new getenv.
namespace Nx2SwitchCatalog {

enum class Category : u8 {
    GpuCommands,
    BufferCache,
    TextureCache,
    Shaders,
    CpuTiming,
    VideoAudio,
    Services,
    Diagnostics,
};
inline constexpr size_t NUM_CATEGORIES = 8;

enum class ValueType : u8 {
    Toggle,   ///< "1" turns it on, "0" off
    Presence, ///< On whenever it is set, whatever the value
    Choice,   ///< One of the entry's choices
    Number,   ///< A whole number from minimum to maximum
    Text,     ///< Free text: a path, an address or a list
};

enum Flags : u8 {
    Diagnostic = 1 << 0,      ///< Logs, dumps or test aids: slower, or fills the log
    EnvironmentOnly = 1 << 1, ///< Read before the switch files are, or not an EDEN_NX2_ name
    CliOnly = 1 << 2,         ///< Read by nemyu-cli only
    GuiOnly = 1 << 3,         ///< Read by the Qt frontend only
    NotInThisBuild = 1 << 4,  ///< Read by changes not every build includes yet
};

/// Texts are QT_TRANSLATE_NOOP("Nx2SwitchCatalog", ...) sources; show them with Translate.
struct Choice {
    const char* value;
    const char* label;
};

struct Entry {
    const char* name;
    Category category;
    ValueType type;
    u8 flags;
    std::span<const Choice> choices;
    int minimum;
    int maximum;
    const char* suggested; ///< The value a new override starts from
    const char* default_value;
    const char* description;
    const char* gains;
    const char* downsides;
    const char* games;
};

[[nodiscard]] std::span<const Entry> Entries();

/// nullptr for a name the catalog doesn't know.
[[nodiscard]] const Entry* Find(std::string_view name);

/// An empty string for nullptr or an empty text.
[[nodiscard]] QString Translate(const char* text);

[[nodiscard]] QString CategoryName(Category category);

} // namespace Nx2SwitchCatalog
