// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/shader_manager_dialog.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <system_error>

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "video_core/renderer_vulkan/pipeline_cache_file.h"
#include "video_core/shader_cache_v2.h"
#include "yuzu/util/icons.h"
#include "yuzu/util/other_emulators.h"

namespace fs = std::filesystem;
namespace CacheFile = Vulkan::PipelineCacheFile;
namespace CacheLog = VideoCommon::ShaderCacheV2;

namespace {

constexpr int PathRole = Qt::UserRole;
constexpr int ImportableRole = Qt::UserRole + 1;
constexpr int ProgramIdRole = Qt::UserRole + 2;

// Files the emulator itself keeps in a game's folder; anything else is left over.
constexpr std::array<std::string_view, 7> OwnFiles{
    "vulkan.bin",          "vulkan_mesh.bin", "vulkan_used.bin",     "vulkan_pipelines.bin",
    "vulkan_binaries.bin", "vulkan_log.bin",  "vulkan_log.bin.lock"};

QString ToQString(const fs::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

fs::path ToPath(const QString& text) {
    return fs::path{text.toStdU16String()};
}

QString FolderName(u64 program_id) {
    return QString::fromStdString(fmt::format("{:016x}", program_id));
}

QString ProgramIdText(u64 program_id) {
    return QString::fromStdString(fmt::format("{:016X}", program_id));
}

fs::path ShaderRoot() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
}

bool IsOwnFile(const fs::path& file) {
    const std::string name = Common::FS::PathToUTF8String(file.filename());
    return std::ranges::find(OwnFiles, name) != OwnFiles.end();
}

QString DataSize(u64 bytes) {
    return QLocale().formattedDataSize(static_cast<qint64>(bytes));
}

u64 FolderSize(const fs::path& dir) {
    u64 total = 0;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (entry.is_regular_file(error)) {
            total += entry.file_size(error);
        }
    }
    return total;
}

struct OtherCache {
    QString emulator;
    fs::path file;
};

// vulkan.bin of the same game in other emulators' shader folders.
std::vector<OtherCache> FindOtherCaches(u64 program_id) {
    std::vector<OtherCache> found;
    std::error_code error;
    for (const OtherEmulators::Folder& root : OtherEmulators::Equivalents(ShaderRoot())) {
        for (const std::string& id : {fmt::format("{:016x}", program_id),
                                      fmt::format("{:016X}", program_id)}) {
            const fs::path file = root.path / id / "vulkan.bin";
            if (fs::is_regular_file(file, error)) {
                found.push_back({root.emulator, file});
                break;
            }
        }
    }
    return found;
}

struct Description {
    QString text;
    bool importable{};
    bool in_use{};
};

