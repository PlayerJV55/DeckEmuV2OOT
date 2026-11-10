// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_ui.h"

#include <array>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QToolButton>
#include <QVariant>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "ui_configure_ui.h"

#include "qt_common/config/uisettings.h"
#include "qt_common/qt_compat.h"
#include "qt_common/util/game.h"
#include "yuzu/configuration/configuration_shared.h"

namespace {

constexpr std::array default_folder_icon_sizes{
    std::make_pair(0, QT_TRANSLATE_NOOP("ConfigureUI", "None")),
    std::make_pair(24, QT_TRANSLATE_NOOP("ConfigureUI", "Small (24x24)")),
    std::make_pair(48, QT_TRANSLATE_NOOP("ConfigureUI", "Standard (48x48)")),
    std::make_pair(72, QT_TRANSLATE_NOOP("ConfigureUI", "Large (72x72)")),
};

// clang-format off
constexpr std::array row_text_names{
    QT_TRANSLATE_NOOP("ConfigureUI", "Filename"),
    QT_TRANSLATE_NOOP("ConfigureUI", "Filetype"),
    QT_TRANSLATE_NOOP("ConfigureUI", "Title ID"),
    QT_TRANSLATE_NOOP("ConfigureUI", "Title Name"),
    QT_TRANSLATE_NOOP("ConfigureUI", "None"),
};
// clang-format on

QString GetTranslatedFolderIconSize(size_t index) {
    return QCoreApplication::translate("ConfigureUI", default_folder_icon_sizes[index].second);
}

QString GetTranslatedRowTextName(size_t index) {
    return QCoreApplication::translate("ConfigureUI", row_text_names[index]);
}
} // Anonymous namespace

static float GetUpFactor(Settings::ResolutionSetup res_setup) {
    Settings::ResolutionScalingInfo info{};
    Settings::TranslateResolutionInfo(res_setup, info);
    return info.up_factor;
}

static void PopulateResolutionComboBox(QComboBox* screenshot_height, QWidget* parent) {
    screenshot_height->clear();

    const auto& enumeration =
        Settings::EnumMetadata<Settings::ResolutionSetup>::Canonicalizations();
    std::set<u32> resolutions{};
    for (const auto& [name, value] : enumeration) {
        const float up_factor = GetUpFactor(value);
        u32 height_undocked = Layout::ScreenUndocked::Height * up_factor;
        u32 height_docked = Layout::ScreenDocked::Height * up_factor;
        resolutions.emplace(height_undocked);
        resolutions.emplace(height_docked);
    }

    screenshot_height->addItem(parent->tr("Auto", "Screenshot height option"));
    for (const auto res : resolutions) {
        screenshot_height->addItem(QString::fromStdString(std::to_string(res)));
    }
}

static u32 ScreenshotDimensionToInt(const QString& height) {
    return std::strtoul(height.toUtf8(), nullptr, 0);
}

