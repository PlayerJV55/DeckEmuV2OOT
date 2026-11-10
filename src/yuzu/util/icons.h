// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QIcon>

namespace Icons {

/// Sections of the side list of the settings dialogs
enum class SettingsSection {
    General,
    System,
    Emulation,
    Graphics,
    Switch2,
    Audio,
    Controls,
    Hotkeys,
    Network,
    Debug,
    AddOns,
};

/// Monochrome line icon from dist/icons/line, drawn in the text colour of the current theme.
/// Disabled icons are dimmed like disabled text. Pass the file name without ".svg".
QIcon Get(const char* name);

/// Icon shown next to a section of the settings dialogs
QIcon ForSettingsSection(SettingsSection section);

/// Takes the text colour again from the current theme, call after the theme changes
void UpdateColors();

} // namespace Icons
