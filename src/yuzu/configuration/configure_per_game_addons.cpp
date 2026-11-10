// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <utility>

#include <fmt/format.h>

#include <QDesktopServices>
#include <QFileInfo>
#include <QHeaderView>
#include <QMenu>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QTreeView>
#include <QUrl>

#include "common/common_types.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "configuration/addon/mod_select_dialog.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"
#include "core/loader/nup.h"
#include "frontend_common/mod_manager.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/mod.h"
#include "ui_configure_per_game_addons.h"
#include "yuzu/configuration/configure_input.h"
#include "yuzu/configuration/configure_per_game_addons.h"

namespace {

// The Version and Type columns of a patch. For mods, PatchManager puts what they contain in the
// version.
std::pair<QString, QString> DescribePatch(const FileSys::Patch& patch) {
    const QString version = QString::fromStdString(patch.version);
    switch (patch.type) {
    case FileSys::PatchType::Update:
        switch (patch.source) {
        case FileSys::PatchSource::NAND:
            return {version, ConfigurePerGameAddons::tr("Update (NAND)")};
        case FileSys::PatchSource::SDMC:
            return {version, ConfigurePerGameAddons::tr("Update (SD card)")};
        case FileSys::PatchSource::External:
            return {version, ConfigurePerGameAddons::tr("Update (external file)")};
        case FileSys::PatchSource::Packed:
            // An XCI or NSP update is only known as packed; a NUP container names its version
            return {patch.version == "PACKED" ? QString{} : version,
                    ConfigurePerGameAddons::tr("Update (in the game file)")};
        default:
            return {version, ConfigurePerGameAddons::tr("Update")};
        }
    case FileSys::PatchType::DLC:
        // The version holds the numbers of the DLC found
        return {version, ConfigurePerGameAddons::tr("DLC")};
    case FileSys::PatchType::Mod:
        break;
    }
    if (patch.opt_in) {
        return {QString{}, ConfigurePerGameAddons::tr("Built-in patch")};
    }
    if (patch.version == "Cheats" && patch.name.starts_with("cheat_")) {
        return {QString{}, ConfigurePerGameAddons::tr("Cheats")};
    }
    if (patch.name == "SDMC" && patch.location.empty()) {
        return {QString{}, ConfigurePerGameAddons::tr("Mod on the SD card: %1").arg(version)};
    }
    return {QString{}, ConfigurePerGameAddons::tr("Mod: %1").arg(version)};
}

} // Anonymous namespace

