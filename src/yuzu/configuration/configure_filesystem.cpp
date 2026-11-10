// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <string>
#include <utility>
#include <vector>
#include <QDir>
#include <QFileDialog>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include "common/fs/fs.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_compat.h"
#include "ui_configure_filesystem.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_filesystem.h"
#include "yuzu/configuration/settings_layout.h"

ConfigureFilesystem::ConfigureFilesystem(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureFilesystem>()) {
    ui->setupUi(this);
    SetConfiguration();

    connect(ui->nand_directory_button, &QToolButton::pressed, this,
            [this] { SetDirectory(DirectoryTarget::NAND, ui->nand_directory_edit); });
    connect(ui->sdmc_directory_button, &QToolButton::pressed, this,
            [this] { SetDirectory(DirectoryTarget::SD, ui->sdmc_directory_edit); });
    connect(ui->save_directory_button, &QToolButton::pressed, this, [this] { SetSaveDirectory(); });
    connect(ui->gamecard_path_button, &QToolButton::pressed, this,
            [this] { SetDirectory(DirectoryTarget::Gamecard, ui->gamecard_path_edit); });
    connect(ui->dump_path_button, &QToolButton::pressed, this,
            [this] { SetDirectory(DirectoryTarget::Dump, ui->dump_path_edit); });
    connect(ui->load_path_button, &QToolButton::pressed, this,
            [this] { SetDirectory(DirectoryTarget::Load, ui->load_path_edit); });

    connect(ui->gamecard_inserted, &QCheckBox::STATE_CHANGED, this,
            &ConfigureFilesystem::UpdateEnabledControls);
    connect(ui->gamecard_current_game, &QCheckBox::STATE_CHANGED, this,
            &ConfigureFilesystem::UpdateEnabledControls);

    connect(ui->add_external_dir_button, &QPushButton::pressed, this,
            &ConfigureFilesystem::AddExternalContentDirectory);
    connect(ui->remove_external_dir_button, &QPushButton::pressed, this,
            &ConfigureFilesystem::RemoveSelectedExternalContentDirectory);
    connect(ui->external_content_list, &QListWidget::itemSelectionChanged, this, [this] {
        ui->remove_external_dir_button->setEnabled(
            !ui->external_content_list->selectedItems().isEmpty());
    });

    namespace fs = std::filesystem;
    const auto same_folder = [](const fs::path& a, const fs::path& b) {
        return (a / "").lexically_normal() == (b / "").lexically_normal();
    };
    const bool custom_dump_root = !same_folder(
        Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir),
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / DUMP_DIR);
    ConfigurationShared::MarkAdvancedGroup(*ui->dumps_group,
                                           custom_dump_root ||
                                               Settings::values.dump_exefs.GetValue() ||
                                               Settings::values.dump_nso.GetValue());

    SetupToolTips();
}

ConfigureFilesystem::~ConfigureFilesystem() = default;

void ConfigureFilesystem::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureFilesystem::SetConfiguration() {
    ui->nand_directory_edit->setText(
        QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::NANDDir)));
    ui->sdmc_directory_edit->setText(
        QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::SDMCDir)));
    ui->save_directory_edit->setText(
        QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::SaveDir)));
    ui->gamecard_path_edit->setText(
        QString::fromStdString(Settings::values.gamecard_path.GetValue()));
    ui->dump_path_edit->setText(
        QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::DumpDir)));
    ui->load_path_edit->setText(
        QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::LoadDir)));

    ui->gamecard_inserted->setChecked(Settings::values.gamecard_inserted.GetValue());
    ui->gamecard_current_game->setChecked(Settings::values.gamecard_current_game.GetValue());
    ui->dump_exefs->setChecked(Settings::values.dump_exefs.GetValue());
    ui->dump_nso->setChecked(Settings::values.dump_nso.GetValue());

    UpdateEnabledControls();
    UpdateExternalContentList();
}

void ConfigureFilesystem::ApplyConfiguration() {
    Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir,
                            ui->nand_directory_edit->text().toStdString());
    Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir,
                            ui->sdmc_directory_edit->text().toStdString());
    Common::FS::SetEdenPath(Common::FS::EdenPath::SaveDir,
                            ui->save_directory_edit->text().toStdString());
    Common::FS::SetEdenPath(Common::FS::EdenPath::DumpDir,
                            ui->dump_path_edit->text().toStdString());
    Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir,
                            ui->load_path_edit->text().toStdString());

    Settings::values.gamecard_inserted = ui->gamecard_inserted->isChecked();
    Settings::values.gamecard_current_game = ui->gamecard_current_game->isChecked();
    Settings::values.gamecard_path = ui->gamecard_path_edit->text().toStdString();
    Settings::values.dump_exefs = ui->dump_exefs->isChecked();
    Settings::values.dump_nso = ui->dump_nso->isChecked();

    std::vector<std::string> new_dirs;
    new_dirs.reserve(ui->external_content_list->count());
    for (int i = 0; i < ui->external_content_list->count(); ++i) {
        new_dirs.push_back(ui->external_content_list->item(i)->text().toStdString());
    }

    if (new_dirs != Settings::values.external_content_dirs) {
        Settings::values.external_content_dirs = std::move(new_dirs);
        emit ExternalContentDirsChanged();
    }
}

