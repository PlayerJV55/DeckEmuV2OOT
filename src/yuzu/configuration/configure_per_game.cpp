// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QString>
#include <QTimer>

#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "common/nx2_profiles.h"
#include "common/settings_enums.h"
#include "common/settings_input.h"
#include "configuration/shared_widget.h"
#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/xts_archive.h"
#include "core/loader/loader.h"
#include "core/loader/nso.h"
#include "frontend_common/config.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/boxart.h"
#include "qt_common/util/vk.h"
#include "ui_configure_per_game.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_applets.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/configure_graphics_frame_gen.h"
#include "yuzu/configuration/configure_input_per_game.h"
#include "yuzu/configuration/configure_network.h"
#include "yuzu/configuration/configure_per_game.h"
#include "yuzu/configuration/configure_per_game_addons.h"
#include "yuzu/configuration/configure_per_game_header.h"
#include "yuzu/configuration/configure_switch2.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/configuration/settings_layout.h"
#include "yuzu/util/icons.h"
#include "yuzu/util/util.h"

ConfigurePerGame::ConfigurePerGame(QWidget* parent, u64 title_id_, const std::string& file_name,
                                   std::vector<VkDeviceInfo::Record>& vk_device_records,
                                   Core::System& system_)
    : QDialog(parent), ui(std::make_unique<Ui::ConfigurePerGame>()),
      game_path{QString::fromStdString(file_name)}, title_id{title_id_}, system{system_},
      builder{std::make_unique<ConfigurationShared::Builder>(this, !system_.IsPoweredOn())},
      tab_group{std::make_shared<std::vector<ConfigurationShared::Tab*>>()} {
    const auto file_path = std::filesystem::path(Common::FS::ToU8String(file_name));
    const auto config_file_name = title_id == 0 ? Common::FS::PathToUTF8String(file_path.filename())
                                                : fmt::format("{:016X}", title_id);
    game_config = std::make_unique<QtConfig>(config_file_name, Config::ConfigType::PerGameConfig);
    addons_tab = std::make_unique<ConfigurePerGameAddons>(system_, this);
    audio_tab = std::make_unique<ConfigureAudio>(system_, tab_group, *builder, this);
    cpu_tab = std::make_unique<ConfigureCpu>(system_, tab_group, *builder, this);
    graphics_advanced_tab =
        std::make_unique<ConfigureGraphicsAdvanced>(system_, tab_group, *builder, this);
    graphics_extensions_tab =
        std::make_unique<ConfigureGraphicsExtensions>(system_, tab_group, *builder, this);
    graphics_frame_gen_tab =
        std::make_unique<ConfigureGraphicsFrameGen>(system_, tab_group, *builder, this);
    graphics_tab = std::make_unique<ConfigureGraphics>(
        system_, vk_device_records, [&]() { graphics_advanced_tab->ExposeComputeOption(); },
        [](Settings::AspectRatio, Settings::ResolutionSetup) {}, tab_group, *builder, this);
    switch2_tab = std::make_unique<ConfigureSwitch2>(system_, tab_group, *builder, this);
    if (title_id != Loader::MarioKartWorldProgramId) {
        switch2_tab->HideMarioKartWorldFrameRate();
    }
    input_tab = std::make_unique<ConfigureInputPerGame>(system_, game_config.get(), this);
    system_tab = std::make_unique<ConfigureSystem>(system_, tab_group, *builder, this);
    network_tab = std::make_unique<ConfigureNetwork>(system_, this);
    applets_tab = std::make_unique<ConfigureApplets>(system_, tab_group, *builder, this);

    ui->setupUi(this);
    addons_tab->setAccessibleName(tr("Add-Ons"));

    header = new ConfigurePerGameHeader(this);
    ui->headerLayout->addWidget(header);

    if (title_id != 0 && !Common::GetNx2ProfileDirectory().empty()) {
        switch_profile_label = new QLabel(this);
        switch_profile_combo = new QComboBox(this);
        switch_profile_combo->addItem(QString{}, QString{});
        for (const std::string& name : Common::ListNx2Profiles()) {
            switch_profile_combo->addItem(QString::fromStdString(name),
                                          QString::fromStdString(name));
        }
        const QString assigned = QString::fromStdString(Common::GetGameNx2Profile(title_id));
        int index = assigned.isEmpty() ? 0 : switch_profile_combo->findData(assigned);
        if (index < 0) {
            // Its file is gone: keep the assignment visible rather than drop it silently
            switch_profile_combo->addItem(tr("%1 (missing)").arg(assigned), assigned);
            index = switch_profile_combo->count() - 1;
        }
        switch_profile_combo->setCurrentIndex(index);
        RetranslateSwitchProfile();
        auto* const profile_layout = new QHBoxLayout;
        profile_layout->addWidget(switch_profile_label);
        profile_layout->addWidget(switch_profile_combo);
        profile_layout->addStretch();
        ui->headerLayout->addLayout(profile_layout);
    }

    reset_button = new QPushButton(this);
    ui->tabWidget->setCornerWidget(reset_button, Qt::TopRightCorner);
    connect(reset_button, &QPushButton::clicked, this, &ConfigurePerGame::ResetCurrentTab);
    connect(ui->tabWidget, &QTabWidget::currentChanged, this,
            &ConfigurePerGame::UpdateResetButton);

    // Each page scrolls on its own, as in Configure. Every wrapper goes into the tab widget now,
    // so the ones not shown stay hidden there instead of being drawn over the dialog.
    for (QWidget* const page : std::initializer_list<QWidget*>{
             addons_tab.get(), system_tab.get(), applets_tab.get(), cpu_tab.get(),
             graphics_tab.get(), graphics_frame_gen_tab.get(), graphics_advanced_tab.get(),
             graphics_extensions_tab.get(), switch2_tab.get(), audio_tab.get(), input_tab.get(),
             network_tab.get()}) {
        QScrollArea* const wrapper = ConfigurationShared::WrapInScrollArea(page, this);
        page_wrappers.emplace(page, wrapper);
        ui->tabWidget->addTab(wrapper, page->accessibleName());

        // A restore button shows or hides whenever a setting starts or stops following the
        // global value: keep the marks of the left list in step
        for (QPushButton* const button : page->findChildren<QPushButton*>()) {
            if (button->objectName().startsWith(QStringLiteral("RestoreButton"))) {
                button->installEventFilter(this);
            }
        }
    }

    ui->show_advanced_check->setChecked(UISettings::values.show_advanced_settings.GetValue());
    ConfigurationShared::SetRichToolTip(
        ui->show_advanced_check, ui->show_advanced_check->text(),
        tr("Shows the groups marked Advanced and the Fixes tab of Graphics. Without it they stay "
           "hidden, except a group with a setting this game changes, so no change is out of "
           "sight."));
    ShowAdvancedSettings(ui->show_advanced_check->isChecked());
    connect(ui->show_advanced_check, &QCheckBox::toggled, this, [this](bool show) {
        UISettings::values.show_advanced_settings = show;
        ShowAdvancedSettings(show);
        UpdateVisibleTabs();
    });

    PopulateSelectionList();
    connect(ui->selectorList, &QListWidget::itemSelectionChanged, this,
            &ConfigurePerGame::UpdateVisibleTabs);
    ui->selectorList->setCurrentRow(0);

    setFocusPolicy(Qt::ClickFocus);
    setWindowTitle(tr("Properties"));

    addons_tab->SetTitleId(title_id);

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigurePerGame::HandleApplyButtonClicked);
    }

    LoadConfiguration();
}

