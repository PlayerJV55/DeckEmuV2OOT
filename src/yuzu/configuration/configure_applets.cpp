// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCheckBox>
#include <QGroupBox>
#include <QVBoxLayout>
#include "common/settings.h"
#include "core/core.h"
#include "ui_configure_applets.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_applets.h"
#include "yuzu/configuration/settings_layout.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureApplets::ConfigureApplets(Core::System& system_,
                                   std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
                                   const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureApplets>()}, system{system_} {
    ui->setupUi(this);

    Setup(builder);

    SetConfiguration();
}

ConfigureApplets::~ConfigureApplets() = default;

void ConfigureApplets::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureApplets::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureApplets::Setup(const ConfigurationShared::Builder& builder) {
    ConfigurationShared::AddSettingGroups(
        *ui->groups_layout, ConfigurationShared::Layout::Applets(),
        [&](Settings::BasicSetting* setting) -> QWidget* {
            return ConfigurationShared::BuildSettingWidget(builder, setting, apply_funcs);
        });

    // Global only, and without Qt WebEngine there is no web applet to disable
#ifdef YUZU_USE_QT_WEB_ENGINE
    if (Settings::IsConfiguringGlobal()) {
        auto* const box = new QGroupBox(tr("Advanced"), this);
        auto* const layout = new QVBoxLayout(box);
        disable_web_applet = new QCheckBox(tr("Disable web applet"), box);
        disable_web_applet->setChecked(Settings::values.disable_web_applet.GetValue());
        ConfigurationShared::SetRichToolTip(
            disable_web_applet, disable_web_applet->text(),
            tr("Closes the pages the Offline web applet would show, such as a game's manual or "
               "legal notices, right away as if the player backed out. Shop, login and similar "
               "screens close either way, and website links open in your PC browser."),
            tr("Enable XInput 8 player support also blocks these pages, so turn it off too if a "
               "game needs them."));
        layout->addWidget(disable_web_applet);
        ui->groups_layout->addWidget(box);
        ConfigurationShared::MarkAdvancedGroup(
            *box, Settings::values.disable_web_applet.GetValue() !=
                      Settings::values.disable_web_applet.GetDefault());
    }
#endif
}

void ConfigureApplets::SetConfiguration() {}

void ConfigureApplets::ApplyConfiguration() {
    const bool powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(powered_on);
    }

    if (Settings::IsConfiguringGlobal()) {
#ifdef YUZU_USE_QT_WEB_ENGINE
        Settings::values.disable_web_applet = disable_web_applet->isChecked();
#else
        Settings::values.disable_web_applet = true;
#endif
    }
}
