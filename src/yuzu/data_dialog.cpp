// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/data_dialog.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include "common/fs/path_util.h"
#include "qt_common/util/content.h"
#include "yuzu/util/icons.h"
#include "yuzu/util/util.h"

namespace fs = std::filesystem;
using FrontendCommon::DataManager::DataDir;

namespace {

QString ToQString(const fs::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

QString DataSize(u64 bytes) {
    return QLocale().formattedDataSize(static_cast<qint64>(bytes));
}

QPixmap IconPixmap(const char* icon, int size, const QWidget* widget) {
    return Icons::Get(icon).pixmap(QSize(size, size), widget->devicePixelRatioF());
}

} // Anonymous namespace

DataDialog::DataDialog(bool emulation_running_, QWidget* parent)
    : QDialog(parent),
      categories{{
          {DataDir::Shaders, "database", tr("Shaders"),
           tr("Vulkan and OpenGL pipeline caches of every game. Safe to clear: games build "
              "them again while you play, with some stutter at first."),
           QStringLiteral("Shaders")},
          {DataDir::Saves, "device-floppy", tr("Saves"),
           tr("Game save data of every profile. Clearing it deletes your progress; the Save "
              "Manager backs up, restores and moves the saves of each game."),
           QStringLiteral("Saves")},
          {DataDir::UserNand, "package", tr("Updates and DLC"),
           tr("Updates and DLC installed to the user NAND. Clearing it uninstalls them."),
           QStringLiteral("UserNand")},
          {DataDir::SysNand, "device-nintendo", tr("System NAND"),
           tr("Installed firmware and system applets. Clearing it removes the firmware; the "
              "system saves with your profiles are kept."),
           QStringLiteral("SysNand")},
          {DataDir::Mods, "puzzle", tr("Mods"),
           tr("Mods, patches and cheats of every game, from the load folder."),
           QStringLiteral("Mods")},
      }},
      emulation_running{emulation_running_} {
    setWindowTitle(tr("Data Manager"));
    resize(900, 520);

    category_list = new QListWidget(this);
    category_list->setIconSize(QSize(28, 28));
    category_list->setMinimumWidth(240);
    category_list->setSpacing(2);
    category_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (const Category& category : categories) {
        category_list->addItem(new QListWidgetItem(Icons::Get(category.icon), category.name));
    }

    icon_label = new QLabel(this);
    icon_label->setFixedSize(48, 48);
    title_label = new QLabel(this);
    QFont title_font = title_label->font();
    title_font.setPointSizeF(title_font.pointSizeF() * 1.3);
    title_font.setBold(true);
    title_label->setFont(title_font);
    size_label = new QLabel(this);
    QFont size_font = size_label->font();
    size_font.setBold(true);
    size_label->setFont(size_font);

    auto* header_text = new QVBoxLayout;
    header_text->addWidget(title_label);
    header_text->addWidget(size_label);
    header_text->addStretch();
    auto* header = new QHBoxLayout;
    header->addWidget(icon_label, 0, Qt::AlignTop);
    header->addLayout(header_text, 1);

    description_label = new QLabel(this);
    description_label->setWordWrap(true);
    path_label = new QLabel(this);
    path_label->setWordWrap(true);
    path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    hint_label = new QLabel(this);
    hint_label->setWordWrap(true);
    auto* hint_icon = new QLabel(this);
    hint_icon->setPixmap(IconPixmap("info-circle", 16, this));
    auto* hint = new QHBoxLayout;
    hint->addWidget(hint_icon, 0, Qt::AlignTop);
    hint->addWidget(hint_label, 1);

    const auto make_button = [this](const char* icon, const QString& text,
                                    const QString& tooltip) {
        auto* button = new QPushButton(Icons::Get(icon), text, this);
        button->setToolTip(tooltip);
        button->setMinimumHeight(30);
        return button;
    };
    open_button = make_button("folder-open", tr("Open Folder"),
                              tr("Opens this folder in your file manager."));
    export_button = make_button("file-export", tr("Export..."),
                                tr("Saves everything in this folder to a zip file."));
    import_button = make_button(
        "download", tr("Import..."),
        tr("Replaces everything in this folder with the contents of a zip file."));
    clear_button = make_button("trash", tr("Clear..."),
                               tr("Deletes everything in this folder. This can't be undone."));
    manager_button = make_button("database", QString(), QString());

    auto* actions = new QGridLayout;
    actions->setHorizontalSpacing(8);
    actions->setVerticalSpacing(6);
    actions->addWidget(open_button, 0, 0);
    actions->addWidget(export_button, 0, 1);
    actions->addWidget(import_button, 1, 0);
    actions->addWidget(clear_button, 1, 1);
    actions->addWidget(manager_button, 2, 0, 1, 2);
    actions->setColumnStretch(0, 1);
    actions->setColumnStretch(1, 1);

    auto* details = new QWidget(this);
    auto* details_layout = new QVBoxLayout(details);
    details_layout->setContentsMargins(8, 0, 0, 0);
    details_layout->addLayout(header);
    details_layout->addWidget(description_label);
    details_layout->addWidget(path_label);
    details_layout->addStretch(1);
    details_layout->addLayout(hint);
    details_layout->addLayout(actions);

    auto* splitter = new QSplitter(this);
    splitter->addWidget(category_list);
    splitter->addWidget(details);
    splitter->setStretchFactor(1, 1);
    splitter->setChildrenCollapsible(false);

    total_label = new QLabel(this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* refresh = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    refresh->setIcon(Icons::Get("refresh"));
    auto* root = buttons->addButton(tr("Open Data Folder"), QDialogButtonBox::ActionRole);
    root->setIcon(Icons::Get("folders"));
    root->setToolTip(tr("Opens the folder that holds all of the emulator's data."));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(refresh, &QPushButton::clicked, this, &DataDialog::Scan);
    connect(root, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            ToQString(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir))));
    });

    auto* footer = new QHBoxLayout;
    footer->addWidget(total_label, 1);
    footer->addWidget(buttons);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addLayout(footer);

    connect(category_list, &QListWidget::currentRowChanged, this, [this] { ShowSelected(); });
    connect(open_button, &QPushButton::clicked, this, &DataDialog::Open);
    connect(export_button, &QPushButton::clicked, this, &DataDialog::Export);
    connect(import_button, &QPushButton::clicked, this, &DataDialog::Import);
    connect(clear_button, &QPushButton::clicked, this, &DataDialog::Clear);
    connect(manager_button, &QPushButton::clicked, this, [this] {
        if (Selected().dir == DataDir::Shaders) {
            emit ShaderManagerRequested();
        } else {
            emit SaveManagerRequested();
        }
        Scan();
    });

    category_list->setCurrentRow(0);
    Scan();
}