ConfigureUi::ConfigureUi(Core::System& system_, QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigureUi>()},
      ratio{Settings::values.aspect_ratio.GetValue()},
      resolution_setting{Settings::values.resolution_setup.GetValue()}, system{system_} {
    ui->setupUi(this);

    InitializeLanguageComboBox();

    for (const auto& theme : UISettings::themes) {
        ui->theme_combobox->addItem(QString::fromUtf8(theme.first),
                                    QString::fromUtf8(theme.second));
    }

    InitializeIconSizeComboBox();
    InitializeRowComboBoxes();

    PopulateResolutionComboBox(ui->screenshot_height, this);

    SetConfiguration();

    // Force game list reload if any of the relevant settings are changed.
    connect(ui->show_add_ons, &QCheckBox::STATE_CHANGED, this, &ConfigureUi::RequestGameListUpdate);
    connect(ui->show_size, &QCheckBox::STATE_CHANGED, this, &ConfigureUi::RequestGameListUpdate);
    connect(ui->show_types, &QCheckBox::STATE_CHANGED, this, &ConfigureUi::RequestGameListUpdate);
    connect(ui->show_play_time, &QCheckBox::STATE_CHANGED, this,
            &ConfigureUi::RequestGameListUpdate);
    connect(ui->folder_icon_size_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConfigureUi::RequestGameListUpdate);
    connect(ui->row_1_text_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigureUi::RequestGameListUpdate);
    connect(ui->row_2_text_combobox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigureUi::RequestGameListUpdate);

    // Update text ComboBoxes after user interaction.
    connect(ui->row_1_text_combobox, QOverload<int>::of(&QComboBox::activated),
            [this] { ConfigureUi::UpdateSecondRowComboBox(); });
    connect(ui->row_2_text_combobox, QOverload<int>::of(&QComboBox::activated),
            [this] { ConfigureUi::UpdateFirstRowComboBox(); });

    // Set screenshot path to user specification.
    connect(ui->screenshot_path_button, &QToolButton::pressed, this, [this] {
        auto dir =
            QFileDialog::getExistingDirectory(this, tr("Select Screenshots Path..."),
                                              QString::fromStdString(Common::FS::GetEdenPathString(
                                                  Common::FS::EdenPath::ScreenshotsDir)));
        if (!dir.isEmpty()) {
            if (dir.back() != QChar::fromLatin1('/')) {
                dir.append(QChar::fromLatin1('/'));
            }

            ui->screenshot_path_edit->setText(dir);
        }
    });

    connect(ui->screenshot_height, &QComboBox::currentTextChanged, [this]() { UpdateWidthText(); });

    connect(ui->reset_game_list_cache, &QPushButton::pressed, this,
            [] { QtCommon::Game::ResetMetadata(); });

#ifndef USE_DISCORD_PRESENCE
    ui->discord_group->setVisible(false);
#endif

    UpdateWidthText();

    SetupToolTips();
}

ConfigureUi::~ConfigureUi() = default;

void ConfigureUi::ApplyConfiguration() {
    UISettings::values.theme =
        ui->theme_combobox->itemData(ui->theme_combobox->currentIndex()).toString().toStdString();
    UISettings::values.show_add_ons = ui->show_add_ons->isChecked();
    UISettings::values.show_size = ui->show_size->isChecked();
    UISettings::values.show_types = ui->show_types->isChecked();
    UISettings::values.show_play_time = ui->show_play_time->isChecked();
    UISettings::values.folder_icon_size = ui->folder_icon_size_combobox->currentData().toUInt();
    UISettings::values.row_1_text_id = ui->row_1_text_combobox->currentData().toUInt();
    UISettings::values.row_2_text_id = ui->row_2_text_combobox->currentData().toUInt();
    UISettings::values.cache_game_list = ui->cache_game_list->isChecked();

    UISettings::values.enable_discord_presence = ui->toggle_discordrpc->isChecked();
    const QString boxart_url = ui->edit_discord_boxart_url->text().trimmed();
    UISettings::values.discord_boxart_url = boxart_url.isEmpty()
                                                ? UISettings::values.discord_boxart_url.GetDefault()
                                                : boxart_url.toStdString();

    UISettings::values.enable_screenshot_save_as = ui->enable_screenshot_save_as->isChecked();
    Common::FS::SetEdenPath(Common::FS::EdenPath::ScreenshotsDir,
                            ui->screenshot_path_edit->text().toStdString());

    const u32 height = ScreenshotDimensionToInt(ui->screenshot_height->currentText());
    UISettings::values.screenshot_height.SetValue(height);

    RequestGameListUpdate();
    system.ApplySettings();
}

void ConfigureUi::RequestGameListUpdate() {
    UISettings::values.is_game_list_reload_pending.exchange(true);
}

