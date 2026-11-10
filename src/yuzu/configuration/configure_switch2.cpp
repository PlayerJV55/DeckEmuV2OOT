// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCheckBox>
#include <QCoreApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include "common/settings.h"
#include "core/core.h"
#include "video_core/game_dlss_status.h"
#include "yuzu/configuration/configure_switch2.h"
#include "yuzu/configuration/nvidia_dlss_panel.h"
#include "yuzu/configuration/settings_layout.h"
#include "yuzu/configuration/shared_widget.h"

namespace {

// The status line under "Use the game's own DLSS": what draws the game's DLSS in this session.
void ShowGameDlssStatus(QLabel* icon, QLabel* text) {
    const QString good = QStringLiteral("<span style=\"color:#2ea043\">&#10004;</span>");
    const QString waiting = QStringLiteral("<span style=\"color:#8b949e\">&#9679;</span>");
    const QString warning = QStringLiteral("<span style=\"color:#d29922\">&#9888;</span>");
    using VideoCore::GameDlssReason;
    using VideoCore::GameDlssState;
    const VideoCore::GameDlssStatus status = VideoCore::GetGameDlssStatus();
    const char* message =
        QT_TRANSLATE_NOOP("ConfigureGraphics", "A plain upscale stands in for the game's DLSS.");
    icon->setText(warning);
    if (status.state == GameDlssState::None) {
        icon->setText(waiting);
        message = QT_TRANSLATE_NOOP("ConfigureGraphics",
                                    "Shown here while a game that uses DLSS runs.");
    } else if (status.state == GameDlssState::Network) {
        icon->setText(good);
        message = QT_TRANSLATE_NOOP("ConfigureGraphics", "The game's own DLSS is running.");
    } else if (status.reason == GameDlssReason::NvidiaDlss) {
        icon->setText(good);
        message = QT_TRANSLATE_NOOP("ConfigureGraphics",
                                    "NVIDIA DLSS draws in place of the game's DLSS.");
    } else if (status.reason == GameDlssReason::TurnedOff) {
        message = QT_TRANSLATE_NOOP("ConfigureGraphics",
                                    "A plain upscale stands in for the game's DLSS (turned off).");
    } else if (status.reason == GameDlssReason::PassesCannotRun) {
        text->setText(QCoreApplication::translate(
            "ConfigureGraphics",
            "A plain upscale stands in: %n of the game's DLSS passes can't run here.", nullptr,
            static_cast<int>(status.rejected_passes)));
        return;
    }
    text->setText(QCoreApplication::translate("ConfigureGraphics", message));
}

// Puts the status line under the widget of "Use the game's own DLSS" and refreshes it every
// second while the dialog is open.
QWidget* WithGameDlssStatus(QWidget* parent, QWidget* widget) {
    QWidget* group = new QWidget(parent);
    QVBoxLayout* group_layout = new QVBoxLayout(group);
    group_layout->setContentsMargins(0, 0, 0, 0);
    group_layout->setSpacing(2);
    group_layout->addWidget(widget);
    QHBoxLayout* status_row = new QHBoxLayout;
    status_row->setContentsMargins(24, 0, 0, 6);
    status_row->setSpacing(8);
    QLabel* status_icon = new QLabel(group);
    status_icon->setTextFormat(Qt::RichText);
    status_icon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    status_icon->setFixedWidth(16);
    QLabel* status_text = new QLabel(group);
    status_text->setTextFormat(Qt::PlainText);
    status_text->setWordWrap(true);
    status_row->addWidget(status_icon);
    status_row->addWidget(status_text, 1);
    group_layout->addLayout(status_row);
    ShowGameDlssStatus(status_icon, status_text);
    QTimer* status_timer = new QTimer(group);
    QObject::connect(status_timer, &QTimer::timeout, group,
                     [status_icon, status_text] { ShowGameDlssStatus(status_icon, status_text); });
    status_timer->start(1000);
    return group;
}

#ifdef HAS_NVIDIA_STREAMLINE
// Puts the panel with NVIDIA's files under "Use NVIDIA DLSS", shown while it is ticked.
QWidget* WithNvidiaDlssPanel(QWidget* parent, ConfigurationShared::Widget* widget) {
    QWidget* group = new QWidget(parent);
    QVBoxLayout* group_layout = new QVBoxLayout(group);
    group_layout->setContentsMargins(0, 0, 0, 0);
    group_layout->setSpacing(2);
    group_layout->addWidget(widget);
    NvidiaDlssPanel* panel = new NvidiaDlssPanel(group);
    group_layout->addWidget(panel);
    if (widget->checkbox != nullptr) {
        panel->setVisible(widget->checkbox->isChecked());
        QObject::connect(widget->checkbox, &QCheckBox::toggled, panel, &QWidget::setVisible);
    }
    return group;
}
#endif

} // Anonymous namespace

ConfigureSwitch2::ConfigureSwitch2(const Core::System& system_,
                                   std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
                                   const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), system{system_} {
    setAccessibleName(tr("Switch 2"));

    auto* const layout = new QVBoxLayout(this);

    QWidget* mkw_widget{};
    ConfigurationShared::AddSettingGroups(
        *layout, ConfigurationShared::Layout::Switch2(),
        [&](Settings::BasicSetting* setting) -> QWidget* {
#ifndef HAS_NVIDIA_STREAMLINE
            // Only builds with NVIDIA Streamline (ENABLE_NVIDIA_DLSS, Windows) can use it.
            if (setting->Id() == Settings::values.nvidia_dlss.Id()) {
                return nullptr;
            }
#endif
            ConfigurationShared::Widget* const widget =
                ConfigurationShared::BuildSettingWidget(builder, setting, apply_funcs);
            if (widget == nullptr) {
                return nullptr;
            }
            if (setting->Id() == Settings::values.game_dlss_network.Id()) {
                return WithGameDlssStatus(this, widget);
            }
#ifdef HAS_NVIDIA_STREAMLINE
            if (setting->Id() == Settings::values.nvidia_dlss.Id()) {
                return WithNvidiaDlssPanel(this, widget);
            }
#endif
            if (setting->Id() == Settings::values.mkw_frame_rate.Id()) {
                mkw_widget = widget;
            }
            return widget;
        });
    if (mkw_widget != nullptr) {
        mkw_group = qobject_cast<QGroupBox*>(mkw_widget->parentWidget());
    }

    // Per game, the global values already include these
    if (Settings::IsConfiguringGlobal()) {
        auto* const automatic = new QGroupBox(tr("Automatic for Switch 2 Games"), this);
        auto* const automatic_layout = new QVBoxLayout(automatic);
        auto* const label = new QLabel(
            tr("Switch 2 games change some settings on their own:\n"
               "- Console memory: always 12 GB.\n"
               "- GPU accuracy: Fast.\n"
               "- Reactive flushing: off.\n"
               "- ASTC recompression: BC3 in place of Uncompressed on graphics cards with 12 GB "
               "of video memory or less.\n"
               "- Vertex input dynamic state: off on NVIDIA and Intel drivers.\n"
               "GPU accuracy and reactive flushing follow a game's own Properties when they are "
               "set there."),
            automatic);
        label->setWordWrap(true);
        automatic_layout->addWidget(label);
        layout->addWidget(automatic);
    }
    layout->addStretch();
}

ConfigureSwitch2::~ConfigureSwitch2() = default;

void ConfigureSwitch2::SetConfiguration() {}

void ConfigureSwitch2::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureSwitch2::HideMarioKartWorldFrameRate() {
    if (mkw_group != nullptr) {
        mkw_group->setVisible(false);
    }
}
