// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <typeinfo>
#include <vector>
#include <QBoxLayout>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include "common/common_types.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "configuration/shared_widget.h"
#include "core/core.h"
#include "ui_configure_cpu.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/settings_layout.h"

ConfigureCpu::ConfigureCpu(const Core::System& system_,
                           std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
                           const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureCpu>()}, system{system_},
      combobox_translations(builder.ComboboxTranslations()) {
    ui->setupUi(this);

    Setup(builder);

    SetConfiguration();

    connect(accuracy_combobox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ConfigureCpu::UpdateGroup);

    connect(backend_combobox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ConfigureCpu::UpdateGroup);

    SetupToolTips();
}

ConfigureCpu::~ConfigureCpu() = default;

void ConfigureCpu::SetConfiguration() {}
void ConfigureCpu::Setup(const ConfigurationShared::Builder& builder) {
    QWidget* accuracy_widget{};
    QWidget* unsafe_widget{};

    // The notes go into the group boxes, right under what they explain
    ui->vboxlayout->removeWidget(ui->label_recommended_accuracy);
    ui->vboxlayout->removeWidget(ui->label_accuracy_description);

    ConfigurationShared::AddSettingGroups(
        *ui->groups_layout, ConfigurationShared::Layout::Emulation(),
        [&](Settings::BasicSetting* setting) -> QWidget* {
            auto* const widget =
                ConfigurationShared::BuildSettingWidget(builder, setting, apply_funcs);
            if (widget == nullptr) {
                return nullptr;
            }
            if (setting->Id() == Settings::values.cpu_accuracy.Id()) {
                // Keep track of cpu_accuracy combobox to display/hide the unsafe settings
                accuracy_widget = widget;
                accuracy_combobox = widget->combobox;
            } else if (setting->Id() == Settings::values.cpu_backend.Id()) {
                backend_combobox = widget->combobox;
#ifndef HAS_NCE
                // Dynarmic is the only backend
                widget->setVisible(false);
#endif
            } else if (setting->GetCategory() == Settings::Category::CpuUnsafe &&
                       unsafe_widget == nullptr) {
                unsafe_widget = widget;
            }
            return widget;
        });

    if (accuracy_widget != nullptr) {
        auto* const layout = static_cast<QBoxLayout*>(accuracy_widget->parentWidget()->layout());
        layout->insertWidget(layout->indexOf(accuracy_widget) + 1,
                             ui->label_recommended_accuracy);
    } else {
        ui->label_recommended_accuracy->setVisible(false);
    }
    if (unsafe_widget != nullptr) {
        unsafe_group = qobject_cast<QGroupBox*>(unsafe_widget->parentWidget());
        static_cast<QBoxLayout*>(unsafe_group->layout())
            ->insertWidget(0, ui->label_accuracy_description);
    } else {
        ui->label_accuracy_description->setVisible(false);
    }

    UpdateGroup();
}

void ConfigureCpu::UpdateGroup() {
    if (unsafe_group == nullptr) {
        return;
    }
    const u32 accuracy = accuracy_combobox->currentIndex();
    const u32 backend = backend_combobox->currentIndex();
    // TODO(crueter): see if this works on NCE
    unsafe_group->setVisible(accuracy == (u32)Settings::CpuAccuracy::Unsafe &&
                             backend == (u32)Settings::CpuBackend::Dynarmic);
}

void ConfigureCpu::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& apply_func : apply_funcs) {
        apply_func(is_powered_on);
    }
}

void ConfigureCpu::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureCpu::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();
}

void ConfigureCpu::SetupToolTips() {
    ConfigurationShared::SetRichToolTip(
        ui->label_recommended_accuracy, ui->label_recommended_accuracy->text(),
        tr("Auto turns on a fixed set of CPU speedups the emulator treats as safe. Accurate, "
           "Paranoid and Debugging are for troubleshooting; Unsafe trades accuracy for speed."));
    ConfigurationShared::SetRichToolTip(
        ui->label_accuracy_description, ui->label_accuracy_description->text(),
        tr("Shown only with CPU accuracy set to Unsafe. Each ticked option trades CPU accuracy "
           "for speed; none can change while a game runs."),
        tr("If a game breaks only under Unsafe, untick options one at a time to find the cause, "
           "or return to Auto."));
}
