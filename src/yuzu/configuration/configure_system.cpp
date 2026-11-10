// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <optional>
#include <vector>

#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QFileDialog>
#include <QGraphicsItem>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QSpinBox>

#include "common/settings.h"
#include "core/core.h"
#include "qt_common/qt_compat.h"
#include "ui_configure_system.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/configuration/settings_layout.h"
#include "yuzu/configuration/shared_widget.h"

constexpr std::array<u32, 7> LOCALE_BLOCKLIST{
    // pzzefezrpnkzeidfej
    // thhsrnhutlohsternp
    // BHH4CG          U
    // Raa1AB          S
    //  nn9
    //  ts
    0b0100011100001100000, // Japan
    0b0000001101001100100, // Americas
    0b0100110100001000010, // Europe
    0b0100110100001000010, // Australia
    0b0000000000000000000, // China
    0b0100111100001000000, // Korea
    0b0100111100001000000, // Taiwan
};

static bool IsValidLocale(u32 region_index, u32 language_index) {
    if (region_index >= LOCALE_BLOCKLIST.size()) {
        return false;
    }
    return ((LOCALE_BLOCKLIST.at(region_index) >> language_index) & 1) == 0;
}

ConfigureSystem::ConfigureSystem(Core::System& system_,
                                 std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
                                 const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureSystem>()}, system{system_} {
    ui->setupUi(this);

    const auto posix_time = std::chrono::system_clock::now().time_since_epoch();
    const auto current_time_s =
        std::chrono::duration_cast<std::chrono::seconds>(posix_time).count();
    previous_time = current_time_s + Settings::values.custom_rtc_offset.GetValue();

    Setup(builder);

    const auto locale_check = [this]() {
        const auto region_index = combo_region->currentIndex();
        const auto language_index = combo_language->currentIndex();
        const bool valid_locale = IsValidLocale(region_index, language_index);
        ui->label_warn_invalid_locale->setVisible(!valid_locale);
        if (!valid_locale) {
            ui->label_warn_invalid_locale->setText(
                tr("Warning: \"%1\" is not a valid language for region \"%2\"")
                    .arg(combo_language->currentText())
                    .arg(combo_region->currentText()));
        }
    };

    const auto update_date_offset = [this]() {
        if (!checkbox_rtc->isChecked()) {
            return;
        }
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const auto selected_s = date_rtc->dateTime().toSecsSinceEpoch();
        const auto offset = static_cast<int>(selected_s - now_s);
        previous_time = selected_s;
        QSignalBlocker blocker(date_rtc_offset);
        date_rtc_offset->setValue(offset);
    };
    const auto update_rtc_date = [this](int _) {
        QSignalBlocker blocker(date_rtc);
        UpdateRtcTime();
    };

    connect(combo_language, qOverload<int>(&QComboBox::currentIndexChanged), this, locale_check);
    connect(combo_region, qOverload<int>(&QComboBox::currentIndexChanged), this, locale_check);
    connect(checkbox_rtc, qOverload<CHECKSTATE_TYPE>(&QCheckBox::STATE_CHANGED), this,
            update_rtc_date);
    connect(date_rtc_offset, qOverload<int>(&QSpinBox::valueChanged), this, update_rtc_date);
    connect(date_rtc, &QDateTimeEdit::dateTimeChanged, this, update_date_offset);

    ui->label_warn_invalid_locale->setVisible(false);
    locale_check();

    SetConfiguration();
    UpdateRtcTime();

    SetupToolTips();
}

ConfigureSystem::~ConfigureSystem() = default;

void ConfigureSystem::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureSystem::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();
}

void ConfigureSystem::SetupToolTips() {
    ConfigurationShared::SetRichToolTip(
        ui->label_warn_invalid_locale, tr("Language and region"),
        tr("Appears when the chosen region and language don't usually go together. It is only a "
           "warning: Nemyu still uses the pair."));
}

void ConfigureSystem::Setup(const ConfigurationShared::Builder& builder) {
    // Goes under the language and region rows
    ui->verticalLayout->removeWidget(ui->label_warn_invalid_locale);
    QWidget* time_zone_widget{};

    const auto build = [&](Settings::BasicSetting* setting) -> QWidget* {
        ConfigurationShared::Widget* widget{};
        if (setting->Id() == Settings::values.use_docked_mode.Id() &&
            Settings::IsConfiguringGlobal()) {
            // The Controls page saves the global mode, so this row only mirrors its choice
            widget = builder.BuildWidget(setting, apply_funcs,
                                         ConfigurationShared::RequestType::Default, false);
            if (widget != nullptr && !widget->Valid()) {
                widget->deleteLater();
                widget = nullptr;
            }
            if (widget != nullptr) {
                console_mode_buttons = widget->radio_buttons;
                for (const auto& [id, button] : console_mode_buttons) {
                    const bool docked = id == static_cast<u32>(Settings::ConsoleMode::Docked);
                    connect(button, &QAbstractButton::clicked, this,
                            [this, docked] { emit ConsoleModeChanged(docked); });
                }
            }
            return widget;
        }

        widget = ConfigurationShared::BuildSettingWidget(builder, setting, apply_funcs);
        if (widget == nullptr) {
            return nullptr;
        }

        // Keep track of the region_index (and language_index) combobox to validate the selected
        // settings
        if (setting->Id() == Settings::values.region_index.Id()) {
            combo_region = widget->combobox;
        }

        if (setting->Id() == Settings::values.language_index.Id()) {
            combo_language = widget->combobox;
        }

        if (setting->Id() == Settings::values.time_zone_index.Id()) {
            time_zone_widget = widget;
        }

        if (setting->Id() == Settings::values.custom_rtc.Id()) {
            checkbox_rtc = widget->checkbox;
        }

        if (setting->Id() == Settings::values.custom_rtc.Id()) {
            date_rtc = widget->date_time_edit;
        }

        if (setting->Id() == Settings::values.custom_rtc_offset.Id()) {
            date_rtc_offset = widget->spinbox;
            widget->setVisible(false);
        }
        return widget;
    };
    ConfigurationShared::AddSettingGroups(*ui->groups_layout,
                                          ConfigurationShared::Layout::System(), build);

    if (time_zone_widget != nullptr) {
        static_cast<QBoxLayout*>(time_zone_widget->parentWidget()->layout())
            ->addWidget(ui->label_warn_invalid_locale);
    }
}

void ConfigureSystem::SetConsoleMode(bool docked, bool locked) {
    for (const auto& [id, button] : console_mode_buttons) {
        button->setChecked((id == static_cast<u32>(Settings::ConsoleMode::Docked)) == docked);
        button->setEnabled(!locked);
    }
}

void ConfigureSystem::UpdateRtcTime() {
    const auto posix_time = std::chrono::system_clock::now().time_since_epoch();
    previous_time = std::chrono::duration_cast<std::chrono::seconds>(posix_time).count();
    date_rtc_offset->setEnabled(checkbox_rtc->isChecked());

    if (checkbox_rtc->isChecked()) {
        previous_time += date_rtc_offset->value();
    }

    const auto date = QDateTime::fromSecsSinceEpoch(previous_time);
    date_rtc->setDateTime(date);
}

void ConfigureSystem::SetConfiguration() {}

void ConfigureSystem::ApplyConfiguration() {
    const bool powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(powered_on);
    }
    UpdateRtcTime();
}
