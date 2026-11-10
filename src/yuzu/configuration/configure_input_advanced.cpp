// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QColorDialog>
#include "common/settings.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "qt_common/qt_compat.h"
#include "ui_configure_input_advanced.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_input_advanced.h"

ConfigureInputAdvanced::ConfigureInputAdvanced(Core::HID::HIDCore& hid_core_, QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureInputAdvanced>()), hid_core{hid_core_} {
    ui->setupUi(this);

    controllers_color_buttons = {{
        {
            ui->player1_left_body_button,
            ui->player1_left_buttons_button,
            ui->player1_right_body_button,
            ui->player1_right_buttons_button,
        },
        {
            ui->player2_left_body_button,
            ui->player2_left_buttons_button,
            ui->player2_right_body_button,
            ui->player2_right_buttons_button,
        },
        {
            ui->player3_left_body_button,
            ui->player3_left_buttons_button,
            ui->player3_right_body_button,
            ui->player3_right_buttons_button,
        },
        {
            ui->player4_left_body_button,
            ui->player4_left_buttons_button,
            ui->player4_right_body_button,
            ui->player4_right_buttons_button,
        },
        {
            ui->player5_left_body_button,
            ui->player5_left_buttons_button,
            ui->player5_right_body_button,
            ui->player5_right_buttons_button,
        },
        {
            ui->player6_left_body_button,
            ui->player6_left_buttons_button,
            ui->player6_right_body_button,
            ui->player6_right_buttons_button,
        },
        {
            ui->player7_left_body_button,
            ui->player7_left_buttons_button,
            ui->player7_right_body_button,
            ui->player7_right_buttons_button,
        },
        {
            ui->player8_left_body_button,
            ui->player8_left_buttons_button,
            ui->player8_right_body_button,
            ui->player8_right_buttons_button,
        },
    }};

    for (std::size_t player_idx = 0; player_idx < controllers_color_buttons.size(); ++player_idx) {
        auto& color_buttons = controllers_color_buttons[player_idx];
        for (std::size_t button_idx = 0; button_idx < color_buttons.size(); ++button_idx) {
            connect(color_buttons[button_idx], &QPushButton::clicked, this,
                    [this, player_idx, button_idx] {
                        OnControllerButtonClick(player_idx, button_idx);
                    });
        }
    }

    connect(ui->mouse_enabled, &QCheckBox::STATE_CHANGED, this,
            &ConfigureInputAdvanced::UpdateUIEnabled);
    connect(ui->debug_enabled, &QCheckBox::STATE_CHANGED, this,
            &ConfigureInputAdvanced::UpdateUIEnabled);
    connect(ui->touchscreen_enabled, &QCheckBox::STATE_CHANGED, this,
            &ConfigureInputAdvanced::UpdateUIEnabled);
    connect(ui->enable_ring_controller, &QCheckBox::STATE_CHANGED, this,
            &ConfigureInputAdvanced::UpdateUIEnabled);

    connect(ui->debug_configure, &QPushButton::clicked, this,
            [this] { CallDebugControllerDialog(); });
    connect(ui->touchscreen_advanced, &QPushButton::clicked, this,
            [this] { CallTouchscreenConfigDialog(); });
    connect(ui->buttonMotionTouch, &QPushButton::clicked, this,
            [this] { CallMotionTouchConfigDialog(); });
    connect(ui->ring_controller_configure, &QPushButton::clicked, this,
            [this] { CallRingControllerDialog(); });
    connect(ui->camera_configure, &QPushButton::clicked, this, [this] { CallCameraDialog(); });

#ifndef _WIN32
    ui->enable_raw_input->setVisible(false);
    ui->disable_wgi_xinput->setVisible(false);
#endif

    SetupToolTips();
    LoadConfiguration();
}

ConfigureInputAdvanced::~ConfigureInputAdvanced() = default;