Description Describe(const fs::path& file, bool own_folder) {
    const std::string name = Common::FS::PathToUTF8String(file.filename());
    if (name == "vulkan_used.bin") {
        return {QObject::tr("Pipelines you used, built first at boot")};
    }
    if (name == "vulkan_pipelines.bin") {
        return {QObject::tr("Driver pipeline cache")};
    }
    if (name == "vulkan_binaries.bin") {
        return {QObject::tr("Compiled shaders saved from the driver")};
    }
    if (name == "vulkan_log.bin.lock") {
        return {QObject::tr("Taken while the shader cache log is written")};
    }
    if (name == "vulkan_log.bin") {
        const CacheFile::Contents contents = CacheFile::InspectLog(file);
        QString text = QObject::tr("Shader cache log, %n pipeline(s)", "",
                                   static_cast<int>(contents.graphics + contents.compute +
                                                    contents.mesh));
        if (contents.damaged) {
            text += QObject::tr(", damaged records skipped");
        }
        const bool in_use = own_folder && CacheLog::Enabled();
        return {in_use ? QObject::tr("In use: %1").arg(text)
                       : QObject::tr("%1; unused while EDEN_NX2_SHADER_CACHE_V2 is 0").arg(text),
                false, in_use};
    }
    const std::optional<u32> version = CacheFile::ReadVersion(file);
    if (!version) {
        return {QObject::tr("Not a shader cache")};
    }
    if (*version == CacheFile::CurrentVersion) {
        const CacheFile::Contents contents = CacheFile::Inspect(file);
        const u32 total = contents.graphics + contents.compute + contents.mesh;
        QString text = QObject::tr("%n pipeline(s)", "", static_cast<int>(total));
        if (contents.mesh != 0 && contents.mesh == total) {
            text = QObject::tr("%n task/mesh pipeline(s)", "", static_cast<int>(total));
        }
        if (contents.damaged) {
            text += QObject::tr(", damaged past them");
        }
        const bool in_use = own_folder && IsOwnFile(file);
        if (in_use && CacheLog::Enabled()) {
            return {QObject::tr("Taken into the shader cache log at boot: %1").arg(text)};
        }
        return {in_use ? QObject::tr("In use: %1").arg(text)
                       : QObject::tr("Can be imported: %1").arg(text),
                !in_use, in_use};
    }
    if (CacheFile::IsImportable(*version)) {
        const CacheFile::Contents contents = CacheFile::Inspect(file);
        QString text = QObject::tr("%n pipeline(s)", "",
                                   static_cast<int>(contents.graphics + contents.compute +
                                                    contents.mesh));
        if (contents.damaged) {
            text += QObject::tr(", damaged past them");
        }
        const bool same_layout =
            std::ranges::find(CacheFile::SameLayoutVersions, *version) !=
            CacheFile::SameLayoutVersions.end();
        return {(same_layout
                     ? QObject::tr("Cache version %1, read as this build's, can be imported: %2")
                     : QObject::tr("Lichii cache version %1, can be imported: %2"))
                    .arg(*version)
                    .arg(text),
                true};
    }
    static const QRegularExpression parked{QStringLiteral("^vulkan(_mesh)?\\.v(\\d+)\\.bin$")};
    const auto match = parked.match(QString::fromStdString(name));
    if (own_folder && match.hasMatch() && match.captured(2).toUInt() == *version) {
        return {QObject::tr("Set aside by a build of cache version %1; unused by this one")
                    .arg(*version)};
    }
    return {QObject::tr("Cache version %1 from another build or emulator; this build reads "
                        "version %2")
                .arg(*version)
                .arg(CacheFile::CurrentVersion)};
}

QTreeWidgetItem* AddFileItem(QTreeWidgetItem* parent, const fs::path& file, const QString& label,
                             bool own_folder) {
    const QFileInfo info(ToQString(file));
    const Description description = Describe(file, own_folder);
    auto* item = new QTreeWidgetItem(
        parent, {label, description.text, DataSize(static_cast<u64>(info.size())),
                 QLocale().toString(info.lastModified(), QLocale::ShortFormat)});
    item->setData(0, PathRole, ToQString(file));
    item->setData(0, ImportableRole, description.importable);
    item->setToolTip(0, ToQString(file));
    item->setToolTip(1, description.text);
    if (description.in_use) {
        QFont font = item->font(1);
        font.setBold(true);
        item->setFont(1, font);
    }
    return item;
}

} // Anonymous namespace