ConfigurePerGame::~ConfigurePerGame() = default;

void ConfigurePerGame::ApplyConfiguration() {
    for (const auto tab : *tab_group) {
        tab->ApplyConfiguration();
    }
    addons_tab->ApplyConfiguration();
    input_tab->ApplyConfiguration();
    network_tab->ApplyConfiguration();

    // A handheld Player 1 forces this game into handheld; with use_global still set, SetValue
    // would change the global console mode instead
    if (Settings::IsDockedMode() && Settings::values.players.GetValue()[0].controller_type ==
                                        Settings::ControllerType::Handheld) {
        Settings::values.use_docked_mode.SetGlobal(false);
        Settings::values.use_docked_mode.SetValue(Settings::ConsoleMode::Handheld);
    }

    system.ApplySettings();
    Settings::LogSettings();

    game_config->SaveAllValues();

    if (switch_profile_combo != nullptr) {
        const std::string selected = switch_profile_combo->currentData().toString().toStdString();
        if (selected != Common::GetGameNx2Profile(title_id) &&
            !Common::SetGameNx2Profile(title_id, selected)) {
            LOG_ERROR(Frontend, "Could not save the switch profile of {:016X}", title_id);
        }
    }
}

void ConfigurePerGame::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

bool ConfigurePerGame::eventFilter(QObject* object, QEvent* event) {
    if ((event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent) &&
        !marker_update_pending) {
        // Once for all the buttons a reset hides
        marker_update_pending = true;
        QTimer::singleShot(0, this, [this] {
            marker_update_pending = false;
            UpdateMarkers();
        });
    }
    return QDialog::eventFilter(object, event);
}

