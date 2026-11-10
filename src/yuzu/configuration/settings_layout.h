// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <vector>
#include <QString>

class QBoxLayout;
class QCheckBox;
class QGroupBox;
class QPushButton;
class QWidget;

namespace Settings {
class BasicSetting;
}

namespace ConfigurationShared {

class Builder;
class Widget;

/// A titled box of settings on a configuration tab. The tables below decide which tab shows a
/// setting and where; its Settings category only picks the config file section it is saved in.
struct SettingGroup {
    QString title;
    std::vector<Settings::BasicSetting*> settings; ///< In display order
    /// Built so its rows keep their values on apply, but never shown
    bool hidden = false;
    /// Shown with "Show advanced settings", or while one of its settings is changed
    bool advanced = false;
};

namespace Layout {
// One table per tab, in display order
std::vector<SettingGroup> General();
std::vector<SettingGroup> System();
std::vector<SettingGroup> Applets();
std::vector<SettingGroup> Emulation();
std::vector<SettingGroup> Graphics();
std::vector<SettingGroup> FrameGeneration();
std::vector<SettingGroup> GraphicsAdvanced();
std::vector<SettingGroup> GraphicsFixes();
std::vector<SettingGroup> Switch2();
std::vector<SettingGroup> Audio();
} // namespace Layout

/// Builds the widget of `setting` with `builder`, or returns nullptr when the setting has no row
/// here (hidden, Android only, or global only in a per-game dialog).
[[nodiscard]] Widget* BuildSettingWidget(const Builder& builder, Settings::BasicSetting* setting,
                                         std::vector<std::function<void(bool)>>& apply_funcs);

/// The editable part of a setting row, without its label or restore button.
[[nodiscard]] QWidget* SettingInput(const Widget& widget);

/// Keeps `inputs` enabled only while `parent` is checked. Pass the inputs, not the rows: disabling
/// a whole row would also disable its restore button, which per-game apply reads as "use global".
/// Rows locked while a game runs stay locked.
void EnableWhileChecked(QCheckBox* parent, const std::vector<QWidget*>& inputs);

/// Appends a group box per group to `layout`, filled with what `build` returns for each of its
/// settings; `build` returns nullptr for a setting without a row or placed elsewhere. Boxes left
/// empty are dropped. Returns the boxes by group index, nullptr for the dropped ones.
std::vector<QGroupBox*> AddSettingGroups(
    QBoxLayout& layout, const std::vector<SettingGroup>& groups,
    const std::function<QWidget*(Settings::BasicSetting*)>& build);

/// Makes a group box built by hand advanced, like the ones of an advanced SettingGroup. `changed`
/// keeps it shown, as for a group with a setting changed from its default.
void MarkAdvancedGroup(QGroupBox& box, bool changed);

/// Shows the advanced groups of `page`, or hides the ones whose settings are all at their default
/// (global configuration) or all follow the global value (per game). Changed groups stay shown, so
/// no change is out of sight.
void ShowAdvancedGroups(QWidget& page, bool show);

/// The "Use the global value" buttons of a per-game `page` that are showing, one for each setting
/// the game sets now, those in advanced groups hidden at the moment included. Rows the page never
/// shows are left out.
[[nodiscard]] std::vector<QPushButton*> OverrideRestoreButtons(const QWidget& page);

/// True when every group of `page` is advanced and hidden, so the page has nothing to show.
[[nodiscard]] bool IsEmptyAdvancedPage(const QWidget& page);

/// Checks that every setting the configuration pages build from the Settings categories is in
/// exactly one table above, logging the ones that are missing or placed twice.
[[nodiscard]] bool LayoutCoversAllSettings();

} // namespace ConfigurationShared