ShaderManagerDialog::ShaderManagerDialog(std::vector<ShaderManagerGame> games_,
                                         u64 selected_program_id, bool emulation_running_,
                                         QWidget* parent)
    : QDialog(parent), games{std::move(games_)}, emulation_running{emulation_running_} {
    setWindowTitle(tr("Shader Manager"));
    resize(1120, 640);

    game_list = new QListWidget(this);
    game_list->setIconSize(QSize(40, 40));
    game_list->setMinimumWidth(300);
    game_list->setSpacing(2);
    game_list->setWordWrap(true);
    game_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    icon_label = new QLabel(this);
    icon_label->setFixedSize(64, 64);
    title_label = new QLabel(this);
    QFont title_font = title_label->font();
    title_font.setPointSizeF(title_font.pointSizeF() * 1.3);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_label = new QLabel(this);
    summary_label->setWordWrap(true);
    summary_label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* header_text = new QVBoxLayout;
    header_text->addWidget(title_label);
    header_text->addWidget(summary_label);
    header_text->addStretch();
    auto* header = new QHBoxLayout;
    header->addWidget(icon_label, 0, Qt::AlignTop);
    header->addLayout(header_text, 1);

    file_tree = new QTreeWidget(this);
    file_tree->setColumnCount(4);
    file_tree->setHeaderLabels({tr("File"), tr("What it is"), tr("Size"), tr("Modified")});
    file_tree->setRootIsDecorated(true);
    file_tree->setUniformRowHeights(true);
    file_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    file_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    file_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    file_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    hint_label = new QLabel(this);
    hint_label->setWordWrap(true);

    const auto make_button = [this](const char* icon, const QString& text,
                                    const QString& tooltip) {
        auto* button = new QPushButton(Icons::Get(icon), text, this);
        button->setToolTip(tooltip);
        return button;
    };
    import_button = make_button(
        "download", tr("Import..."),
        tr("Adds the pipelines of a shader cache file (or of the selected cache from another "
           "emulator) that this game's cache doesn't have yet."));
    export_button = make_button(
        "file-export", tr("Export..."),
        tr("Copies this game's cache to a folder, to keep it or share it."));
    prepare_button = make_button(
        "stack-2", tr("Prepare Shaders"),
        tr("Builds every pipeline in this game's cache and stops, so the game doesn't compile "
           "them while you play."));
    clean_button = make_button(
        "eraser", tr("Clean Up..."),
        tr("Deletes the cache files of other versions and emulators that this build can't use."));
    delete_button = make_button("trash", tr("Delete Cache..."),
                                tr("Deletes every shader cache file of this game."));
    folder_button = make_button("folder-open", tr("Open Folder"),
                                tr("Opens this game's shader cache folder."));

    auto* actions = new QGridLayout;
    actions->setHorizontalSpacing(8);
    actions->setVerticalSpacing(6);
    const std::array<QPushButton*, 6> action_buttons{import_button, export_button, prepare_button,
                                                     folder_button, clean_button, delete_button};
    for (size_t i = 0; i < action_buttons.size(); ++i) {
        action_buttons[i]->setMinimumHeight(30);
        actions->addWidget(action_buttons[i], static_cast<int>(i / 3), static_cast<int>(i % 3));
    }
    for (int column = 0; column < 3; ++column) {
        actions->setColumnStretch(column, 1);
    }

    auto* details = new QWidget(this);
    auto* details_layout = new QVBoxLayout(details);
    details_layout->setContentsMargins(0, 0, 0, 0);
    details_layout->addLayout(header);
    details_layout->addWidget(file_tree, 1);
    details_layout->addWidget(hint_label);
    details_layout->addLayout(actions);

    auto* splitter = new QSplitter(this);
    splitter->addWidget(game_list);
    splitter->addWidget(details);
    splitter->setStretchFactor(1, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* refresh = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    refresh->setIcon(Icons::Get("refresh"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(refresh, &QPushButton::clicked, this, [this] { Populate(SelectedProgramId()); });

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addWidget(buttons);

    connect(game_list, &QListWidget::currentRowChanged, this, [this] { ShowSelectedGame(); });
    connect(file_tree, &QTreeWidget::itemSelectionChanged, this, [this] { UpdateButtons(); });
    connect(import_button, &QPushButton::clicked, this, &ShaderManagerDialog::Import);
    connect(export_button, &QPushButton::clicked, this, &ShaderManagerDialog::Export);
    connect(prepare_button, &QPushButton::clicked, this, &ShaderManagerDialog::PrepareShaders);
    connect(clean_button, &QPushButton::clicked, this, &ShaderManagerDialog::CleanUp);
    connect(delete_button, &QPushButton::clicked, this, &ShaderManagerDialog::DeleteCache);
    connect(folder_button, &QPushButton::clicked, this, &ShaderManagerDialog::OpenFolder);

    Populate(selected_program_id);
}

ShaderManagerDialog::~ShaderManagerDialog() = default;

void ShaderManagerDialog::Populate(u64 select_program_id) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<u64> ids;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(ShaderRoot(), error)) {
        if (!entry.is_directory(error)) {
            continue;
        }
        const std::string name = Common::FS::PathToUTF8String(entry.path().filename());
        bool parsed = false;
        const u64 id = QString::fromStdString(name).toULongLong(&parsed, 16);
        if (parsed && name.size() == 16 && id != 0) {
            ids.push_back(id);
        }
    }
    for (const ShaderManagerGame& game : games) {
        if (game.program_id != 0 && !FindOtherCaches(game.program_id).empty()) {
            ids.push_back(game.program_id);
        }
    }
    if (select_program_id != 0) {
        ids.push_back(select_program_id);
    }
    std::ranges::sort(ids);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::ranges::sort(ids, [this](u64 a, u64 b) {
        return GameTitle(a).localeAwareCompare(GameTitle(b)) < 0;
    });

    game_list->blockSignals(true);
    game_list->clear();
    int select_row = ids.empty() ? -1 : 0;
    for (const u64 id : ids) {
        const ShaderManagerGame* game = FindGame(id);
        const u64 size = FolderSize(GameDir(id));
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n%2").arg(GameTitle(id),
                                         size != 0 ? DataSize(size) : tr("No cache yet")));
        if (game != nullptr && !game->icon.isNull()) {
            item->setIcon(QIcon(game->icon));
        } else {
            item->setIcon(Icons::Get("database"));
        }
        item->setData(ProgramIdRole, QVariant::fromValue<qulonglong>(id));
        item->setToolTip(ProgramIdText(id));
        game_list->addItem(item);
        if (id == select_program_id) {
            select_row = game_list->count() - 1;
        }
    }
    game_list->blockSignals(false);
    game_list->setCurrentRow(select_row);
    ShowSelectedGame();
    QApplication::restoreOverrideCursor();
}

