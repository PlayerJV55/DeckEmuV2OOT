// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <vector>

#include <QDialog>
#include <QPixmap>
#include <QString>

#include "common/common_types.h"

class QLabel;
class QListWidget;
class QPushButton;
class QTreeWidget;

/// A game of the game list, as the shader manager shows it.
struct ShaderManagerGame {
    u64 program_id{};
    QString title;
    QPixmap icon;
    QString path;
};

/// Lists each game's shader caches: the one this build uses, the ones other versions and
/// emulators left, and caches of the same game in other emulators' folders. Imports, exports,
/// cleans up and deletes them, and starts Prepare Shaders.
class ShaderManagerDialog : public QDialog {
    Q_OBJECT
public:
    ShaderManagerDialog(std::vector<ShaderManagerGame> games, u64 selected_program_id,
                        bool emulation_running, QWidget* parent = nullptr);
    ~ShaderManagerDialog() override;

signals:
    void PrepareShadersRequested(const QString& game_path);

private:
    void Populate(u64 select_program_id);
    void ShowSelectedGame();
    void UpdateButtons();
    void Import();
    void ImportFile(const std::filesystem::path& file);
    void Export();
    void CleanUp();
    void DeleteCache();
    void OpenFolder();
    void PrepareShaders();

    u64 SelectedProgramId() const;
    const ShaderManagerGame* FindGame(u64 program_id) const;
    std::filesystem::path GameDir(u64 program_id) const;
    QString GameTitle(u64 program_id) const;

    std::vector<ShaderManagerGame> games;
    bool emulation_running{};

    QListWidget* game_list{};
    QLabel* icon_label{};
    QLabel* title_label{};
    QLabel* summary_label{};
    QTreeWidget* file_tree{};
    QLabel* hint_label{};
    QPushButton* import_button{};
    QPushButton* export_button{};
    QPushButton* prepare_button{};
    QPushButton* clean_button{};
    QPushButton* delete_button{};
    QPushButton* folder_button{};
};
