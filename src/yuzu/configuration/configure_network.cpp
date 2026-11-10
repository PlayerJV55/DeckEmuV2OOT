// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QPushButton>
#include <QtConcurrent/QtConcurrent>
#include "common/settings.h"
#include "core/core.h"
#include "core/internal_network/network_interface.h"
#include "ui_configure_network.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_network.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureNetwork::ConfigureNetwork(const Core::System& system_, QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureNetwork>()), system{system_} {
    ui->setupUi(this);
    for (const auto& iface : Network::GetAvailableNetworkInterfaces())
        ui->network_interface->addItem(QString::fromStdString(iface.name));

    if (!Settings::IsConfiguringGlobal()) {
        // The interface is not switchable: only the global window may write it
        ui->label_4->setVisible(false);
        ui->network_interface->setVisible(false);

        airplane_restore_button = ConfigurationShared::Widget::CreateRestoreGlobalButton(
            Settings::values.airplane_mode.UsingGlobal(), this);
        ui->gridLayout_2->addWidget(airplane_restore_button, 2, 2);
        connect(airplane_restore_button, &QAbstractButton::clicked, this, [this] {
            airplane_restore_button->setEnabled(false);
            airplane_restore_button->setVisible(false);
            ui->airplane_mode->setChecked(Settings::values.airplane_mode.GetValue(true));
        });
        connect(ui->airplane_mode, &QAbstractButton::clicked, this, [this] {
            airplane_restore_button->setEnabled(true);
            airplane_restore_button->setVisible(true);
        });
    }

    this->SetConfiguration();
    SetupToolTips();
}

ConfigureNetwork::~ConfigureNetwork() = default;

void ConfigureNetwork::ApplyConfiguration() {
    if (Settings::IsConfiguringGlobal()) {
        Settings::values.network_interface = ui->network_interface->currentText().toStdString();
        // A running game's own value is not the global one
        if (Settings::values.airplane_mode.UsingGlobal()) {
            Settings::values.airplane_mode.SetValue(ui->airplane_mode->isChecked());
        }
        return;
    }

    // Writing while use_global is set would change the global value
    const bool custom = airplane_restore_button->isEnabled();
    Settings::values.airplane_mode.SetGlobal(!custom);
    if (custom) {
        Settings::values.airplane_mode.SetValue(ui->airplane_mode->isChecked());
    }
}

void ConfigureNetwork::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNetwork::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();
}

void ConfigureNetwork::SetupToolTips() {
    using ConfigurationShared::SetRichToolTip;

    SetRichToolTip(ui->network_interface, ui->label_4->text(),
                   tr("Chooses which of this PC's network adapters the emulated console uses for "
                      "its IP address and for local wireless play in multiplayer rooms. Lists only "
                      "connected adapters with an IPv4 address. Cannot change while a game runs."),
                   tr("If multiplayer says no valid network interface is selected, pick your "
                      "active adapter here while no game is running."));
    // Properties reuses this page for a single game
    if (Settings::IsConfiguringGlobal()) {
        SetRichToolTip(ui->airplane_mode, ui->airplane_mode->text(),
                       tr("Tells games wireless is off: internet connection requests fail and TCP "
                          "connections are refused. On by default. Greyed out here while the "
                          "running game uses its own value."),
                       tr("To let a single game connect, untick it in that game's Properties > "
                          "Network instead."));
    } else {
        SetRichToolTip(ui->airplane_mode, ui->airplane_mode->text(),
                       tr("Tells this game wireless is off: internet connection requests fail and "
                          "TCP connections are refused. Changing it gives this game its own value; "
                          "the restore button next to it goes back to the global setting."));
    }
}

void ConfigureNetwork::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();
    auto const network_interface = Settings::values.network_interface.GetValue();
    auto const airplane_mode = Settings::values.airplane_mode.GetValue();
    ui->network_interface->setCurrentText(QString::fromStdString(network_interface));
    ui->network_interface->setEnabled(runtime_lock);
    ui->airplane_mode->setChecked(airplane_mode);
    if (Settings::IsConfiguringGlobal()) {
        ui->airplane_mode->setEnabled(Settings::values.airplane_mode.UsingGlobal());
    }
}
