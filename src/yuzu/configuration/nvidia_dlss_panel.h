// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HAS_NVIDIA_STREAMLINE

#include <functional>

#include <QCoreApplication>
#include <QWidget>

class QLabel;
class QPushButton;
class QShowEvent;

/// Shown under "Use NVIDIA DLSS": which of NVIDIA's files DLSS needs were found and their
/// versions, a button that opens the folder they go in, where they come from, and a button that
/// fetches missing ones from NVIDIA's own Streamline release.
class NvidiaDlssPanel : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(NvidiaDlssPanel)

public:
    explicit NvidiaDlssPanel(QWidget* parent);
    ~NvidiaDlssPanel() override;

    void Refresh();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void Download();

    QLabel* icon_label;
    QLabel* status_label;
    QPushButton* download_button;
    std::function<void()> cancel_download; ///< Set while a download runs
};

#endif