ConfigurePerGameAddons::ConfigurePerGameAddons(Core::System& system_, QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigurePerGameAddons>()}, system{system_} {
    ui->setupUi(this);

    item_model = new QStandardItemModel(ui->tree_view);
    ui->tree_view->setModel(item_model);

    item_model->insertColumns(0, COLUMN_COUNT);
    RetranslateUI();

    QHeaderView* const header = ui->tree_view->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(NAME_COLUMN, QHeaderView::Stretch);
    header->setSectionResizeMode(VERSION_COLUMN, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(TYPE_COLUMN, QHeaderView::ResizeToContents);
    // Listed in the order PatchManager gives until a header is clicked
    header->setSortIndicator(-1, Qt::AscendingOrder);

    // We must register all custom types with the Qt Automoc system so that we are able to use it
    // with signals/slots. In this case, QList falls under the umbrella of custom types.
    qRegisterMetaType<QList<QStandardItem*>>("QList<QStandardItem*>");

    ui->tree_view->setEnabled(!system.IsPoweredOn());

    connect(item_model, &QStandardItemModel::itemChanged, this,
            &ConfigurePerGameAddons::OnItemChanged);
    connect(item_model, &QStandardItemModel::itemChanged,
            [] { UISettings::values.is_game_list_reload_pending.exchange(true); });

    connect(ui->folder, &QAbstractButton::clicked, this, &ConfigurePerGameAddons::InstallModFolder);
    connect(ui->zip, &QAbstractButton::clicked, this, &ConfigurePerGameAddons::InstallModZip);
    connect(ui->open_mods_folder, &QAbstractButton::clicked, this,
            &ConfigurePerGameAddons::OpenModsFolder);

    connect(ui->tree_view, &QTreeView::customContextMenuRequested, this,
            &ConfigurePerGameAddons::showContextMenu);
}

ConfigurePerGameAddons::~ConfigurePerGameAddons() = default;

void ConfigurePerGameAddons::OnItemChanged(QStandardItem* item) {
    if (update_items.size() > 1 && item->checkState() == Qt::Checked) {
        auto it = std::find(update_items.begin(), update_items.end(), item);
        if (it != update_items.end()) {
            for (auto* update_item : update_items) {
                if (update_item != item && update_item->checkState() == Qt::Checked) {
                    disconnect(item_model, &QStandardItemModel::itemChanged, this,
                               &ConfigurePerGameAddons::OnItemChanged);
                    update_item->setCheckState(Qt::Unchecked);
                    connect(item_model, &QStandardItemModel::itemChanged, this,
                            &ConfigurePerGameAddons::OnItemChanged);
                }
            }
        }
    }
}

void ConfigurePerGameAddons::ApplyConfiguration() {
    std::vector<std::string> disabled_addons;
    std::vector<std::string> enabled_addons;

    for (const auto& item : list_items) {
        if (item.front()->data(OPT_IN).toBool()) {
            // Off by default: kept in the enabled list when checked, in no list otherwise.
            if (item.front()->checkState() == Qt::Checked) {
                enabled_addons.push_back(item.front()->text().toStdString());
            }
            continue;
        }
        const auto disabled = item.front()->checkState() == Qt::Unchecked;
        if (disabled) {
            QVariant userData = item.front()->data(Qt::UserRole);
            if (userData.isValid() && userData.canConvert<quint32>() &&
                item.front()->text() == QStringLiteral("Update")) {
                quint32 numeric_version = userData.toUInt();
                disabled_addons.push_back(fmt::format("Update@{}", numeric_version));
            } else {
                disabled_addons.push_back(item.front()->text().toStdString());
            }
        }
    }

    auto current = Settings::values.disabled_addons[title_id];
    std::sort(disabled_addons.begin(), disabled_addons.end());
    std::sort(current.begin(), current.end());
    const auto enabled_it = Settings::values.enabled_addons.find(title_id);
    auto current_enabled = enabled_it != Settings::values.enabled_addons.end()
                               ? enabled_it->second
                               : std::vector<std::string>{};
    std::sort(enabled_addons.begin(), enabled_addons.end());
    std::sort(current_enabled.begin(), current_enabled.end());
    if (disabled_addons != current || enabled_addons != current_enabled) {
        Common::FS::RemoveFile(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                               "game_list" / fmt::format("{:016X}.pv.txt", title_id));
    }

    Settings::values.disabled_addons[title_id] = disabled_addons;
    if (enabled_addons.empty()) {
        Settings::values.enabled_addons.erase(title_id);
    } else {
        Settings::values.enabled_addons[title_id] = std::move(enabled_addons);
    }
}

void ConfigurePerGameAddons::LoadFromFile(FileSys::VirtualFile file_) {
    file = std::move(file_);
    LoadConfiguration();
}

void ConfigurePerGameAddons::SetTitleId(u64 id) {
    this->title_id = id;
}

void ConfigurePerGameAddons::InstallMods(const QStringList& mods) {
    QStringList failed;
    for (const auto& mod : mods) {
        if (FrontendCommon::InstallMod(mod.toStdString(), title_id, true) ==
            FrontendCommon::Failed) {
            failed << QFileInfo(mod).baseName();
        }
    }

    if (failed.empty()) {
        QtCommon::Frontend::Information(tr("Mod Install Succeeded"),
                                        tr("Successfully installed all mods."));

        item_model->removeRows(0, item_model->rowCount());
        list_items.clear();
        LoadConfiguration();

        UISettings::values.is_game_list_reload_pending.exchange(true);
    } else {
        QtCommon::Frontend::Critical(
            tr("Mod Install Failed"),
            tr("Failed to install the following mods:\n\t%1\nCheck the log for details.")
                .arg(failed.join(QStringLiteral("\n\t"))));
    }
}

void ConfigurePerGameAddons::InstallModPath(const QString& path, const QString& fallbackName) {
    const auto mods = QtCommon::Mod::GetModFolders(path, fallbackName);

    if (mods.size() > 1) {
        ModSelectDialog* dialog = new ModSelectDialog(mods, this);
        connect(dialog, &ModSelectDialog::modsSelected, this, &ConfigurePerGameAddons::InstallMods);
        dialog->show();
    } else if (!mods.empty()) {
        InstallMods(mods);
    }
}

void ConfigurePerGameAddons::InstallModFolder() {
    const auto path = QtCommon::Frontend::GetExistingDirectory(
        tr("Mod Folder"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (path.isEmpty()) {
        return;
    }

    InstallModPath(path);
}

void ConfigurePerGameAddons::InstallModZip() {
    // TODO(crueter): use GetOpenFileName to allow select multiple ZIPs
    const auto path = QtCommon::Frontend::GetOpenFileName(
        tr("Zipped Mod Location"),
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation),
        tr("Zipped Archives (*.zip)"));
    if (path.isEmpty()) {
        return;
    }

    const QString extracted = QtCommon::Mod::ExtractMod(path);
    if (!extracted.isEmpty())
        InstallModPath(extracted, QFileInfo(path).baseName());
}

void ConfigurePerGameAddons::AddonDeleteRequested(QList<QModelIndex> selected) {
    QList<QModelIndex> filtered;
    for (const QModelIndex& index : selected) {
        if (!index.data(PATCH_LOCATION).toString().isEmpty())
            filtered << index;
    }

    if (filtered.empty()) {
        QtCommon::Frontend::Critical(tr("Invalid Selection"),
                                     tr("Only mods, cheats, and patches can be deleted.\nTo delete "
                                        "NAND-installed updates, right-click the game in the game "
                                        "list and click Remove -> Remove Installed Update."));
        return;
    }

    const auto header = tr("You are about to delete the following installed mods:\n");
    QString selected_str;
    for (const QModelIndex& index : filtered) {
        selected_str = selected_str % index.data().toString() % QStringLiteral("\n");
    }

    const auto footer = tr("\nOnce deleted, these can NOT be recovered. Are you 100% sure "
                           "you want to delete them?");

    QString caption = header % selected_str % footer;

    auto choice = QtCommon::Frontend::Warning(tr("Delete add-on(s)?"), caption,
                                              QtCommon::Frontend::StandardButton::Yes |
                                                  QtCommon::Frontend::StandardButton::No);

    if (choice == QtCommon::Frontend::StandardButton::No)
        return;

    for (const QModelIndex& index : filtered) {
        std::filesystem::remove_all(index.data(PATCH_LOCATION).toString().toStdString());
    }

    QtCommon::Frontend::Information(tr("Successfully deleted"),
                                    tr("Successfully deleted all selected mods."));

    item_model->removeRows(0, item_model->rowCount());
    list_items.clear();
    LoadConfiguration();

    UISettings::values.is_game_list_reload_pending.exchange(true);
}

void ConfigurePerGameAddons::OpenModsFolder() {
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) /
                      fmt::format("{:016X}", title_id);
    if (!Common::FS::CreateDirs(path)) {
        LOG_ERROR(Frontend, "Unable to create the mods folder");
    }
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QString::fromStdString(Common::FS::PathToUTF8String(path))));
}

