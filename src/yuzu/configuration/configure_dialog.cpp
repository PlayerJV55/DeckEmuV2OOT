// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <initializer_list>
#include <memory>
#include <QScreen>
#include <QScrollArea>
#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/vk.h"
#include "ui_configure.h"
#include "yuzu/configuration/configure_applets.h"
#include "yuzu/configuration/configure_audio.h"
#include "yuzu/configuration/configure_cpu.h"
#include "yuzu/configuration/configure_debug_tab.h"
#include "yuzu/configuration/configure_dialog.h"
#include "yuzu/configuration/configure_filesystem.h"
#include "yuzu/configuration/configure_general.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/configure_graphics_extensions.h"
#include "yuzu/configuration/configure_graphics_frame_gen.h"
#include "yuzu/configuration/configure_hotkeys.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_input_player.h"
#include "yuzu/configuration/configure_network.h"
#include "yuzu/configuration/configure_nx2_switches.h"
#include "yuzu/configuration/configure_profile_manager.h"
#include "yuzu/configuration/configure_switch2.h"
#include "yuzu/configuration/configure_system.h"
#include "yuzu/configuration/configure_ui.h"
#include "yuzu/configuration/configure_web.h"
#include "yuzu/configuration/settings_layout.h"
#include "yuzu/hotkeys.h"
#include "yuzu/util/icons.h"

ConfigureDialog::ConfigureDialog(QWidget* parent, HotkeyRegistry& registry_,
                                 InputCommon::InputSubsystem* input_subsystem,
                                 std::vector<VkDeviceInfo::Record>& vk_device_records,
                                 Core::System& system_, bool enable_web_config)
    : QDialog(parent), ui{std::make_unique<Ui::ConfigureDialog>()}, registry(registry_),
      system{system_},
      builder{std::make_unique<ConfigurationShared::Builder>(this, !system_.IsPoweredOn())},
      applets_tab{std::make_unique<ConfigureApplets>(system_, nullptr, *builder, this)},
      audio_tab{std::make_unique<ConfigureAudio>(system_, nullptr, *builder, this)},
      cpu_tab{std::make_unique<ConfigureCpu>(system_, nullptr, *builder, this)},
      debug_tab_tab{std::make_unique<ConfigureDebugTab>(system_, this)},
      filesystem_tab{std::make_unique<ConfigureFilesystem>(this)},
      general_tab{std::make_unique<ConfigureGeneral>(system_, nullptr, *builder, this)},
      graphics_advanced_tab{
          std::make_unique<ConfigureGraphicsAdvanced>(system_, nullptr, *builder, this)},
      graphics_extensions_tab{
          std::make_unique<ConfigureGraphicsExtensions>(system_, nullptr, *builder, this)},
      graphics_frame_gen_tab{
          std::make_unique<ConfigureGraphicsFrameGen>(system_, nullptr, *builder, this)},
      ui_tab{std::make_unique<ConfigureUi>(system_, this)},
      graphics_tab{std::make_unique<ConfigureGraphics>(
          system_, vk_device_records, [&]() { graphics_advanced_tab->ExposeComputeOption(); },
          [this](Settings::AspectRatio ratio, Settings::ResolutionSetup setup) {
              ui_tab->UpdateScreenshotInfo(ratio, setup);
          },
          nullptr, *builder, this)},
      hotkeys_tab{std::make_unique<ConfigureHotkeys>(system_.HIDCore(), this)},
      input_tab{std::make_unique<ConfigureInput>(system_, this)},
      network_tab{std::make_unique<ConfigureNetwork>(system_, this)},
      nx2_switches_tab{std::make_unique<ConfigureNx2Switches>(this)},
      profile_tab{std::make_unique<ConfigureProfileManager>(system_, this)},
      switch2_tab{std::make_unique<ConfigureSwitch2>(system_, nullptr, *builder, this)},
      system_tab{std::make_unique<ConfigureSystem>(system_, nullptr, *builder, this)},
      web_tab{std::make_unique<ConfigureWeb>(this)} {
    Settings::SetConfiguringGlobal(true);
    DEBUG_ASSERT(ConfigurationShared::LayoutCoversAllSettings());

    ui->setupUi(this);

    for (QWidget* const page : std::initializer_list<QWidget*>{
             applets_tab.get(), audio_tab.get(), cpu_tab.get(), debug_tab_tab.get(),
             filesystem_tab.get(), general_tab.get(), graphics_tab.get(),
             graphics_frame_gen_tab.get(), graphics_advanced_tab.get(),
             graphics_extensions_tab.get(), hotkeys_tab.get(), profile_tab.get(),
             network_tab.get(), system_tab.get(), ui_tab.get(), web_tab.get()}) {
        AddPage(page);
    }
    for (QWidget* const sub_tab : input_tab->GetSubTabs()) {
        AddPage(sub_tab);
    }
    AddSwitch2Tab(switch2_tab.get());
    AddSwitch2Tab(nx2_switches_tab.get());
    // Only its sub tabs are shown
    ui->tabWidget->addTab(input_tab.get(), tr("Controls"));

    web_tab->SetWebServiceConfigEnabled(enable_web_config);
    hotkeys_tab->Populate(registry);

    // The console mode is saved from Controls and also shown on System
    connect(input_tab.get(), &ConfigureInput::ConsoleModeChanged, system_tab.get(),
            &ConfigureSystem::SetConsoleMode);
    connect(system_tab.get(), &ConfigureSystem::ConsoleModeChanged, input_tab.get(),
            &ConfigureInput::SetConsoleMode);

    input_tab->Initialize(input_subsystem);

    general_tab->SetResetCallback([&] { this->close(); });

    ui->show_advanced_check->setChecked(UISettings::values.show_advanced_settings.GetValue());
    ConfigurationShared::SetRichToolTip(
        ui->show_advanced_check, ui->show_advanced_check->text(),
        tr("Shows the groups marked Advanced, the Fixes tab of Graphics and the Advanced Switches "
           "tab of Switch 2. Without it they stay hidden, except a group with a setting changed from "
           "its default, so no change is out of sight. Debug is always shown."));
    ShowAdvancedSettings(ui->show_advanced_check->isChecked());
    connect(ui->show_advanced_check, &QCheckBox::toggled, this, [this](bool show) {
        UISettings::values.show_advanced_settings = show;
        ShowAdvancedSettings(show);
        UpdateVisibleTabs();
    });

    SetConfiguration();
    PopulateSelectionList();

    connect(ui->tabWidget, &QTabWidget::currentChanged, this, [this](int index) {
        if (index != -1) {
            debug_tab_tab->SetCurrentIndex(0);
        }
    });
    connect(ui_tab.get(), &ConfigureUi::LanguageChanged, this, &ConfigureDialog::OnLanguageChanged);
    connect(filesystem_tab.get(), &ConfigureFilesystem::ExternalContentDirsChanged, this,
            &ConfigureDialog::ExternalContentDirsChanged);
    connect(ui->selectorList, &QListWidget::itemSelectionChanged, this,
            &ConfigureDialog::UpdateVisibleTabs);

    if (system.IsPoweredOn()) {
        QPushButton* apply_button = ui->buttonBox->addButton(QDialogButtonBox::Apply);
        connect(apply_button, &QAbstractButton::clicked, this,
                &ConfigureDialog::HandleApplyButtonClicked);
    }

    adjustSize();
    // Large enough for the Controls page and its nine tabs without scrolling sideways; the pages
    // still scroll on small screens
    const QSize screen_limit = screen()->availableGeometry().size() * 0.9;
    resize(QSize(1280, 860).expandedTo(size()).boundedTo(screen_limit));
    ui->selectorList->setCurrentRow(0);

    // Selects the leftmost button on the bottom bar (Cancel as of writing)
    ui->buttonBox->setFocus();
}

