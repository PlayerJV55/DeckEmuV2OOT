// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <map>
#include <memory>
#include <vector>
#include <QComboBox>
#include <QPushButton>

#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_details.h"
#include "common/common_types.h"
#include "common/settings.h"
#include "common/settings_common.h"
#include "core/core.h"
#include "qt_common/config/shared_translation.h"
#include "qt_common/config/uisettings.h"
#include "ui_configure_audio.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/settings_layout.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureAudio::ConfigureAudio(const Core::System& system_,
                               std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
                               const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui(std::make_unique<Ui::ConfigureAudio>()), system{system_} {
    ui->setupUi(this);
    Setup(builder);

    SetConfiguration();
}

ConfigureAudio::~ConfigureAudio() = default;

void ConfigureAudio::Setup(const ConfigurationShared::Builder& builder) {
    const auto build = [&](Settings::BasicSetting* setting) -> QWidget* {
        auto* widget = ConfigurationShared::BuildSettingWidget(builder, setting, apply_funcs);
        if (widget == nullptr) {
            return nullptr;
        }

        auto global_sink_match = [this] {
            return static_cast<Settings::AudioEngine>(sink_combo_box->currentIndex()) ==
                   Settings::values.sink_id.GetValue(true);
        };
        if (setting->Id() == Settings::values.sink_id.Id()) {
            // TODO (lat9nq): Let the system manage sink_id
            sink_combo_box = widget->combobox;
            InitializeAudioSinkComboBox();

            if (Settings::IsConfiguringGlobal()) {
                connect(sink_combo_box, qOverload<int>(&QComboBox::currentIndexChanged), this,
                        &ConfigureAudio::UpdateAudioDevices);
            } else {
                restore_sink_button = ConfigurationShared::Widget::CreateRestoreGlobalButton(
                    Settings::values.sink_id.UsingGlobal(), widget);
                widget->layout()->addWidget(restore_sink_button);
                connect(restore_sink_button, &QAbstractButton::clicked, [this](bool) {
                    Settings::values.sink_id.SetGlobal(true);
                    const int sink_index = static_cast<int>(Settings::values.sink_id.GetValue());
                    sink_combo_box->setCurrentIndex(sink_index);
                    ConfigureAudio::UpdateAudioDevices(sink_index);
                    Settings::values.audio_output_device_id.SetGlobal(true);
                    Settings::values.audio_input_device_id.SetGlobal(true);
                    restore_sink_button->setVisible(false);
                });
                connect(sink_combo_box, qOverload<int>(&QComboBox::currentIndexChanged),
                        [this, global_sink_match](const int slot) {
                            Settings::values.sink_id.SetGlobal(false);
                            Settings::values.audio_output_device_id.SetGlobal(false);
                            Settings::values.audio_input_device_id.SetGlobal(false);

                            restore_sink_button->setVisible(true);
                            restore_sink_button->setEnabled(true);
                            output_device_combo_box->setCurrentIndex(0);
                            restore_output_device_button->setVisible(true);
                            restore_output_device_button->setEnabled(global_sink_match());
                            input_device_combo_box->setCurrentIndex(0);
                            restore_input_device_button->setVisible(true);
                            restore_input_device_button->setEnabled(global_sink_match());
                            ConfigureAudio::UpdateAudioDevices(slot);
                        });
            }
        } else if (setting->Id() == Settings::values.audio_output_device_id.Id()) {
            // Keep track of output (and input) device comboboxes to populate them with system
            // devices, which are determined at run time
            output_device_combo_box = widget->combobox;

            if (!Settings::IsConfiguringGlobal()) {
                restore_output_device_button =
                    ConfigurationShared::Widget::CreateRestoreGlobalButton(
                        Settings::values.audio_output_device_id.UsingGlobal(), widget);
                restore_output_device_button->setEnabled(global_sink_match());
                restore_output_device_button->setVisible(
                    !Settings::values.audio_output_device_id.UsingGlobal());
                widget->layout()->addWidget(restore_output_device_button);

                connect(restore_output_device_button, &QAbstractButton::clicked, [this](bool) {
                    Settings::values.audio_output_device_id.SetGlobal(true);
                    SetOutputDevicesFromDeviceID();
                    restore_output_device_button->setVisible(false);
                });
                connect(output_device_combo_box, qOverload<int>(&QComboBox::currentIndexChanged),
                        [this, global_sink_match](int) {
                            if (updating_devices) {
                                return;
                            }
                            Settings::values.audio_output_device_id.SetGlobal(false);
                            restore_output_device_button->setVisible(true);
                            restore_output_device_button->setEnabled(global_sink_match());
                        });
            }
        } else if (setting->Id() == Settings::values.audio_input_device_id.Id()) {
            input_device_combo_box = widget->combobox;

            if (!Settings::IsConfiguringGlobal()) {
                restore_input_device_button =
                    ConfigurationShared::Widget::CreateRestoreGlobalButton(
                        Settings::values.audio_input_device_id.UsingGlobal(), widget);
                widget->layout()->addWidget(restore_input_device_button);
                connect(restore_input_device_button, &QAbstractButton::clicked, [this](bool) {
                    Settings::values.audio_input_device_id.SetGlobal(true);
                    SetInputDevicesFromDeviceID();
                    restore_input_device_button->setVisible(false);
                });
                connect(input_device_combo_box, qOverload<int>(&QComboBox::currentIndexChanged),
                        [this, global_sink_match](int) {
                            if (updating_devices) {
                                return;
                            }
                            Settings::values.audio_input_device_id.SetGlobal(false);
                            restore_input_device_button->setVisible(true);
                            restore_input_device_button->setEnabled(global_sink_match());
                        });
            }
        }
        return widget;
    };
    ConfigurationShared::AddSettingGroups(*ui->groups_layout,
                                          ConfigurationShared::Layout::Audio(), build);
}