void ConfigureFilesystem::UpdateExternalContentList() {
    ui->external_content_list->clear();
    for (const auto& dir : Settings::values.external_content_dirs) {
        ui->external_content_list->addItem(QString::fromStdString(dir));
    }
}

void ConfigureFilesystem::AddExternalContentDirectory() {
    const QString dir_path = QFileDialog::getExistingDirectory(
        this, tr("Select External Content Directory..."), QString());

    if (dir_path.isEmpty()) {
        return;
    }

    QString normalized_path = QDir::toNativeSeparators(dir_path);
    if (normalized_path.back() != QDir::separator()) {
        normalized_path.append(QDir::separator());
    }

    for (int i = 0; i < ui->external_content_list->count(); ++i) {
        if (ui->external_content_list->item(i)->text() == normalized_path) {
            QMessageBox::information(this, tr("Directory Already Added"),
                                     tr("This directory is already in the list."));
            return;
        }
    }

    ui->external_content_list->addItem(normalized_path);
}

void ConfigureFilesystem::RemoveSelectedExternalContentDirectory() {
    auto selected = ui->external_content_list->selectedItems();
    if (!selected.isEmpty()) {
        qDeleteAll(ui->external_content_list->selectedItems());
    }
}

void ConfigureFilesystem::SetDirectory(DirectoryTarget target, QLineEdit* edit) {
    QString caption;

    switch (target) {
    case DirectoryTarget::NAND:
        caption = tr("Select Emulated NAND Directory...");
        break;
    case DirectoryTarget::SD:
        caption = tr("Select Emulated SD Directory...");
        break;
    case DirectoryTarget::Save:
        caption = tr("Select Save Data Directory...");
        break;
    case DirectoryTarget::Gamecard:
        caption = tr("Select Gamecard Path...");
        break;
    case DirectoryTarget::Dump:
        caption = tr("Select Dump Directory...");
        break;
    case DirectoryTarget::Load:
        caption = tr("Select Mod Load Directory...");
        break;
    }

    QString str;
    if (target == DirectoryTarget::Gamecard) {
        str = QFileDialog::getOpenFileName(this, caption, QFileInfo(edit->text()).dir().path(),
                                           tr("NX Gamecard (*.xci)"));
    } else {
        str = QFileDialog::getExistingDirectory(this, caption, edit->text());
    }

    if (str.isNull() || str.isEmpty()) {
        return;
    }

    if (target != DirectoryTarget::Gamecard && str.back() != QChar::fromLatin1('/')) {
        str.append(QChar::fromLatin1('/'));
    }

    edit->setText(str);
}

void ConfigureFilesystem::SetSaveDirectory() {
    const QString current_path = ui->save_directory_edit->text();
    const QString nand_path = ui->nand_directory_edit->text();

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Save Data Directory"));
    msgBox.setText(tr("Choose an action for the save data directory:"));

    QPushButton* customButton = msgBox.addButton(tr("Set Custom Path"), QMessageBox::ActionRole);
    QPushButton* resetButton = msgBox.addButton(tr("Reset to NAND"), QMessageBox::ActionRole);
    msgBox.addButton(QMessageBox::Cancel);

    msgBox.exec();

    if (msgBox.clickedButton() == customButton) {
        QString new_path = QFileDialog::getExistingDirectory(
            this, tr("Select Save Data Directory..."), current_path);

        if (new_path.isNull() || new_path.isEmpty()) {
            return;
        }

        if (new_path.back() != QChar::fromLatin1('/')) {
            new_path.append(QChar::fromLatin1('/'));
        }

        if (new_path != current_path) {
            PromptSaveMigration(current_path, new_path);
            ui->save_directory_edit->setText(new_path);
        }
    } else if (msgBox.clickedButton() == resetButton) {
        if (current_path != nand_path) {
            PromptSaveMigration(current_path, nand_path);
            ui->save_directory_edit->setText(nand_path);
        }
    }
}