ConfigureDialog::~ConfigureDialog() = default;

void ConfigureDialog::SetConfiguration() {}

void ConfigureDialog::ApplyConfiguration() {
    general_tab->ApplyConfiguration();
    ui_tab->ApplyConfiguration();
    system_tab->ApplyConfiguration();
    profile_tab->ApplyConfiguration();
    filesystem_tab->ApplyConfiguration();
    input_tab->ApplyConfiguration();
    hotkeys_tab->ApplyConfiguration(registry);
    cpu_tab->ApplyConfiguration();
    graphics_tab->ApplyConfiguration();
    graphics_frame_gen_tab->ApplyConfiguration();
    graphics_advanced_tab->ApplyConfiguration();
    graphics_extensions_tab->ApplyConfiguration();
    switch2_tab->ApplyConfiguration();
    nx2_switches_tab->ApplyConfiguration();
    audio_tab->ApplyConfiguration();
    debug_tab_tab->ApplyConfiguration();
    web_tab->ApplyConfiguration();
    network_tab->ApplyConfiguration();
    applets_tab->ApplyConfiguration();
    system.ApplySettings();
    Settings::LogSettings();
}

void ConfigureDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QDialog::changeEvent(event);
}

void ConfigureDialog::RetranslateUI() {
    const int old_row = ui->selectorList->currentRow();
    const int old_index = ui->tabWidget->currentIndex();

    ui->retranslateUi(this);

    PopulateSelectionList();
    ui->selectorList->setCurrentRow(old_row);

    UpdateVisibleTabs();
    ui->tabWidget->setCurrentIndex(old_index);
}