void ConfigureAudio::SetConfiguration() {
    SetOutputSinkFromSinkID();

    // The device list cannot be pre-populated (nor listed) until the output sink is known.
    UpdateAudioDevices(sink_combo_box->currentIndex());

    SetOutputDevicesFromDeviceID();
    SetInputDevicesFromDeviceID();
}

void ConfigureAudio::SetOutputSinkFromSinkID() {
    [[maybe_unused]] const QSignalBlocker blocker(sink_combo_box);

    const std::string new_sink_id = []() -> const std::string {
        const Settings::AudioEngine sink_id = Settings::values.sink_id.GetValue();
        const auto canonicalizations =
            Settings::EnumMetadata<Settings::AudioEngine>::Canonicalizations();

        for (u32 i = 0; i < canonicalizations.size(); ++i) {
            const Settings::AudioEngine value = canonicalizations[i].second;
            const std::string_view key = canonicalizations[i].first;

            if (sink_id == value)
                return std::string(key);
        }

        return std::string("null");
    }();

    sink_combo_box->setCurrentIndex(
        std::max(0, sink_combo_box->findData(QString::fromStdString(new_sink_id))));
}

void ConfigureAudio::SetOutputDevicesFromDeviceID() {
    int new_device_index = 0;

    const QString output_device_id =
        QString::fromStdString(Settings::values.audio_output_device_id.GetValue());
    for (int index = 0; index < output_device_combo_box->count(); index++) {
        if (output_device_combo_box->itemData(index).toString() == output_device_id) {
            new_device_index = index;
            break;
        }
    }

    output_device_combo_box->setCurrentIndex(new_device_index);
}

void ConfigureAudio::SetInputDevicesFromDeviceID() {
    int new_device_index = 0;
    const QString input_device_id =
        QString::fromStdString(Settings::values.audio_input_device_id.GetValue());
    for (int index = 0; index < input_device_combo_box->count(); index++) {
        if (input_device_combo_box->itemData(index).toString() == input_device_id) {
            new_device_index = index;
            break;
        }
    }

    input_device_combo_box->setCurrentIndex(new_device_index);
}

void ConfigureAudio::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& apply_func : apply_funcs) {
        apply_func(is_powered_on);
    }

    const u32 new_sink_id = [this]() {
        const std::string sink_id = sink_combo_box->currentData().toString().toStdString();
        const auto canonicalizations =
            Settings::EnumMetadata<Settings::AudioEngine>::Canonicalizations();

        for (u32 i = 0; i < canonicalizations.size(); ++i) {
            if (sink_id == canonicalizations[i].first)
                return i;
        }

        return u32(0);
    }();

    Settings::values.sink_id.SetValue(Settings::AudioEngine(new_sink_id));
    Settings::values.audio_output_device_id.SetValue(
        output_device_combo_box->currentData().toString().toStdString());
    Settings::values.audio_input_device_id.SetValue(
        input_device_combo_box->currentData().toString().toStdString());
}

void ConfigureAudio::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureAudio::UpdateAudioDevices(int sink_index) {
    updating_devices = true;
    output_device_combo_box->clear();
    output_device_combo_box->addItem(tr("Auto"),
                                     QString::fromUtf8(AudioCore::Sink::auto_device_name));

    const auto sink_id =
        Settings::ToEnum<Settings::AudioEngine>(
            sink_combo_box->itemData(sink_index).toString().toStdString());
    for (const auto& device : AudioCore::Sink::GetDeviceListForSink(sink_id, false)) {
        output_device_combo_box->addItem(QString::fromStdString(device),
                                         QString::fromStdString(device));
    }

    input_device_combo_box->clear();
    input_device_combo_box->addItem(tr("Auto"),
                                    QString::fromUtf8(AudioCore::Sink::auto_device_name));
    for (const auto& device : AudioCore::Sink::GetDeviceListForSink(sink_id, true)) {
        input_device_combo_box->addItem(QString::fromStdString(device),
                                        QString::fromStdString(device));
    }
    updating_devices = false;
}

void ConfigureAudio::InitializeAudioSinkComboBox() {
    sink_combo_box->clear();
    // The combo boxes show readable names; the item data holds the config value.
    sink_combo_box->addItem(tr("Auto"), QString::fromUtf8(AudioCore::Sink::auto_device_name));
    for (const auto& id : AudioCore::Sink::GetSinkIDs()) {
        const QString key = QString::fromStdString(std::string{Settings::CanonicalizeEnum(id)});
        QString name = key;
        if (key == QStringLiteral("cubeb")) {
            name = QStringLiteral("Cubeb");
        } else if (key == QStringLiteral("sdl3")) {
            name = QStringLiteral("SDL3");
        } else if (key == QStringLiteral("null")) {
            name = tr("None");
        }
        sink_combo_box->addItem(name, key);
    }
}

void ConfigureAudio::RetranslateUI() {
    ui->retranslateUi(this);
}