void ConfigurePerGame::RetranslateUI() {
    const int old_row = ui->selectorList->currentRow();
    ui->retranslateUi(this);
    PopulateSelectionList();
    ui->selectorList->setCurrentRow(old_row);
    RetranslateSwitchProfile();
}

void ConfigurePerGame::RetranslateSwitchProfile() {
    if (switch_profile_combo == nullptr) {
        return;
    }
    switch_profile_label->setText(tr("Switch profile:"));
    switch_profile_combo->setItemText(0, tr("None (global switches)"));
    const QString tool_tip =
        tr("The Advanced Switches profile this game uses: its ticked switches replace the global "
           "ones while the game runs. Profiles are made in Configure > Switch 2 > Advanced "
           "Switches. A change applies the next time the game starts.");
    switch_profile_label->setToolTip(tool_tip);
    switch_profile_combo->setToolTip(tool_tip);
}

void ConfigurePerGame::PopulateSelectionList() {
    // The order of Configure; General and Debug have no settings a game can change
    sections = {
        {addons_tab.get()},
        {system_tab.get(), applets_tab.get()},
        {cpu_tab.get()},
        {graphics_tab.get(), graphics_frame_gen_tab.get(), graphics_advanced_tab.get(),
         graphics_extensions_tab.get()},
        {switch2_tab.get()},
        {audio_tab.get()},
        {input_tab.get()},
        {network_tab.get()},
    };
    section_names = {
        tr("Add-Ons"),  tr("System"), tr("Emulation"), tr("Graphics"),
        tr("Switch 2"), tr("Audio"),  tr("Controls"),  tr("Network"),
    };

    reset_button->setText(tr("Reset to Global"));
    reset_button->setToolTip(
        tr("Sets every setting this game changes on this tab back to the global value."));

    [[maybe_unused]] const QSignalBlocker blocker(ui->selectorList);
    ui->selectorList->clear();
    using Section = Icons::SettingsSection;
    static constexpr std::array section_icons{
        Section::AddOns, Section::System, Section::Emulation, Section::Graphics,
        Section::Switch2, Section::Audio,  Section::Controls,  Section::Network,
    };
    for (std::size_t i = 0; i < section_names.size(); ++i) {
        ui->selectorList->addItem(
            new QListWidgetItem(Icons::ForSettingsSection(section_icons[i]), section_names[i]));
    }
    UpdateMarkers();
}

void ConfigurePerGame::UpdateMarkers() {
    for (std::size_t i = 0; i < sections.size(); ++i) {
        // Marks the entries where this game changes something
        const bool changed = std::ranges::any_of(sections[i], [](const QWidget* page) {
            return !ConfigurationShared::OverrideRestoreButtons(*page).empty();
        });
        ui->selectorList->item(static_cast<int>(i))
            ->setText(changed ? section_names[i] + QStringLiteral(" \u2022") : section_names[i]);
    }
    UpdateResetButton();
}

void ConfigurePerGame::UpdateResetButton() {
    const auto* const wrapper = qobject_cast<const QScrollArea*>(ui->tabWidget->currentWidget());
    const QWidget* const page = wrapper != nullptr ? wrapper->widget() : nullptr;
    if (page == nullptr) {
        reset_button->hide();
        return;
    }
    // Only tabs with settings that have a global value get the button
    const auto has_restore_button = [](const QPushButton* button) {
        return button->objectName().startsWith(QStringLiteral("RestoreButton"));
    };
    reset_button->setVisible(
        std::ranges::any_of(page->findChildren<QPushButton*>(), has_restore_button));
    reset_button->setEnabled(!ConfigurationShared::OverrideRestoreButtons(*page).empty());
}

