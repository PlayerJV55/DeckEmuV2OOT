// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include <QDialog>
#include <QString>

#include "common/common_types.h"
#include "yuzu/shader_manager_dialog.h"

class QLabel;
class QListWidget;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

/// Lists each game's save data per profile, its backups and the same game's saves in other
/// emulators. Backs saves up, restores and imports them (backing up the save they replace),
/// exports and deletes them.
class SaveManagerDialog : public QDialog {
    Q_OBJECT
public:
    SaveManagerDialog(std::vector<ShaderManagerGame> games, u64 selected_program_id,
                      u64 running_program_id, QWidget* parent = nullptr);
    ~SaveManagerDialog() override;

private:
    void Populate(u64 select_program_id);
    void ShowSelectedGame();
    void UpdateButtons();
    void BackUp();
    void Import();
    void ImportFolder(const std::filesystem::path& source, const QString& source_user,
                      const QString& source_name);
    void ImportZip(const std::filesystem::path& zip, const QString& source_user);
    void Export();
    void Delete();
    void OpenFolder();

    std::optional<QString> TargetUser(const QString& source_user);
    bool ReplaceSave(const QString& user, const std::filesystem::path& source);
    const QTreeWidgetItem* SelectedItem() const;
    const QTreeWidgetItem* SelectedOwnSave() const;
    u64 SelectedProgramId() const;
    const ShaderManagerGame* FindGame(u64 program_id) const;
    QString GameTitle(u64 program_id) const;

    std::vector<ShaderManagerGame> games;
    u64 running_program_id{};

    QListWidget* game_list{};
    QLabel* icon_label{};
    QLabel* title_label{};
    QLabel* summary_label{};
    QTreeWidget* save_tree{};
    QLabel* hint_label{};
    QPushButton* backup_button{};
    QPushButton* import_button{};
    QPushButton* export_button{};
    QPushButton* delete_button{};
    QPushButton* folder_button{};
};