void ConfigureInputAdvanced::OnControllerButtonClick(std::size_t player_idx,
                                                     std::size_t button_idx) {
    const QColor new_bg_color = QColorDialog::getColor(controllers_colors[player_idx][button_idx]);
    if (!new_bg_color.isValid()) {
        return;
    }
    controllers_colors[player_idx][button_idx] = new_bg_color;
    controllers_color_buttons[player_idx][button_idx]->setStyleSheet(
        QStringLiteral("background-color: %1; min-width: 60px;")
            .arg(controllers_colors[player_idx][button_idx].name()));
}

void ConfigureInputAdvanced::ApplyConfiguration() {
    for (std::size_t player_idx = 0; player_idx < controllers_color_buttons.size(); ++player_idx) {
        auto& player = Settings::values.players.GetValue()[player_idx];
        std::array<u32, 4> colors{};
        std::transform(controllers_colors[player_idx].begin(), controllers_colors[player_idx].end(),
                       colors.begin(), [](QColor color) { return color.rgb(); });

        player.body_color_left = colors[0];
        player.button_color_left = colors[1];
        player.body_color_right = colors[2];
        player.button_color_right = colors[3];

        hid_core.GetEmulatedControllerByIndex(player_idx)->ReloadColorsFromSettings();
    }

    Settings::values.debug_pad_enabled = ui->debug_enabled->isChecked();
    Settings::values.mouse_enabled = ui->mouse_enabled->isChecked();
    Settings::values.keyboard_enabled = ui->keyboard_enabled->isChecked();
    Settings::values.emulate_analog_keyboard = ui->emulate_analog_keyboard->isChecked();
    Settings::values.touchscreen.enabled = ui->touchscreen_enabled->isChecked();
    Settings::values.enable_raw_input = ui->enable_raw_input->isChecked();
    Settings::values.disable_wgi_xinput = ui->disable_wgi_xinput->isChecked();
    Settings::values.enable_udp_controller = ui->enable_udp_controller->isChecked();
    Settings::values.controller_navigation = ui->controller_navigation->isChecked();
    Settings::values.enable_ring_controller = ui->enable_ring_controller->isChecked();
    Settings::values.enable_ir_sensor = ui->enable_ir_sensor->isChecked();
    Settings::values.enable_joycon_driver = ui->enable_joycon_driver->isChecked();
    Settings::values.enable_procon_driver = ui->enable_procon_driver->isChecked();
    Settings::values.random_amiibo_id = ui->random_amiibo_id->isChecked();
}

void ConfigureInputAdvanced::LoadConfiguration() {
    for (std::size_t player_idx = 0; player_idx < controllers_color_buttons.size(); ++player_idx) {
        auto& player = Settings::values.players.GetValue()[player_idx];
        std::array<u32, 4> colors = {
            player.body_color_left,
            player.button_color_left,
            player.body_color_right,
            player.button_color_right,
        };

        std::transform(colors.begin(), colors.end(), controllers_colors[player_idx].begin(),
                       [](u32 rgb) { return QColor::fromRgb(rgb); });

        for (std::size_t button_idx = 0; button_idx < colors.size(); ++button_idx) {
            controllers_color_buttons[player_idx][button_idx]->setStyleSheet(
                QStringLiteral("background-color: %1; min-width: 60px;")
                    .arg(controllers_colors[player_idx][button_idx].name()));
        }
    }

    ui->debug_enabled->setChecked(Settings::values.debug_pad_enabled.GetValue());
    ui->mouse_enabled->setChecked(Settings::values.mouse_enabled.GetValue());
    ui->keyboard_enabled->setChecked(Settings::values.keyboard_enabled.GetValue());
    ui->emulate_analog_keyboard->setChecked(Settings::values.emulate_analog_keyboard.GetValue());
    ui->touchscreen_enabled->setChecked(Settings::values.touchscreen.enabled);
    ui->enable_raw_input->setChecked(Settings::values.enable_raw_input.GetValue());
    ui->disable_wgi_xinput->setChecked(Settings::values.disable_wgi_xinput.GetValue());
    ui->enable_udp_controller->setChecked(Settings::values.enable_udp_controller.GetValue());
    ui->controller_navigation->setChecked(Settings::values.controller_navigation.GetValue());
    ui->enable_ring_controller->setChecked(Settings::values.enable_ring_controller.GetValue());
    ui->enable_ir_sensor->setChecked(Settings::values.enable_ir_sensor.GetValue());
    ui->enable_joycon_driver->setChecked(Settings::values.enable_joycon_driver.GetValue());
    ui->enable_procon_driver->setChecked(Settings::values.enable_procon_driver.GetValue());
    ui->random_amiibo_id->setChecked(Settings::values.random_amiibo_id.GetValue());

    UpdateUIEnabled();
}

