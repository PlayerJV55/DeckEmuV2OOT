// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <functional>
#include <utility>
#include <vector>
#include <QDir>
#include <QFileDialog>
#include <QListWidget>
#include <QMessageBox>
#include "common/settings.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "ui_configure_general.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_general.h"
#include "yuzu/configuration/settings_layout.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureGeneral::ConfigureGeneral(const Core::System& system_,
                                   std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
                                   const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGeneral>()}, system{system_} {
    ui->setupUi(this);

    Setup(builder);

    SetConfiguration();

    connect(ui->button_reset_defaults, &QPushButton::clicked, this,
            &ConfigureGeneral::ResetDefaults);

    if (!Settings::IsConfiguringGlobal()) {
        ui->button_reset_defaults->setVisible(false);
    }

    SetupToolTips();
}

ConfigureGeneral::~ConfigureGeneral() = default;

void ConfigureGeneral::SetConfiguration() {}

void ConfigureGeneral::Setup(const ConfigurationShared::Builder& builder) {
    ConfigurationShared::AddSettingGroups(
        *ui->groups_layout, ConfigurationShared::Layout::General(),
        [&](Settings::BasicSetting* setting) -> QWidget* {
#ifndef ENABLE_UPDATE_CHECKER
            // Nothing reads it without the update checker
            if (setting->Id() == UISettings::values.check_for_updates.Id()) {
                return nullptr;
            }
#endif
            return ConfigurationShared::BuildSettingWidget(builder, setting, apply_funcs);
        });
}

// Called to set the callback when resetting settings to defaults
void ConfigureGeneral::SetResetCallback(std::function<void()> callback) {
    reset_callback = std::move(callback);
}

void ConfigureGeneral::ResetDefaults() {
    QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Nemyu"),
        tr("This resets all settings and removes all per-game configurations. This will not "
           "delete game directories, profiles, or input profiles. Proceed?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::No) {
        return;
    }
    UISettings::values.reset_to_defaults = true;
    UISettings::values.is_game_list_reload_pending.exchange(true);
    reset_callback();
}

void ConfigureGeneral::ApplyConfiguration() {
    bool powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(powered_on);
    }
}

void ConfigureGeneral::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGeneral::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();
}

void ConfigureGeneral::SetupToolTips() {
    using ConfigurationShared::SetRichToolTip;

    SetRichToolTip(ui->button_reset_defaults, ui->button_reset_defaults->text(),
                   tr("Asks first, then resets every setting, deletes all per-game configs and "
                      "the game list cache, and closes this window. Controller mappings and "
                      "hotkeys go back to defaults and disabled add-ons are re-enabled; game "
                      "folders, favorites and user profiles stay."),
                   tr("Save your controller setup as an input profile first; saved input "
                      "profiles are kept."));
}