void ShaderManagerDialog::ShowSelectedGame() {
    file_tree->clear();
    const u64 id = SelectedProgramId();
    if (id == 0) {
        title_label->setText(tr("No shader caches"));
        summary_label->setText(tr("Play a game, or import a cache from its context menu in the "
                                  "game list, and it shows up here."));
        icon_label->clear();
        UpdateButtons();
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const ShaderManagerGame* game = FindGame(id);
    title_label->setText(GameTitle(id));
    icon_label->setPixmap(game != nullptr && !game->icon.isNull()
                              ? game->icon.scaled(64, 64, Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation)
                              : Icons::Get("database").pixmap(48, 48));

    const fs::path dir = GameDir(id);
    std::error_code error;
    auto* own = new QTreeWidgetItem(file_tree, {tr("This emulator")});
    own->setFirstColumnSpanned(true);
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (entry.is_regular_file(error)) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files, [](const fs::path& a, const fs::path& b) {
        if (IsOwnFile(a) != IsOwnFile(b)) {
            return IsOwnFile(a);
        }
        return a.filename() < b.filename();
    });
    u64 left_over = 0;
    for (const fs::path& file : files) {
        AddFileItem(own, file, ToQString(file.filename()), true);
        if (!IsOwnFile(file)) {
            left_over += fs::file_size(file, error);
        }
    }
    if (files.empty()) {
        auto* none = new QTreeWidgetItem(own, {tr("No cache yet")});
        none->setFirstColumnSpanned(true);
    }

    const std::vector<OtherCache> others = FindOtherCaches(id);
    if (!others.empty()) {
        auto* group = new QTreeWidgetItem(file_tree, {tr("Other emulators")});
        group->setFirstColumnSpanned(true);
        for (const OtherCache& other : others) {
            AddFileItem(group, other.file, other.emulator, false);
        }
    }
    file_tree->expandAll();

    const fs::path main_file = dir / "vulkan.bin";
    const fs::path log_file = dir / CacheLog::FileName;
    QString summary;
    if (CacheLog::Enabled() && CacheLog::HasLog(log_file)) {
        const CacheFile::Contents contents = CacheFile::InspectLog(log_file);
        summary = tr("%1 graphics, %2 compute and %3 task/mesh pipelines cached in the shader "
                     "cache log (cache version %4).")
                      .arg(contents.graphics)
                      .arg(contents.compute)
                      .arg(contents.mesh)
                      .arg(CacheFile::CurrentVersion);
    } else if (CacheFile::ReadVersion(main_file) == CacheFile::CurrentVersion) {
        const CacheFile::Contents contents = CacheFile::Inspect(main_file);
        const CacheFile::Contents mesh = CacheFile::Inspect(dir / "vulkan_mesh.bin");
        summary = tr("%1 graphics, %2 compute and %3 task/mesh pipelines cached (cache version "
                     "%4).")
                      .arg(contents.graphics)
                      .arg(contents.compute)
                      .arg(mesh.mesh + contents.mesh)
                      .arg(CacheFile::CurrentVersion);
    } else {
        summary = tr("No cache this build can use yet: play the game, or import one.");
    }
    if (left_over != 0) {
        summary += QLatin1Char(' ') +
                   tr("%1 of files this build can't use; Clean Up deletes them.")
                       .arg(DataSize(left_over));
    }
    summary_label->setText(QStringLiteral("%1   %2\n%3").arg(ProgramIdText(id),
                                                         ToQString(dir), summary));
    QApplication::restoreOverrideCursor();
    UpdateButtons();
}