void ConfigureDialog::HandleApplyButtonClicked() {
    UISettings::values.configuration_applied = true;
    ApplyConfiguration();
}

Q_DECLARE_METATYPE(QList<QWidget*>);

void ConfigureDialog::PopulateSelectionList() {
    struct Entry {
        Icons::SettingsSection section;
        QString name;
        QList<QWidget*> tabs;
    };
    using Section = Icons::SettingsSection;
    const std::array<Entry, 10> items{{
        {Section::General, tr("General"), {general_tab.get(), ui_tab.get()}},
        {Section::System,
         tr("System"),
         {system_tab.get(), profile_tab.get(), filesystem_tab.get(), applets_tab.get()}},
        {Section::Emulation, tr("Emulation"), {cpu_tab.get()}},
        {Section::Graphics,
         tr("Graphics"),
         {graphics_tab.get(), graphics_frame_gen_tab.get(), graphics_advanced_tab.get(),
          graphics_extensions_tab.get()}},
        {Section::Switch2, tr("Switch 2"), switch2_tabs},
        {Section::Audio, tr("Audio"), {audio_tab.get()}},
        {Section::Controls, tr("Controls"), input_tab->GetSubTabs()},
        {Section::Hotkeys, tr("Hotkeys"), {hotkeys_tab.get()}},
        {Section::Network, tr("Network"), {network_tab.get(), web_tab.get()}},
        {Section::Debug, tr("Debug"), {debug_tab_tab.get()}},
    }};

    [[maybe_unused]] const QSignalBlocker blocker(ui->selectorList);

    ui->selectorList->clear();
    for (const auto& entry : items) {
        auto* const item =
            new QListWidgetItem(Icons::ForSettingsSection(entry.section), entry.name);
        item->setData(Qt::UserRole, QVariant::fromValue(entry.tabs));

        ui->selectorList->addItem(item);
    }
}

void ConfigureDialog::AddPage(QWidget* page) {
    // Each page scrolls on its own. Every wrapper goes into the tab widget now, so the ones not
    // shown stay hidden there instead of being drawn over the dialog.
    QScrollArea* const wrapper = ConfigurationShared::WrapInScrollArea(page, this);
    page_wrappers.emplace(page, wrapper);
    ui->tabWidget->addTab(wrapper, page->accessibleName());
}

void ConfigureDialog::AddSwitch2Tab(QWidget* page) {
    AddPage(page);
    switch2_tabs.push_back(page);
}

void ConfigureDialog::OnLanguageChanged(const QString& locale) {
    emit LanguageChanged(locale);
    //  Reloading the game list is needed to force retranslation.
    UISettings::values.is_game_list_reload_pending = true;
    // first apply the configuration, and then restore the display. The theme only switches when
    // the dialog closes, so the rebuilt tooltips keep the colours of the one on screen.
    const std::string shown_theme = UISettings::values.theme;
    ApplyConfiguration();
    UISettings::values.theme = shown_theme;
    RetranslateUI();
    SetConfiguration();
}

void ConfigureDialog::UpdateVisibleTabs() {
    const auto items = ui->selectorList->selectedItems();
    if (items.isEmpty()) {
        return;
    }

    [[maybe_unused]] const QSignalBlocker blocker(ui->tabWidget);

    QWidget* const current = ui->tabWidget->currentWidget();
    ui->tabWidget->clear();

    const auto tabs = qvariant_cast<QList<QWidget*>>(items[0]->data(Qt::UserRole));

    for (auto* const tab : tabs) {
        if (ConfigurationShared::IsEmptyAdvancedPage(*tab)) {
            continue;
        }
        // The environment switches table is for advanced users only
        if (tab == nx2_switches_tab.get() && !ui->show_advanced_check->isChecked()) {
            continue;
        }
        LOG_DEBUG(Frontend, "{}", tab->accessibleName().toStdString());
        ui->tabWidget->addTab(page_wrappers.at(tab), tab->accessibleName());
    }
    // Stays on the tab it showed when only the advanced settings were toggled
    if (const int index = ui->tabWidget->indexOf(current); index != -1) {
        ui->tabWidget->setCurrentIndex(index);
    }
}

void ConfigureDialog::ShowAdvancedSettings(bool show) {
    for (const auto& entry : page_wrappers) {
        ConfigurationShared::ShowAdvancedGroups(*entry.second->widget(), show);
    }
}
