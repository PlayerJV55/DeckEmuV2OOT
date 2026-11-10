// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QPixmap>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;

/// The strip at the top of game properties: cover, name and the facts about the game file.
class ConfigurePerGameHeader : public QWidget {
    Q_OBJECT

public:
    struct Info {
        QString name;
        QString developer; ///< The row is hidden when empty
        QString version;
        QString title_id;
        QString format;
        QString size;
        QString path; ///< Host path of the game file
        QPixmap cover;
    };

    explicit ConfigurePerGameHeader(QWidget* parent = nullptr);
    ~ConfigurePerGameHeader() override;

    void SetInfo(const Info& info);

private:
    class ElidedLabel;

    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    void OpenFileLocation();

    QString path;

    QLabel* cover_label;
    ElidedLabel* name_label;
    QLabel* developer_label;
    QLabel* version_key;
    QLabel* version_label;
    QLabel* title_id_key;
    QLabel* title_id_label;
    QToolButton* copy_title_id_button;
    QLabel* format_key;
    QLabel* format_label;
    QLabel* size_key;
    QLabel* size_label;
    QLabel* path_key;
    ElidedLabel* path_label;
    QPushButton* open_location_button;
    QPushButton* copy_path_button;
};
