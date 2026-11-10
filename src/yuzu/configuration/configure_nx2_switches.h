// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <QString>
#include <QWidget>
#include "common/nx2_switches.h"

namespace Nx2SwitchCatalog {
struct Entry;
}

namespace Ui {
class ConfigureNx2Switches;
}

/// A table of the emulator's environment switches. Ticked rows go to this install's switch file
/// (Common::GetUserNx2SwitchFile), which LoadNx2Switches reads at the next start, or to the switch
/// profile picked at the top, which games set to use it get over the global switches.
class ConfigureNx2Switches : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureNx2Switches(QWidget* parent = nullptr);
    ~ConfigureNx2Switches() override;

    /// Reloads the table from the switch file or profile being edited.
    void SetConfiguration();
    /// Writes the ticked switches to the switch file or profile, if they changed. False if it
    /// couldn't be written.
    bool ApplyConfiguration();

private:
    struct Row {
        const Nx2SwitchCatalog::Entry* entry; ///< nullptr for a switch the catalog doesn't know
        QString name;
        QString search_text; ///< Lowercase name and description, for the search box
        bool locked;         ///< Set in the real environment, or not settable from the file
        QString presence_value; ///< What a Presence switch is set to while ticked
    };

    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void UpdateFileLabel();
    void PopulateCategoryFilter();
    void PopulateProfileCombo();

    /// Rebuilds the table; `ticked` are the switches shown ticked, with their values.
    void Populate(const std::vector<Common::Nx2Switch>& ticked);
    void AddRow(const Nx2SwitchCatalog::Entry* entry, const QString& name);
    /// Puts an editor for `value` in the row's Value cell, a line edit if the typed one can't
    /// show it.
    void SetEditor(int index, const QString& value);
    /// Editors are enabled while their row is ticked and the page may change the switch.
    void UpdateEditorEnabled(int index);
    [[nodiscard]] QString EditorValue(int index) const;
    [[nodiscard]] bool IsTicked(int index) const;
    void SetTicked(int index, bool ticked);
    [[nodiscard]] int FindRow(const QString& name) const;
    [[nodiscard]] QString ToolTip(const Row& row) const;
    [[nodiscard]] std::vector<Common::Nx2Switch> CurrentSwitches() const;
    void UpdateFilter();

    void Import();
    void Export();
    void ResetAll();

    void SelectProfile(int index);
    void NewProfile();
    void RenameProfile();
    void DeleteProfile();
    [[nodiscard]] bool HasChanges() const;
    /// Asks whether to save changed switches before another file is edited; false to stay.
    bool SaveOrDiscardChanges();
    /// Asks for a profile name until it is valid and free, or the user cancels.
    [[nodiscard]] std::optional<std::string> AskProfileName(const QString& title,
                                                            const QString& current);

    std::unique_ptr<Ui::ConfigureNx2Switches> ui;
    /// The profile being edited, empty for the global switches.
    std::string profile;
    std::filesystem::path switch_file;
    /// The switch file's assignments, last one winning, as last read or written.
    std::map<std::string, std::string> saved;
    /// Switches the other switch files set, with the file, for the tooltips. While a profile is
    /// edited, every global switch file.
    std::map<std::string, std::pair<std::string, std::filesystem::path>> other_files;
    std::vector<Row> rows;
};