void ConfigurePerGame::ResetCurrentTab() {
    const auto* const wrapper = qobject_cast<const QScrollArea*>(ui->tabWidget->currentWidget());
    if (wrapper == nullptr || wrapper->widget() == nullptr) {
        return;
    }
    const auto buttons = ConfigurationShared::OverrideRestoreButtons(*wrapper->widget());
    for (QPushButton* const button : buttons) {
        // Rows locked while the game runs stay as they are
        if (button->isEnabled()) {
            button->click();
        }
    }
    UpdateMarkers();
}

void ConfigurePerGame::UpdateVisibleTabs() {
    const int row = ui->selectorList->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= sections.size()) {
        return;
    }

    [[maybe_unused]] const QSignalBlocker blocker(ui->tabWidget);

    QWidget* const current = ui->tabWidget->currentWidget();
    ui->tabWidget->clear();
    for (QWidget* const page : sections[row]) {
        if (!ConfigurationShared::IsEmptyAdvancedPage(*page)) {
            ui->tabWidget->addTab(page_wrappers.at(page), page->accessibleName());
        }
    }
    // Stays on the tab it showed when only the advanced settings were toggled
    if (const int index = ui->tabWidget->indexOf(current); index != -1) {
        ui->tabWidget->setCurrentIndex(index);
    }
    UpdateResetButton();
}

void ConfigurePerGame::ShowAdvancedSettings(bool show) {
    for (const auto& entry : page_wrappers) {
        ConfigurationShared::ShowAdvancedGroups(*entry.second->widget(), show);
    }
}

void ConfigurePerGame::HandleApplyButtonClicked() {
    UISettings::values.configuration_applied = true;
    ApplyConfiguration();
}

void ConfigurePerGame::LoadFromFile(FileSys::VirtualFile file_) {
    file = std::move(file_);
    LoadConfiguration();
}

void ConfigurePerGame::LoadConfiguration() {
    if (file == nullptr) {
        return;
    }

    addons_tab->LoadFromFile(file);

    ConfigurePerGameHeader::Info info;
    info.title_id = QStringLiteral("%1").arg(title_id, 16, 16, QLatin1Char{'0'}).toUpper();

    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto control = pm.GetControlMetadata();
    const auto loader = Loader::GetLoader(system, file);

    if (control.first != nullptr) {
        info.version = QString::fromStdString(control.first->GetVersionString());
        info.name = QString::fromStdString(control.first->GetApplicationName());
        info.developer = QString::fromStdString(control.first->GetDeveloperName());
    } else {
        std::string title;
        if (loader->ReadTitle(title) == Loader::ResultStatus::Success)
            info.name = QString::fromStdString(title);

        FileSys::NACP nacp;
        if (loader->ReadControlData(nacp) == Loader::ResultStatus::Success)
            info.developer = QString::fromStdString(nacp.GetDeveloperName());

        info.version = QStringLiteral("1.0.0");
    }

    if (control.second != nullptr) {
        const auto bytes = control.second->ReadAllBytes();
        info.cover.loadFromData(bytes.data(), static_cast<u32>(bytes.size()));
    } else {
        std::vector<u8> bytes;
        if (loader->ReadIcon(bytes) == Loader::ResultStatus::Success) {
            info.cover.loadFromData(bytes.data(), static_cast<u32>(bytes.size()));
        }
    }
    // Dumps without control data, such as loose Switch 2 ones, have no icon: show the box art the
    // game list shows for them.
    if (info.cover.isNull()) {
        QImage cover = QtCommon::Boxart::LoadCachedCover(title_id, info.name.toStdString());
        if (cover.isNull()) {
            cover.load(QStringLiteral(":/img/placeholder.png"));
        }
        info.cover = QPixmap::fromImage(cover);
    }

    if (info.name.isEmpty()) {
        info.name = QString::fromStdString(file->GetName());
    }

    info.format = QString::fromStdString(Loader::GetFileTypeString(loader->GetFileType()));
    info.size = QtCommon::ReadableByteSize(file->GetSize());
    info.path = game_path;

    header->SetInfo(info);
}