DataDialog::~DataDialog() = default;

void DataDialog::Scan() {
    if (scanning) {
        return;
    }
    scanning = true;
    usage.fill(std::nullopt);
    std::vector<fs::path> dirs;
    for (const Category& category : categories) {
        dirs.push_back(FrontendCommon::DataManager::GetDataDir(category.dir));
    }

    auto* watcher = new QFutureWatcher<std::vector<Usage>>(this);
    connect(watcher, &QFutureWatcher<std::vector<Usage>>::finished, this, [this, watcher] {
        const std::vector<Usage> result = watcher->result();
        for (size_t i = 0; i < result.size(); ++i) {
            usage[i] = result[i];
        }
        scanning = false;
        watcher->deleteLater();
        ShowSelected();
    });
    watcher->setFuture(QtConcurrent::run([dirs = std::move(dirs)] {
        std::vector<Usage> result;
        for (const fs::path& dir : dirs) {
            Usage folder;
            std::error_code error;
            fs::recursive_directory_iterator it(
                dir, fs::directory_options::skip_permission_denied, error);
            for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
                std::error_code entry_error;
                if (it->is_regular_file(entry_error)) {
                    folder.bytes += it->file_size(entry_error);
                    ++folder.files;
                }
            }
            result.push_back(folder);
        }
        return result;
    }));
    ShowSelected();
}

