// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include "common/net/net.h"

namespace Ui {
class UpdateDialog;
}

class UpdateDialog : public QDialog {
    Q_OBJECT

public:
    // exec() returns QDialog::Accepted to download, QDialog::Rejected to be reminded later.
    static constexpr int SkipVersion = 2;

    explicit UpdateDialog(const Common::Net::Release& release, QWidget* parent = nullptr);
    ~UpdateDialog();

private slots:
    void Download();

private:
    Ui::UpdateDialog* ui;
    Common::Net::Asset m_asset;
    Common::Net::Asset m_checksum;
};