void ShaderManagerDialog::UpdateButtons() {
    const u64 id = SelectedProgramId();
    std::error_code error;
    const fs::path dir = GameDir(id);
    const bool has_dir = id != 0 && fs::is_directory(dir, error);
    const bool has_cache =
        has_dir && (CacheFile::ReadVersion(dir / "vulkan.bin") == CacheFile::CurrentVersion ||
                    (CacheLog::Enabled() && CacheLog::HasLog(dir / CacheLog::FileName)));
    bool has_left_over = false;
    if (has_dir) {
        for (const auto& entry : fs::directory_iterator(dir, error)) {
            has_left_over |= entry.is_regular_file(error) && !IsOwnFile(entry.path());
        }
    }
    const ShaderManagerGame* game = FindGame(id);
    const bool can_change = !emulation_running && id != 0;
    import_button->setEnabled(can_change);
    export_button->setEnabled(has_cache);
    prepare_button->setEnabled(can_change && has_cache && game != nullptr &&
                               !game->path.isEmpty());
    clean_button->setEnabled(can_change && has_left_over);
    delete_button->setEnabled(can_change && has_dir);
    folder_button->setEnabled(has_dir);

    const auto selected = file_tree->selectedItems();
    const bool importable_selected =
        !selected.isEmpty() && selected.front()->data(0, ImportableRole).toBool();
    import_button->setText(importable_selected ? tr("Import Selected") : tr("Import..."));
    if (emulation_running) {
        hint_label->setText(tr("Stop emulation to import, clean up or delete shader caches."));
    } else if (game == nullptr && id != 0) {
        hint_label->setText(tr("This game isn't in the game list, so Prepare Shaders can't "
                               "start it."));
    } else {
        hint_label->clear();
    }
}

void ShaderManagerDialog::Import() {
    const auto selected = file_tree->selectedItems();
    if (!selected.isEmpty() && selected.front()->data(0, ImportableRole).toBool()) {
        ImportFile(ToPath(selected.front()->data(0, PathRole).toString()));
        return;
    }
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Import Shader Cache"), QString{},
        tr("Shader caches (*.bin);;All files (*)"));
    if (!file.isEmpty()) {
        ImportFile(ToPath(file));
    }
}