void ConfigurePerGameAddons::showContextMenu(const QPoint& pos) {
    const QModelIndex index = ui->tree_view->indexAt(pos);
    auto selected = ui->tree_view->selectionModel()->selectedRows();
    if (index.isValid() && selected.empty()) {
        QModelIndex idx = item_model->index(index.row(), 0);
        if (idx.isValid())
            selected << idx;
    }

    if (selected.empty())
        return;

    QMenu menu(this);

    if (selected.length() == 1) {
        const QString loc = selected.at(0).data(PATCH_LOCATION).toString();
        const QFileInfo info(loc);
        if (!loc.isEmpty() && info.exists()) {
            // A cheat file opens the folder it is in
            const QString folder = info.isDir() ? loc : info.absolutePath();
            QAction* open = menu.addAction(tr("&Open Folder"));
            connect(open, &QAction::triggered, this,
                    [folder]() { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
        }
    }

    // Only mods and cheats have files of their own to delete
    const bool deletable = std::ranges::any_of(selected, [](const QModelIndex& selected_index) {
        return !selected_index.data(PATCH_LOCATION).toString().isEmpty();
    });
    if (deletable) {
        QAction* remove = menu.addAction(tr("&Delete..."));
        connect(remove, &QAction::triggered, this,
                [this, selected]() { AddonDeleteRequested(selected); });
    }

    if (!menu.isEmpty()) {
        menu.exec(ui->tree_view->viewport()->mapToGlobal(pos));
    }
}

void ConfigurePerGameAddons::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigurePerGameAddons::RetranslateUI() {
    ui->retranslateUi(this);
    item_model->setHeaderData(NAME_COLUMN, Qt::Horizontal, tr("Name"));
    item_model->setHeaderData(VERSION_COLUMN, Qt::Horizontal, tr("Version"));
    item_model->setHeaderData(TYPE_COLUMN, Qt::Horizontal, tr("Type"));
}

void ConfigurePerGameAddons::LoadConfiguration() {
    if (file == nullptr) {
        return;
    }

    const FileSys::PatchManager pm{title_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto loader = Loader::GetLoader(system, file);

    FileSys::VirtualFile update_raw;
    loader->ReadUpdateRaw(update_raw);

    auto patches = pm.GetPatches(update_raw);
    // A NUP container's own update and add-on content, which no content provider lists
    if (loader->GetFileType() == Loader::FileType::NUP) {
        const auto& nup_loader = static_cast<const Loader::AppLoader_NUP&>(*loader);
        for (auto& carried : nup_loader.GetCarriedPatches()) {
            const auto installed = std::find_if(patches.begin(), patches.end(), [](const auto& p) {
                return p.type == FileSys::PatchType::DLC;
            });
            if (carried.type == FileSys::PatchType::DLC && installed != patches.end()) {
                installed->version += ", " + carried.version;
            } else {
                patches.push_back(std::move(carried));
            }
        }
    }
    SetPatches(patches);
}

void ConfigurePerGameAddons::SetPatches(const std::vector<FileSys::Patch>& patches) {
    const auto& disabled = Settings::values.disabled_addons[title_id];
    const auto enabled_it = Settings::values.enabled_addons.find(title_id);
    const std::vector<std::string> enabled = enabled_it != Settings::values.enabled_addons.end()
                                                 ? enabled_it->second
                                                 : std::vector<std::string>{};

    update_items.clear();
    list_items.clear();
    item_model->removeRows(0, item_model->rowCount());

    bool has_enabled_update = false;

    for (const auto& patch : patches) {
        const auto name = QString::fromStdString(patch.name);

        auto* const first_item = new QStandardItem;
        first_item->setText(name);
        first_item->setCheckable(true);

        const bool is_external_update = patch.type == FileSys::PatchType::Update &&
                                        patch.source == FileSys::PatchSource::External &&
                                        patch.numeric_version != 0;

        const bool is_mod = patch.type == FileSys::PatchType::Mod;

        if (is_external_update) {
            first_item->setData(static_cast<quint32>(patch.numeric_version), NUMERIC_VERSION);
        } else if (is_mod) {
            first_item->setData(QString::fromStdString(patch.location), PATCH_LOCATION);
        }

        bool patch_disabled = false;
        if (patch.opt_in) {
            first_item->setData(true, OPT_IN);
            patch_disabled =
                std::find(enabled.begin(), enabled.end(), name.toStdString()) == enabled.end();
        } else if (is_external_update) {
            std::string disabled_key = fmt::format("Update@{}", patch.numeric_version);
            patch_disabled =
                std::find(disabled.begin(), disabled.end(), disabled_key) != disabled.end();
        } else {
            patch_disabled =
                std::find(disabled.begin(), disabled.end(), name.toStdString()) != disabled.end();
        }

        bool should_enable = !patch_disabled;

        if (patch.type == FileSys::PatchType::Update) {
            if (should_enable) {
                if (has_enabled_update) {
                    should_enable = false;
                } else {
                    has_enabled_update = true;
                }
            }
            update_items.push_back(first_item);
        }

        first_item->setCheckState(should_enable ? Qt::Checked : Qt::Unchecked);

        const auto [version, type] = DescribePatch(patch);
        auto* const type_item = new QStandardItem{type};
        if (patch.opt_in) {
            type_item->setToolTip(tr("Part of Nemyu. Off unless ticked."));
        }
        list_items.push_back(
            QList<QStandardItem*>{first_item, new QStandardItem{version}, type_item});
        item_model->appendRow(list_items.back());
    }

    // Keeps the order the player picked when the list is loaded again after an install
    const QHeaderView* const header = ui->tree_view->header();
    if (header->sortIndicatorSection() >= 0) {
        item_model->sort(header->sortIndicatorSection(), header->sortIndicatorOrder());
    }
}