void DataDialog::ShowSelected() {
    u64 total = 0;
    for (size_t i = 0; i < categories.size(); ++i) {
        const QString size = usage[i] ? DataSize(usage[i]->bytes) : tr("Calculating...");
        category_list->item(static_cast<int>(i))
            ->setText(QStringLiteral("%1\n%2").arg(categories[i].name, size));
        total += usage[i] ? usage[i]->bytes : 0;
    }
    total_label->setText(scanning ? tr("Calculating the size of your data...")
                                  : tr("All data: %1").arg(DataSize(total)));

    const int row = std::max(category_list->currentRow(), 0);
    const Category& category = Selected();
    icon_label->setPixmap(IconPixmap(category.icon, 48, this));
    title_label->setText(category.name);
    description_label->setText(category.description);
    path_label->setText(ToQString(FrontendCommon::DataManager::GetDataDir(category.dir)));
    if (const auto& folder = usage[static_cast<size_t>(row)]) {
        size_label->setText(folder->files == 0
                                ? tr("Empty")
                                : tr("%1 in %n file(s)", "", static_cast<int>(folder->files))
                                      .arg(DataSize(folder->bytes)));
    } else {
        size_label->setText(tr("Calculating..."));
    }

    const bool changeable = !emulation_running && !scanning;
    import_button->setEnabled(changeable);
    clear_button->setEnabled(changeable);
    if (emulation_running) {
        hint_label->setText(
            tr("A game is running, so importing and clearing are off until it stops."));
    } else {
        hint_label->setText(
            tr("Importing and clearing replace what is in this folder, and can't be undone. "
               "Export it first to keep a copy."));
    }

    const bool has_manager = category.dir == DataDir::Shaders || category.dir == DataDir::Saves;
    manager_button->setVisible(has_manager);
    if (has_manager) {
        const bool shaders = category.dir == DataDir::Shaders;
        manager_button->setIcon(Icons::Get(shaders ? "database" : "device-floppy"));
        manager_button->setText(shaders ? tr("Open Shader Manager") : tr("Open Save Manager"));
        manager_button->setToolTip(shaders ? tr("Manages the shader caches of each game.")
                                           : tr("Manages and backs up the saves of each game."));
    }
}

const DataDialog::Category& DataDialog::Selected() const {
    const int row = category_list->currentRow();
    return categories[row < 0 ? 0 : static_cast<size_t>(row)];
}

std::optional<std::string> DataDialog::SelectProfile() const {
    if (Selected().dir != DataDir::Saves) {
        return std::string{};
    }
    std::string user_id = GetProfileIDString();
    if (user_id.empty()) {
        return std::nullopt;
    }
    return user_id;
}

void DataDialog::Open() {
    const std::optional<std::string> user_id = SelectProfile();
    if (!user_id) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(
        FrontendCommon::DataManager::GetDataDirString(Selected().dir, *user_id))));
}

void DataDialog::Export() {
    const std::optional<std::string> user_id = SelectProfile();
    if (!user_id) {
        return;
    }
    QtCommon::Content::ExportDataDir(Selected().dir, *user_id, Selected().export_name);
}

void DataDialog::Import() {
    const std::optional<std::string> user_id = SelectProfile();
    if (!user_id) {
        return;
    }
    QtCommon::Content::ImportDataDir(Selected().dir, *user_id, [this] { Scan(); });
}

void DataDialog::Clear() {
    const std::optional<std::string> user_id = SelectProfile();
    if (!user_id) {
        return;
    }
    QtCommon::Content::ClearDataDir(Selected().dir, *user_id);
    Scan();
}