void ConfigureUi::SetConfiguration() {
    ui->theme_combobox->setCurrentIndex(
        ui->theme_combobox->findData(QString::fromStdString(UISettings::values.theme)));
    ui->language_combobox->setCurrentIndex(ui->language_combobox->findData(
        QString::fromStdString(UISettings::values.language.GetValue())));
    ui->show_add_ons->setChecked(UISettings::values.show_add_ons.GetValue());
    ui->show_size->setChecked(UISettings::values.show_size.GetValue());
    ui->show_types->setChecked(UISettings::values.show_types.GetValue());
    ui->show_play_time->setChecked(UISettings::values.show_play_time.GetValue());
    ui->folder_icon_size_combobox->setCurrentIndex(
        ui->folder_icon_size_combobox->findData(UISettings::values.folder_icon_size.GetValue()));
    ui->cache_game_list->setChecked(UISettings::values.cache_game_list.GetValue());

    ui->toggle_discordrpc->setChecked(UISettings::values.enable_discord_presence.GetValue());
    ui->edit_discord_boxart_url->setText(
        QString::fromStdString(UISettings::values.discord_boxart_url.GetValue()));
    ui->edit_discord_boxart_url->setPlaceholderText(
        QString::fromStdString(UISettings::values.discord_boxart_url.GetDefault()));

    ui->enable_screenshot_save_as->setChecked(
        UISettings::values.enable_screenshot_save_as.GetValue());
    ui->screenshot_path_edit->setText(QString::fromStdString(
        Common::FS::GetEdenPathString(Common::FS::EdenPath::ScreenshotsDir)));

    const auto height = UISettings::values.screenshot_height.GetValue();
    if (height == 0) {
        ui->screenshot_height->setCurrentIndex(0);
    } else {
        ui->screenshot_height->setCurrentText(QStringLiteral("%1").arg(height));
    }
}

void ConfigureUi::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureUi::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();

    for (int i = 0; i < ui->folder_icon_size_combobox->count(); i++) {
        ui->folder_icon_size_combobox->setItemText(
            i, GetTranslatedFolderIconSize(static_cast<size_t>(i)));
    }

    for (int i = 0; i < ui->row_1_text_combobox->count(); i++) {
        const QString name = GetTranslatedRowTextName(static_cast<size_t>(i));

        ui->row_1_text_combobox->setItemText(i, name);
        ui->row_2_text_combobox->setItemText(i, name);
    }
}