void ConfigureInputAdvanced::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureInputAdvanced::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();
}

void ConfigureInputAdvanced::SetupToolTips() {
    using ConfigurationShared::SetRichToolTip;

    SetRichToolTip(ui->joyconColorsGroup, ui->joyconColorsGroup->title(),
                   tr("Sets the Joy-Con body and button colors games see for each player; a Pro "
                      "Controller reports the left colors. Controllers on the direct Joy-Con or "
                      "Pro Controller driver report their real colors instead."),
                   tr("Purely cosmetic: it does not change how any control works."));
    SetRichToolTip(ui->keyboard_enabled, ui->keyboard_enabled->text(),
                   tr("Reports a USB keyboard to the game and passes your key presses to it, for "
                      "games and apps that accept a keyboard. Keys mapped on the Player tabs still "
                      "press their buttons too."),
                   tr("Not needed to play with the keyboard: key mappings on the Player tabs work "
                      "with this off."));
    SetRichToolTip(ui->mouse_enabled, ui->mouse_enabled->text(),
                   tr("Reports a USB mouse to the game and passes your PC mouse to it. While on, "
                      "left click no longer acts as a screen tap and mouse panning is blocked."),
                   tr("If clicks stopped working as screen taps, check that this is off."));
    SetRichToolTip(ui->touchscreen_enabled, ui->touchscreen_enabled->text(),
                   tr("Has no effect on games in this build: it only unlocks the Advanced button "
                      "next to it. Touch input stays on either way, and left click acts as a "
                      "finger unless Mouse is on."));
    SetRichToolTip(ui->touchscreen_advanced, ui->touchscreen_advanced->text(),
                   tr("Opens Configure Touchscreen, which sets the finger size (Touch Diameter X "
                      "and Y, 90 by default) and Rotational Angle that games receive with each "
                      "touch. Available only while Touchscreen is checked."),
                   tr("Wrong values can stop touch from working; Restore Defaults in that dialog "
                      "brings back 90, 90 and 0."));
    SetRichToolTip(ui->debug_enabled, ui->debug_enabled->text(),
                   tr("Reports a developer debug pad to the game, controlled by the mapping set in "
                      "Configure (a Pro Controller layout without Home or Capture)."),
                   tr("Leave it off unless homebrew or test software asks for the debug pad."));
    SetRichToolTip(ui->debug_configure, ui->debug_configure->text(),
                   tr("Opens Configure Debug Controller to map keys or a controller to the debug "
                      "pad. Available only while Debug Controller is checked."));
    SetRichToolTip(ui->enable_ring_controller, ui->enable_ring_controller->text(),
                   tr("Attaches an emulated Ring-Con for games that use it. Configure maps its "
                      "Pull and Push on Player 1, or reads a real Ring-Con through the direct "
                      "Joy-Con driver. On by default."),
                   tr("A real Ring-Con also needs the player setup described in Configure, done "
                      "before starting the game."));
    SetRichToolTip(ui->ring_controller_configure, ui->ring_controller_configure->text(),
                   tr("Opens Configure Ring Controller: map Pull and Push to keys or buttons, set "
                      "the ring's deadzone, or test a real Ring-Con on a Joy-Con with Enable, "
                      "which shows its sensor value. Available only while Ring Controller is "
                      "checked."),
                   tr("Enable for a real Ring-Con fails unless Enable direct JoyCon driver is "
                      "on."));
    SetRichToolTip(ui->enable_ir_sensor, ui->enable_ir_sensor->text(),
                   tr("Feeds a PC camera's image to games that use the right Joy-Con's infrared "
                      "camera. Current Nemyu releases are built without camera support, so this "
                      "box and its Configure button stay grayed out."));
    SetRichToolTip(ui->camera_configure, ui->camera_configure->text(),
                   tr("Opens Configure Infrared Camera to choose which PC camera stands in for the "
                      "IR camera, with a preview. Grayed out in builds without camera support, "
                      "which includes current releases."));
    SetRichToolTip(ui->emulate_analog_keyboard, ui->emulate_analog_keyboard->text(),
                   tr("Makes sticks mapped to keys turn gradually toward a new direction instead "
                      "of snapping to it, more like a real stick. How far the stick is pushed "
                      "still jumps straight to full or to the Modifier Range."),
                   tr("Try it if steering or camera turns feel jerky when playing on the "
                      "keyboard."));
    SetRichToolTip(ui->enable_raw_input, ui->enable_raw_input->text(),
                   tr("Has SDL read controllers through Windows raw input, for up to 8 Xbox-style "
                      "(XInput) players. Windows only; takes effect after restarting Nemyu."),
                   tr("Leave it off unless you connect more Xbox-style controllers than Nemyu "
                      "detects."));
    SetRichToolTip(ui->enable_udp_controller, ui->enable_udp_controller->text(),
                   tr("Lists Cemuhook UDP pads in Input Device so their buttons and sticks can be "
                      "mapped. Motion and touch from a UDP server work without it."),
                   tr("Leave it off unless a UDP source is the controller you play with; when on, "
                      "its buttons can get picked up while mapping another device."));
    SetRichToolTip(ui->controller_navigation, ui->controller_navigation->text(),
                   tr("Lets Player 1's controller move through the game list, Yes/No prompts and "
                      "the controller and profile pickers: D-pad or stick to move, A to confirm, B "
                      "to go back. On by default."),
                   tr("Turn it off if a connected controller keeps moving the selection in the "
                      "game list."));
    SetRichToolTip(ui->enable_joycon_driver, ui->enable_joycon_driver->text(),
                   tr("Has Nemyu talk to real Joy-Cons directly instead of through SDL, which adds "
                      "Ring-Con, NFC (amiibo) and IR camera support. On by default; takes effect "
                      "after restarting Nemyu."),
                   tr("Turn it off if your Joy-Cons misbehave, so SDL handles them instead."));
    SetRichToolTip(ui->enable_procon_driver, ui->enable_procon_driver->text(),
                   tr("Has Nemyu talk to a real Switch Pro Controller directly instead of through "
                      "SDL. Experimental and off by default; takes effect after restarting "
                      "Nemyu."),
                   tr("Turn it back off if your Pro Controller stops responding or acts up."));
    SetRichToolTip(ui->random_amiibo_id, ui->random_amiibo_id->text(),
                   tr("Gives the amiibo a new random ID each time the game reads it, so the game "
                      "treats every scan as a different figure."),
                   tr("Turn it on for games that let the same amiibo be used only once."));
    SetRichToolTip(ui->buttonMotionTouch, ui->buttonMotionTouch->text(),
                   tr("Opens the same Configure Motion / Touch dialog as Motion > Configure on the "
                      "Player tabs: Cemuhook UDP servers, touch-from-button profiles and UDP touch "
                      "calibration."));
    SetRichToolTip(ui->disable_wgi_xinput, ui->disable_wgi_xinput->text(),
                   tr("Stops SDL from using Windows.Gaming.Input and from pairing raw input with "
                      "XInput. Meant to stop SDL adding a Home press when Minus and Plus are held "
                      "together; may change trigger and rumble behavior. Windows only; needs a "
                      "restart."),
                   tr("Turn it on if pressing Minus and Plus together also fires Home hotkeys, "
                      "such as Home+Minus closing Nemyu."));
}

void ConfigureInputAdvanced::UpdateUIEnabled() {
    ui->debug_configure->setEnabled(ui->debug_enabled->isChecked());
    ui->touchscreen_advanced->setEnabled(ui->touchscreen_enabled->isChecked());
    ui->ring_controller_configure->setEnabled(ui->enable_ring_controller->isChecked());
#if !defined(YUZU_USE_QT_MULTIMEDIA)
    ui->enable_ir_sensor->setEnabled(false);
    ui->camera_configure->setEnabled(false);
#endif
}
