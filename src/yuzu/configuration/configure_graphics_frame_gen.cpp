// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCheckBox>
#include <QVBoxLayout>
#include "common/settings.h"
#include "core/core.h"
#include "yuzu/configuration/configure_graphics_frame_gen.h"
#include "yuzu/configuration/settings_layout.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureGraphicsFrameGen::ConfigureGraphicsFrameGen(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), system{system_} {
    setAccessibleName(tr("Frame Generation"));

    auto* const layout = new QVBoxLayout(this);

    QCheckBox* frame_gen_checkbox{};
    QCheckBox* flow_scale_auto_checkbox{};
    QWidget* flow_scale_input{};
    std::vector<QWidget*> inputs;

    ConfigurationShared::AddSettingGroups(
        *layout, ConfigurationShared::Layout::FrameGeneration(),
        [&](Settings::BasicSetting* setting) -> QWidget* {
            auto* const widget =
                ConfigurationShared::BuildSettingWidget(builder, setting, apply_funcs);
            if (widget == nullptr) {
                return nullptr;
            }
            if (setting->Id() == Settings::values.frame_gen.Id()) {
                frame_gen_checkbox = widget->checkbox;
                return widget;
            }
            QWidget* const input = ConfigurationShared::SettingInput(*widget);
            if (input != nullptr) {
                inputs.push_back(input);
            }
            if (setting->Id() == Settings::values.frame_gen_flow_scale_auto.Id()) {
                flow_scale_auto_checkbox = widget->checkbox;
            } else if (setting->Id() == Settings::values.frame_gen_flow_scale.Id()) {
                flow_scale_input = input;
            }
            return widget;
        });
    layout->addStretch();

    if (frame_gen_checkbox == nullptr) {
        return;
    }
    ConfigurationShared::EnableWhileChecked(frame_gen_checkbox, inputs);
    // The flow scale is only used without the automatic one
    if (flow_scale_auto_checkbox != nullptr && flow_scale_input != nullptr) {
        const auto update_flow_scale = [frame_gen_checkbox, flow_scale_auto_checkbox,
                                        flow_scale_input] {
            flow_scale_input->setEnabled(frame_gen_checkbox->isChecked() &&
                                         !flow_scale_auto_checkbox->isChecked());
        };
        connect(frame_gen_checkbox, &QCheckBox::toggled, this, update_flow_scale);
        connect(flow_scale_auto_checkbox, &QCheckBox::toggled, this, update_flow_scale);
        update_flow_scale();
    }
}

ConfigureGraphicsFrameGen::~ConfigureGraphicsFrameGen() = default;

void ConfigureGraphicsFrameGen::SetConfiguration() {}

void ConfigureGraphicsFrameGen::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}