void ShaderManagerDialog::ImportFile(const fs::path& file) {
    const u64 id = SelectedProgramId();
    if (id == 0) {
        return;
    }
    const std::optional<u32> version = CacheFile::ReadVersion(file);
    if (!version) {
        QMessageBox::warning(this, tr("Import Shader Cache"),
                             tr("This file isn't a shader cache."));
        return;
    }
    if (!CacheFile::IsImportable(*version)) {
        QMessageBox::information(
            this, tr("Import Shader Cache"),
            tr("This cache is version %1, and this build reads version %2 and Lichii's %3 to "
               "%4.\n\nEach build and emulator records its shaders in its own way, so a cache of "
               "another version can't be converted: the game builds its shaders again as you "
               "play. Nothing was changed.")
                .arg(*version)
                .arg(CacheFile::CurrentVersion)
                .arg(CacheFile::LichiiVersions.front())
                .arg(CacheFile::LichiiVersions.back()));
        return;
    }
    static const QRegularExpression id_pattern{QStringLiteral("[0-9a-fA-F]{16}")};
    const QString path_text = ToQString(file);
    bool names_other_game = false;
    for (auto it = id_pattern.globalMatch(path_text); it.hasNext();) {
        const QString found = it.next().captured(0);
        if (found.compare(ProgramIdText(id), Qt::CaseInsensitive) == 0) {
            names_other_game = false;
            break;
        }
        names_other_game = true;
    }
    if (names_other_game &&
        QMessageBox::question(this, tr("Import Shader Cache"),
                              tr("The file's path names another game than %1 (%2). Its "
                                 "shaders won't be used by this game. Import anyway?")
                                  .arg(GameTitle(id), ProgramIdText(id))) != QMessageBox::Yes) {
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const fs::path dir = GameDir(id);
    const bool into_log = CacheLog::Enabled();
    const auto merge = [&dir, into_log](const fs::path& source) {
        return into_log ? CacheFile::MergeIntoLog(source, dir) : CacheFile::Merge(source, dir);
    };
    CacheFile::MergeResult result = merge(file);
    std::error_code error;
    const fs::path mesh_file = file.parent_path() / "vulkan_mesh.bin";
    if (result.error.empty() && file.filename() == "vulkan.bin" &&
        fs::is_regular_file(mesh_file, error)) {
        const CacheFile::MergeResult mesh = merge(mesh_file);
        result.added += mesh.added;
        result.duplicates += mesh.duplicates;
        result.source_damaged |= mesh.source_damaged;
    }
    QApplication::restoreOverrideCursor();
    Populate(id);

    if (!result.error.empty() && result.added == 0) {
        QMessageBox::warning(this, tr("Import Shader Cache"),
                             tr("Nothing was imported: %1.")
                                 .arg(QString::fromStdString(result.error)));
        return;
    }
    QString text = tr("Added %n pipeline(s)", "", static_cast<int>(result.added));
    if (result.duplicates != 0) {
        text += QLatin1Char(' ') + tr("(%n already in the cache)", "",
                                      static_cast<int>(result.duplicates));
    }
    text += QLatin1Char('.');
    if (result.source_damaged) {
        text += QLatin1Char(' ') + tr("The file is damaged past them; the rest was left out.");
    }
    const ShaderManagerGame* game = FindGame(id);
    if (result.added == 0 || game == nullptr || game->path.isEmpty() || emulation_running) {
        QMessageBox::information(this, tr("Import Shader Cache"), text);
        return;
    }
    text += QStringLiteral("\n\n") +
            tr("Prepare the shaders now? The game builds every pipeline once and stops, so it "
               "doesn't compile them while you play.");
    if (QMessageBox::question(this, tr("Import Shader Cache"), text) == QMessageBox::Yes) {
        PrepareShaders();
    }
}

void ShaderManagerDialog::Export() {
    const u64 id = SelectedProgramId();
    const QString target_root =
        QFileDialog::getExistingDirectory(this, tr("Export Shader Cache To"));
    if (target_root.isEmpty()) {
        return;
    }
    QString name = tr("%1 [%2] Nemyu shaders v%3")
                       .arg(GameTitle(id), ProgramIdText(id))
                       .arg(CacheFile::CurrentVersion);
    static const QRegularExpression invalid{QStringLiteral("[<>:\"/\\\\|?*]")};
    name.replace(invalid, QStringLiteral("_"));
    const fs::path target = ToPath(target_root) / ToPath(name);
    std::error_code error;
    fs::create_directories(target, error);
    if (CacheLog::Enabled()) {
        // The log is written out as the files every build imports.
        const CacheFile::ExportResult result = CacheFile::ExportLog(GameDir(id), target);
        if (!result.error.empty()) {
            QMessageBox::warning(this, tr("Export Shader Cache"),
                                 tr("Couldn't export the cache: %1")
                                     .arg(QString::fromStdString(result.error)));
            return;
        }
    } else {
        for (const char* file : {"vulkan.bin", "vulkan_mesh.bin", "vulkan_used.bin"}) {
            const fs::path source = GameDir(id) / file;
            if (!fs::is_regular_file(source, error)) {
                continue;
            }
            fs::copy_file(source, target / file, fs::copy_options::overwrite_existing, error);
            if (error) {
                QMessageBox::warning(this, tr("Export Shader Cache"),
                                     tr("Couldn't copy %1: %2")
                                         .arg(QString::fromLatin1(file),
                                              QString::fromStdString(error.message())));
                return;
            }
        }
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(ToQString(target)));
    QMessageBox::information(this, tr("Export Shader Cache"),
                             tr("Exported to %1.\n\nTo use it, open the Shader Manager on the "
                                "other computer and import vulkan.bin from that folder.")
                                 .arg(ToQString(target)));
}

void ShaderManagerDialog::CleanUp() {
    const u64 id = SelectedProgramId();
    std::error_code error;
    std::vector<fs::path> left_over;
    u64 bytes = 0;
    QStringList names;
    for (const auto& entry : fs::directory_iterator(GameDir(id), error)) {
        if (entry.is_regular_file(error) && !IsOwnFile(entry.path())) {
            left_over.push_back(entry.path());
            bytes += entry.file_size(error);
            names << ToQString(entry.path().filename());
        }
    }
    if (left_over.empty()) {
        return;
    }
    if (QMessageBox::question(
            this, tr("Clean Up Shader Caches"),
            tr("Delete %1 (%2)?\n\nThis build can't use them. A build of another cache version "
               "would find its own cache among them again, and builds it anew once they're "
               "gone.")
                .arg(names.join(QStringLiteral(", ")), DataSize(bytes))) != QMessageBox::Yes) {
        return;
    }
    for (const fs::path& file : left_over) {
        fs::remove(file, error);
    }
    Populate(id);
}

void ShaderManagerDialog::DeleteCache() {
    const u64 id = SelectedProgramId();
    if (QMessageBox::question(this, tr("Delete Shader Cache"),
                              tr("Delete every shader cache file of %1? The game builds its "
                                 "shaders again as you play.")
                                  .arg(GameTitle(id))) != QMessageBox::Yes) {
        return;
    }
    std::error_code error;
    fs::remove_all(GameDir(id), error);
    if (error) {
        QMessageBox::warning(this, tr("Delete Shader Cache"),
                             tr("Couldn't delete the folder: %1")
                                 .arg(QString::fromStdString(error.message())));
    }
    Populate(id);
}

void ShaderManagerDialog::OpenFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(ToQString(GameDir(SelectedProgramId()))));
}

void ShaderManagerDialog::PrepareShaders() {
    const ShaderManagerGame* game = FindGame(SelectedProgramId());
    if (game == nullptr || game->path.isEmpty()) {
        return;
    }
    emit PrepareShadersRequested(game->path);
    accept();
}

u64 ShaderManagerDialog::SelectedProgramId() const {
    const QListWidgetItem* item = game_list->currentItem();
    return item != nullptr ? item->data(ProgramIdRole).toULongLong() : 0;
}

const ShaderManagerGame* ShaderManagerDialog::FindGame(u64 program_id) const {
    const auto it = std::ranges::find(games, program_id, &ShaderManagerGame::program_id);
    return it != games.end() ? &*it : nullptr;
}

fs::path ShaderManagerDialog::GameDir(u64 program_id) const {
    return ShaderRoot() / ToPath(FolderName(program_id));
}

QString ShaderManagerDialog::GameTitle(u64 program_id) const {
    const ShaderManagerGame* game = FindGame(program_id);
    if (game != nullptr && !game->title.isEmpty()) {
        return game->title;
    }
    return tr("Unknown game (%1)").arg(ProgramIdText(program_id));
}