void ConfigureFilesystem::PromptSaveMigration(const QString& from_path, const QString& to_path) {
    namespace fs = std::filesystem;

    const fs::path source_save_dir = fs::path(from_path.toStdString()) / "user" / "save";
    const fs::path dest_save_dir = fs::path(to_path.toStdString()) / "user" / "save";

    std::error_code ec;

    bool source_has_saves = false;
    if (Common::FS::Exists(source_save_dir)) {
        bool source_empty = fs::is_empty(source_save_dir, ec);
        source_has_saves = !ec && !source_empty;
    }

    // Check if destination already has saves
    bool dest_has_saves = false;
    if (Common::FS::Exists(dest_save_dir)) {
        bool dest_empty = fs::is_empty(dest_save_dir, ec);
        dest_has_saves = !ec && !dest_empty;
    }

    if (!source_has_saves) {
        return;
    }

    QString message;
    if (dest_has_saves) {
        message = tr("Save data exists in both the old and new locations.\n\n"
                     "Old: %1\n"
                     "New: %2\n\n"
                     "Would you like to migrate saves from the old location?\n"
                     "WARNING: This will overwrite any conflicting saves in the new location!")
                      .arg(QString::fromStdString(source_save_dir.string()))
                      .arg(QString::fromStdString(dest_save_dir.string()));
    } else {
        message = tr("Would you like to migrate your save data to the new location?\n\n"
                     "From: %1\n"
                     "To: %2")
                      .arg(QString::fromStdString(source_save_dir.string()))
                      .arg(QString::fromStdString(dest_save_dir.string()));
    }

    QMessageBox::StandardButton reply =
        QMessageBox::question(this, tr("Migrate Save Data"), message,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (reply != QMessageBox::Yes) {
        return;
    }

    QProgressDialog progress(tr("Migrating save data..."), tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();

    if (!Common::FS::Exists(dest_save_dir)) {
        if (!Common::FS::CreateDirs(dest_save_dir)) {
            progress.close();
            QMessageBox::warning(this, tr("Migration Failed"),
                                 tr("Failed to create destination directory."));
            return;
        }
    }

    fs::copy(source_save_dir, dest_save_dir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);

    progress.close();

    if (ec) {
        QMessageBox::warning(
            this, tr("Migration Failed"),
            tr("Failed to migrate save data:\n%1").arg(QString::fromStdString(ec.message())));
        return;
    }

    QMessageBox::StandardButton deleteReply =
        QMessageBox::question(this, tr("Migration Complete"),
                              tr("Save data has been migrated successfully.\n\n"
                                 "Would you like to delete the old save data?"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (deleteReply == QMessageBox::Yes) {
        Common::FS::RemoveDirRecursively(source_save_dir);
    }
}

void ConfigureFilesystem::UpdateEnabledControls() {
    ui->gamecard_current_game->setEnabled(ui->gamecard_inserted->isChecked());
    ui->gamecard_path_edit->setEnabled(ui->gamecard_inserted->isChecked() &&
                                       !ui->gamecard_current_game->isChecked());
    ui->gamecard_path_button->setEnabled(ui->gamecard_inserted->isChecked() &&
                                         !ui->gamecard_current_game->isChecked());
}

void ConfigureFilesystem::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();
}

void ConfigureFilesystem::SetupToolTips() {
    using ConfigurationShared::SetRichToolTip;

    SetRichToolTip(ui->nand_directory_edit, ui->label->text(),
                   tr("Holds the emulated console's internal storage: firmware, installed games, "
                      "updates and DLC, user profiles and, by default, saves. Changing it copies "
                      "nothing, leaves Save Data on the old folder and only takes full effect "
                      "after Nemyu restarts."),
                   tr("To move your data, copy the old NAND folder's contents into the new one "
                      "first, then choose Reset to NAND from the Save Data ... button."));
    SetRichToolTip(ui->nand_directory_button, ui->label->text(),
                   tr("Opens a folder picker for the NAND folder. Nothing is copied; restart Nemyu "
                      "after pressing OK so everything uses the new folder."));
    SetRichToolTip(ui->sdmc_directory_edit, ui->label_2->text(),
                   tr("Holds the emulated console's SD card. Changing it copies nothing, and games "
                      "keep using the old folder until Nemyu restarts or the game list reloads."),
                   tr("Mods also load from its atmosphere/contents/<title ID> folder; they show as "
                      "SDMC in a game's Properties > Add-Ons."));
    SetRichToolTip(ui->sdmc_directory_button, ui->label_2->text(),
                   tr("Opens a folder picker for the SD card folder. Nothing is copied; restart "
                      "Nemyu after pressing OK so games use the new folder."));
    SetRichToolTip(ui->save_directory_edit, ui->label_save->text(),
                   tr("Holds game saves, in its user/save subfolder. Defaults to the NAND folder, "
                      "but changing NAND on this page leaves it on the old one. A new path applies "
                      "from the next game boot."),
                   tr("Change it with the ... button instead of typing a path, so Nemyu offers to "
                      "copy your saves over."));
    SetRichToolTip(ui->save_directory_button, ui->label_save->text(),
                   tr("Offers Set Custom Path or Reset to NAND. If the old folder has saves, Nemyu "
                      "offers to copy them, overwriting same-named saves in the new folder, then "
                      "asks whether to delete the old ones."),
                   tr("Saves are copied at once but the new path applies only on OK, so don't "
                      "cancel the settings window after migrating."));
    SetRichToolTip(ui->gamecard_inserted, ui->gamecard_inserted->text(),
                   tr("Puts an emulated game card in the slot at the next game boot, from the Path "
                      "file or the booted game. Games still load from their own files, and game "
                      "card reads are not emulated, so it rarely changes anything."));
    SetRichToolTip(ui->gamecard_current_game, ui->gamecard_current_game->text(),
                   tr("Uses the game you boot as the inserted card instead of the Path file; only "
                      "an .xci game makes a valid card. Available while Inserted is ticked."));
    SetRichToolTip(ui->gamecard_path_edit, ui->label_3->text(),
                   tr("Sets the .xci game card image inserted at the next game boot while Current "
                      "Game is unticked."),
                   tr("Only .xci images work here; NSP files and extracted game folders do not "
                      "read as a game card."));
    SetRichToolTip(ui->gamecard_path_button, ui->label_3->text(),
                   tr("Opens a file picker that lists .xci game card images."));
    SetRichToolTip(ui->dump_path_edit, ui->label_8->text(),
                   tr("Holds ExeFS, NSO and RomFS dumps, each in a subfolder named after the title "
                      "ID, plus shader and macro dumps from the debug options. ExeFS and NSO dumps "
                      "keep going to the old folder until Nemyu restarts or the game list "
                      "reloads."),
                   tr("Pick a drive with room to spare: a full RomFS dump needs the RomFS size "
                      "plus 1 GiB free."));
    SetRichToolTip(ui->dump_path_button, ui->label_8->text(),
                   tr("Opens a folder picker for the Dump Root folder. Restart Nemyu after "
                      "pressing OK so ExeFS and NSO dumps also go there."));
    SetRichToolTip(ui->load_path_edit, ui->label_9->text(),
                   tr("Holds mods, each in <title ID>/<mod name>/ with exefs, romfs or cheats "
                      "folders inside; each can be turned off in the game's Properties > Add-Ons. "
                      "Games keep loading mods from the old folder until Nemyu restarts or the "
                      "game list reloads."),
                   tr("Right-click a game and choose Open Mod Data Location to open its folder "
                      "here directly."));
    SetRichToolTip(ui->load_path_button, ui->label_9->text(),
                   tr("Opens a folder picker for the Mod Load Root folder. Restart Nemyu after "
                      "pressing OK so games load mods from it."));
    SetRichToolTip(ui->dump_nso, ui->dump_nso->text(),
                   tr("Writes each code module (NSO) a game loads at boot to "
                      "<Dump Root>/<title ID>/nso, decompressed and before mod patches. It repeats "
                      "at every boot, adding disk writes; meant for modders."),
                   tr("An IPS patch for a module must be named after the build ID at the end of "
                      "that module's dump file name."));
    SetRichToolTip(ui->dump_exefs, ui->dump_exefs->text(),
                   tr("Copies the game's ExeFS (code files and main.npdm) to "
                      "<Dump Root>/<title ID>/exefs at each game boot, with its update and mods' "
                      "exefs files layered in. Skipped for games run from an extracted folder."),
                   tr("Turn it off after one boot; the code is copied as stored, so use Dump "
                      "Decompressed NSOs for decompressed code."));
    SetRichToolTip(ui->external_content_list, ui->groupBox_external->title(),
                   tr("Lists folders Nemyu scans, subfolders included, for update and DLC files "
                      "(NSP, NSZ, XCI, XCZ) and uses them without installing to NAND. Saving a "
                      "change rescans the game list unless a game is running."),
                   tr("Solid-compressed NSZ files are skipped; recompress them in block mode "
                      "(nsz -B) to use them here."));
    SetRichToolTip(ui->add_external_dir_button, ui->add_external_dir_button->text(),
                   tr("Opens a folder picker and adds the chosen folder to the External Content "
                      "list. The list is saved when you press OK, and rescanned then unless a "
                      "game is running."),
                   tr("Changed folders while a game was running? Press the refresh button in the "
                      "status bar after stopping it."));
    SetRichToolTip(ui->remove_external_dir_button, ui->remove_external_dir_button->text(),
                   tr("Removes the selected folder from the External Content list; files on disk "
                      "are not touched. Its updates and DLC stop being used once the list is "
                      "rescanned."));
}
