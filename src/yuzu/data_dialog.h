// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>

#include <QDialog>
#include <QString>

#include "common/common_types.h"
#include "frontend_common/data_manager.h"

class QLabel;
class QListWidget;
class QPushButton;

/// Lists the emulator's data folders (shaders, NAND, mods and saves) with their size, and opens,
/// exports, imports or clears each one. Shaders and saves also lead to their own managers.
class DataDialog : public QDialog {
    Q_OBJECT

public:
    explicit DataDialog(bool emulation_running, QWidget* parent = nullptr);
    ~DataDialog() override;

signals:
    void ShaderManagerRequested();
    void SaveManagerRequested();

private:
    struct Category {
        FrontendCommon::DataManager::DataDir dir;
        const char* icon;
        QString name;
        QString description;
        QString export_name;
    };

    struct Usage {
        u64 bytes{};
        u64 files{};
    };

    void Scan();
    void ShowSelected();
    const Category& Selected() const;
    std::optional<std::string> SelectProfile() const;

    void Open();
    void Export();
    void Import();
    void Clear();

    std::array<Category, 5> categories;
    std::array<std::optional<Usage>, 5> usage;
    bool emulation_running{};
    bool scanning{};

    QListWidget* category_list{};
    QLabel* icon_label{};
    QLabel* title_label{};
    QLabel* description_label{};
    QLabel* path_label{};
    QLabel* size_label{};
    QLabel* hint_label{};
    QLabel* total_label{};
    QPushButton* open_button{};
    QPushButton* export_button{};
    QPushButton* import_button{};
    QPushButton* clear_button{};
    QPushButton* manager_button{};
};