void ConfigureUi::SetupToolTips() {
    using ConfigurationShared::SetRichToolTip;

    SetRichToolTip(ui->language_combobox, ui->language_label->text(),
                   tr("Sets the language of Nemyu's menus and dialogs. <System> uses your "
                      "operating system's language when Nemyu has a translation for it, "
                      "otherwise English. Picking one applies every change in this window right "
                      "away."),
                   tr("Games keep using the Language set on the System page."));
    SetRichToolTip(ui->theme_combobox, ui->theme_label->text(),
                   tr("Changes the look of Nemyu's windows. Dark and Midnight Blue are dark "
                      "styles, and the Colorful versions use colored icons. Applies when this "
                      "window closes after OK or Apply."),
                   tr("Dark Colorful is the default on Windows."));
    SetRichToolTip(ui->show_add_ons, ui->show_add_ons->text(),
                   tr("Shows the Add-ons column in the game list's tree view: each game's "
                      "update, DLC and mods with versions, with disabled ones marked [D]."));
    SetRichToolTip(ui->show_size, ui->show_size->text(),
                   tr("Shows the Size column in the game list's tree view with each game "
                      "file's size."));
    SetRichToolTip(ui->show_types, ui->show_types->text(),
                   tr("Shows the File type column in the game list's tree view, such as NSP or "
                      "XCI."));
    SetRichToolTip(ui->show_play_time, ui->show_play_time->text(),
                   tr("Shows the Play time column in the game list's tree view with how long "
                      "you have played each game."),
                   tr("Play time also shows when you hover over a game, even with this column "
                      "hidden."));
    SetRichToolTip(ui->folder_icon_size_combobox, ui->folder_icon_size_label->text(),
                   tr("Sets the icon size of folder rows in the tree view: game folders, "
                      "installed SD and NAND titles, Favorites and the add-folder row. None "
                      "currently shrinks them to 8x8 instead of hiding them."),
                   tr("Game icon size is set separately in View > Game Icon Size."));
    SetRichToolTip(ui->row_1_text_combobox, ui->row_1_label->text(),
                   tr("Picks the main text for each game: title name, filename, file type or "
                      "title ID. The tree view sorts by it, and grid and carousel views show "
                      "only this line."),
                   tr("Pick Filename to tell apart several copies of the same game that share "
                      "one title."));
    SetRichToolTip(ui->row_2_text_combobox, ui->row_2_label->text(),
                   tr("Picks the indented second line under each game in the tree view, or "
                      "None. It cannot match Row 1, and grid and carousel views never show it."));
    SetRichToolTip(ui->cache_game_list, ui->cache_game_list->text(),
                   tr("Stores each game's Add-ons column, plus the name and icon of installed "
                      "titles, on disk so the game list loads faster. Stored entries are reused "
                      "until the cache is reset, even after a game changes."),
                   tr("Keep it on; if an entry looks outdated, reset the cache instead of turning "
                      "this off."));
    SetRichToolTip(ui->reset_game_list_cache, ui->reset_game_list_cache->text(),
                   tr("Deletes the stored game list data right away; the list rebuilds it on its "
                      "next reload."),
                   tr("The refresh button in the status bar clears the same cache and reloads the "
                      "list at once."));
    SetRichToolTip(ui->toggle_discordrpc, ui->toggle_discordrpc->text(),
                   tr("Shows the game you are playing in your Discord status, with its cover, "
                      "title and elapsed time, and that you are not in a game otherwise. Needs the "
                      "Discord desktop app running on this PC."),
                   tr("On by default; turn it off if you don't want Discord friends to see what "
                      "you play."));
    SetRichToolTip(ui->edit_discord_boxart_url, ui->label_discord_boxart_url->text(),
                   tr("Sets where Nemyu downloads game covers first, as <URL><game-name>.png, "
                      "before Eden's cover repository; leave empty for Nemyu's default. Covers "
                      "appear in Discord, on the loading screen and for games without an icon, "
                      "even with Discord status off."),
                   tr("Covers already saved in the boxart folder of Nemyu's data folder are never "
                      "refetched; delete one to refetch it, or replace it with your own."));
    SetRichToolTip(ui->enable_screenshot_save_as, ui->enable_screenshot_save_as->text(),
                   tr("When on, Capture Screenshot pauses the game and opens a Save dialog; "
                      "when off, screenshots go straight to the Screenshots Path. Windows only."),
                   tr("Turn off to take screenshots without interrupting play."));
    SetRichToolTip(ui->screenshot_path_edit, ui->label->text(),
                   tr("Folder where Capture Screenshot (Ctrl+P by default) saves PNG files "
                      "named by title ID and time. Captures a game saves on its own go here "
                      "too."),
                   tr("The in-game album reads this folder and only lists files that keep "
                      "Nemyu's naming."));
    // The button only reads "...", so it borrows its row's label as the title
    SetRichToolTip(ui->screenshot_path_button, ui->label->text(),
                   tr("Opens a folder picker to choose the Screenshots Path."));
    SetRichToolTip(ui->screenshot_height, ui->label_3->text(),
                   tr("Sets the height of Capture Screenshot images; the width follows the "
                      "aspect ratio in Graphics. Auto uses 720 in handheld or 1080 docked, "
                      "times the resolution scale. You can also type a height."),
                   tr("Captures a game saves on its own are always 1280x720, whatever you set "
                      "here."));
}

void ConfigureUi::InitializeLanguageComboBox() {
    ui->language_combobox->addItem(tr("<System>"), QString{});
    ui->language_combobox->addItem(tr("English"), QStringLiteral("en"));
    QDirIterator it(QStringLiteral(":/languages"), QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        QString locale = it.next();
        locale.truncate(locale.lastIndexOf(QLatin1Char{'.'}));
        locale.remove(0, locale.lastIndexOf(QLatin1Char{'/'}) + 1);
        const QString lang = QLocale::languageToString(QLocale(locale).language());
        const QString country = QLocale::territoryToString(QLocale(locale).territory());
        ui->language_combobox->addItem(QStringLiteral("%1 (%2)").arg(lang, country), locale);
    }

    // Unlike other configuration changes, interface language changes need to be reflected on the
    // interface immediately. This is done by passing a signal to the main window, and then
    // retranslating when passing back.
    connect(ui->language_combobox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ConfigureUi::OnLanguageChanged);
}

void ConfigureUi::InitializeIconSizeComboBox() {
    for (size_t i = 0; i < default_folder_icon_sizes.size(); i++) {
        const auto size = default_folder_icon_sizes[i].first;
        ui->folder_icon_size_combobox->addItem(GetTranslatedFolderIconSize(i), size);
    }
}

void ConfigureUi::InitializeRowComboBoxes() {
    UpdateFirstRowComboBox(true);
    UpdateSecondRowComboBox(true);
}

void ConfigureUi::UpdateFirstRowComboBox(bool init) {
    const int currentIndex =
        init ? UISettings::values.row_1_text_id.GetValue()
             : ui->row_1_text_combobox->findData(ui->row_1_text_combobox->currentData());

    ui->row_1_text_combobox->clear();

    for (std::size_t i = 0; i < row_text_names.size(); i++) {
        const QString row_text_name = GetTranslatedRowTextName(i);
        ui->row_1_text_combobox->addItem(row_text_name, QVariant::fromValue(i));
    }

    ui->row_1_text_combobox->setCurrentIndex(ui->row_1_text_combobox->findData(currentIndex));

    ui->row_1_text_combobox->removeItem(4); // None
    ui->row_1_text_combobox->removeItem(
        ui->row_1_text_combobox->findData(ui->row_2_text_combobox->currentData()));
}

void ConfigureUi::UpdateSecondRowComboBox(bool init) {
    const int currentIndex =
        init ? UISettings::values.row_2_text_id.GetValue()
             : ui->row_2_text_combobox->findData(ui->row_2_text_combobox->currentData());

    ui->row_2_text_combobox->clear();

    for (std::size_t i = 0; i < row_text_names.size(); ++i) {
        const QString row_text_name = GetTranslatedRowTextName(i);
        ui->row_2_text_combobox->addItem(row_text_name, QVariant::fromValue(i));
    }

    ui->row_2_text_combobox->setCurrentIndex(ui->row_2_text_combobox->findData(currentIndex));

    ui->row_2_text_combobox->removeItem(
        ui->row_2_text_combobox->findData(ui->row_1_text_combobox->currentData()));
}

void ConfigureUi::OnLanguageChanged(int index) {
    if (index == -1)
        return;

    emit LanguageChanged(ui->language_combobox->itemData(index).toString());
}

void ConfigureUi::UpdateWidthText() {
    const u32 height = ScreenshotDimensionToInt(ui->screenshot_height->currentText());
    const u32 width = UISettings::CalculateWidth(height, ratio);
    if (height == 0) {
        const auto up_factor = GetUpFactor(resolution_setting);
        const u32 height_docked = Layout::ScreenDocked::Height * up_factor;
        const u32 width_docked = UISettings::CalculateWidth(height_docked, ratio);
        const u32 height_undocked = Layout::ScreenUndocked::Height * up_factor;
        const u32 width_undocked = UISettings::CalculateWidth(height_undocked, ratio);
        ui->screenshot_width->setText(tr("Auto (%1 x %2, %3 x %4)", "Screenshot width value")
                                          .arg(width_undocked)
                                          .arg(height_undocked)
                                          .arg(width_docked)
                                          .arg(height_docked));
    } else {
        ui->screenshot_width->setText(QStringLiteral("%1 x").arg(width));
    }
}

void ConfigureUi::UpdateScreenshotInfo(Settings::AspectRatio ratio_,
                                       Settings::ResolutionSetup resolution_setting_) {
    ratio = ratio_;
    resolution_setting = resolution_setting_;
    UpdateWidthText();
}
