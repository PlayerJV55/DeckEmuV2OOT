// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/save_manager_dialog.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <system_error>

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/fs/ryujinx_compat.h"
#include "common/fs/symlink.h"
#include "common/string_util.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/hle/service/acc/profile_manager.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/zip.h"
#include "yuzu/util/icons.h"
#include "yuzu/util/other_emulators.h"
#include "yuzu/util/util.h"

namespace fs = std::filesystem;

namespace {

constexpr int PathRole = Qt::UserRole;
constexpr int KindRole = Qt::UserRole + 1;
constexpr int UserRole = Qt::UserRole + 2;
constexpr int ProgramIdRole = Qt::UserRole + 3;

enum class Kind { None, Own, Backup, Other };

// Automatic backups kept per game; manual ones stay until deleted.
constexpr std::size_t KeptAutomaticBackups = 10;

QString ToQString(const fs::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

fs::path ToPath(const QString& text) {
    return fs::path{text.toStdU16String()};
}

QString ProgramIdText(u64 program_id) {
    return QString::fromStdString(fmt::format("{:016X}", program_id));
}

QString DataSize(u64 bytes) {
    return QLocale().formattedDataSize(static_cast<qint64>(bytes));
}

QString DeviceUser() {
    return QString(32, QLatin1Char('0'));
}

bool IsHex(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    });
}

fs::path SaveRoot() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir) / "user" / "save" /
           "0000000000000000";
}

fs::path BackupRoot(u64 program_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "backups" / "saves" /
           ToPath(ProgramIdText(program_id));
}

struct Stats {
    u64 bytes{};
    u64 files{};
    QDateTime modified;
};

Stats FolderStats(const fs::path& dir) {
    Stats stats;
    std::error_code error;
    for (auto it = fs::recursive_directory_iterator(dir, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file(error)) {
            continue;
        }
        stats.bytes += it->file_size(error);
        ++stats.files;
        const QDateTime modified = QFileInfo(ToQString(it->path())).lastModified();
        if (!stats.modified.isValid() || modified > stats.modified) {
            stats.modified = modified;
        }
    }
    return stats;
}

struct Save {
    QString user;
    fs::path path;
};

// Saves of the game under a save root laid out like this emulator's: <user>/<program id>.
std::vector<Save> SavesIn(const fs::path& root, u64 program_id) {
    std::vector<Save> saves;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(root, error)) {
        const std::string user = Common::FS::PathToUTF8String(entry.path().filename());
        if (user.size() != 32 || !IsHex(user) || !entry.is_directory(error)) {
            continue;
        }
        for (const std::string& name :
             {fmt::format("{:016X}", program_id), fmt::format("{:016x}", program_id)}) {
            const fs::path path = entry.path() / name;
            if (fs::is_directory(path, error) && FolderStats(path).files != 0) {
                saves.push_back({QString::fromStdString(user).toUpper(), path});
                break;
            }
        }
    }
    return saves;
}

std::vector<u64> TitlesWithSaves() {
    std::vector<u64> ids;
    std::error_code error;
    for (const auto& user : fs::directory_iterator(SaveRoot(), error)) {
        if (!user.is_directory(error)) {
            continue;
        }
        for (const auto& title : fs::directory_iterator(user.path(), error)) {
            const std::string name = Common::FS::PathToUTF8String(title.path().filename());
            if (name.size() == 16 && IsHex(name) && title.is_directory(error) &&
                FolderStats(title.path()).files != 0) {
                ids.push_back(std::stoull(name, nullptr, 16));
            }
        }
    }
    return ids;
}

struct OtherSave {
    QString emulator;
    Save save;
};

std::vector<OtherSave> FindOtherSaves(u64 program_id) {
    std::vector<OtherSave> found;
    for (const OtherEmulators::Folder& root : OtherEmulators::Equivalents(SaveRoot())) {
        for (Save& save : SavesIn(root.path, program_id)) {
            found.push_back({root.emulator, std::move(save)});
        }
    }
    std::vector<Common::FS::IMEN> imens;
    if (Common::FS::ReadKvdb(Common::FS::GetKvdbPath(), imens) ==
        Common::FS::IMENReadResult::Success) {
        for (const Common::FS::IMEN& imen : imens) {
            const fs::path path = Common::FS::GetRyuSavePath(imen.save_id);
            if (imen.title_id == program_id && FolderStats(path).files != 0) {
                found.push_back({QStringLiteral("Ryujinx"), {QString{}, path}});
            }
        }
    }
    return found;
}

std::optional<QString> LocalProfileName(const QString& user) {
    if (user == DeviceUser()) {
        return QObject::tr("Device save (every profile)");
    }
    auto& manager = QtCommon::system->GetProfileManager();
    for (const Common::UUID& uuid : manager.FindExistingProfileUUIDs()) {
        const u128 id = uuid.AsU128();
        if (QString::fromStdString(fmt::format("{:016X}{:016X}", id[1], id[0])) != user) {
            continue;
        }
        Service::Account::ProfileBase base{};
        if (!manager.GetProfileBase(uuid, base)) {
            return std::nullopt;
        }
        return QString::fromStdString(Common::StringFromFixedZeroTerminatedBuffer(
            reinterpret_cast<const char*>(base.username.data()), base.username.size()));
    }
    return std::nullopt;
}

QString ProfileName(const QString& user) {
    if (user.isEmpty()) {
        return QObject::tr("Profile unknown");
    }
    return LocalProfileName(user).value_or(QObject::tr("Profile %1").arg(user.left(8)));
}

struct Backup {
    fs::path file;
    QString user;
    QDateTime time;
    bool automatic{};
};

std::vector<Backup> FindBackups(u64 program_id) {
    static const QRegularExpression pattern{QStringLiteral(
        "^(\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2})_([0-9A-F]{32})(_auto)?\\.zip$")};
    std::vector<Backup> backups;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(BackupRoot(program_id), error)) {
        const auto match = pattern.match(ToQString(entry.path().filename()));
        if (!entry.is_regular_file(error) || !match.hasMatch()) {
            continue;
        }
        backups.push_back({
            .file = entry.path(),
            .user = match.captured(2),
            .time = QDateTime::fromString(match.captured(1), QStringLiteral("yyyy-MM-dd_HH-mm-ss")),
            .automatic = match.hasCaptured(3),
        });
    }
    std::ranges::sort(backups, [](const Backup& a, const Backup& b) { return a.time > b.time; });
    return backups;
}

std::optional<fs::path> MakeBackup(u64 program_id, const QString& user, const fs::path& source,
                                   bool automatic) {
    const fs::path dir = BackupRoot(program_id);
    std::error_code error;
    fs::create_directories(dir, error);
    const QString name =
        QStringLiteral("%1_%2%3.zip")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")),
                 user, automatic ? QStringLiteral("_auto") : QString{});
    const fs::path file = dir / ToPath(name);
    if (!QtCommon::Zip::CompressDir(ToQString(file), ToQString(source))) {
        fs::remove(file, error);
        return std::nullopt;
    }
    if (automatic) {
        std::size_t kept = 0;
        for (const Backup& backup : FindBackups(program_id)) {
            if (backup.automatic && ++kept > KeptAutomaticBackups) {
                fs::remove(backup.file, error);
            }
        }
    }
    return file;
}

QTreeWidgetItem* AddItem(QTreeWidgetItem* parent, Kind kind, const fs::path& path,
                         const QString& user, const QString& label, const QString& details,
                         u64 bytes, const QDateTime& modified) {
    auto* item = new QTreeWidgetItem(
        parent, {label, details, DataSize(bytes), QLocale().toString(modified, QLocale::ShortFormat)});
    item->setData(0, PathRole, ToQString(path));
    item->setData(0, KindRole, static_cast<int>(kind));
    item->setData(0, UserRole, user);
    item->setToolTip(0, ToQString(path));
    item->setToolTip(1, details);
    return item;
}

} // Anonymous namespace

SaveManagerDialog::SaveManagerDialog(std::vector<ShaderManagerGame> games_,
                                     u64 selected_program_id, u64 running_program_id_,
                                     QWidget* parent)
    : QDialog(parent), games{std::move(games_)}, running_program_id{running_program_id_} {
    setWindowTitle(tr("Save Manager"));
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

    save_tree = new QTreeWidget(this);
    save_tree->setColumnCount(4);
    save_tree->setHeaderLabels({tr("Save"), tr("Details"), tr("Size"), tr("Modified")});
    save_tree->setRootIsDecorated(true);
    save_tree->setUniformRowHeights(true);
    save_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    save_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    save_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    save_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    hint_label = new QLabel(this);
    hint_label->setWordWrap(true);

    const auto make_button = [this](const char* icon, const QString& text,
                                    const QString& tooltip) {
        auto* button = new QPushButton(Icons::Get(icon), text, this);
        button->setToolTip(tooltip);
        button->setMinimumHeight(30);
        return button;
    };
    backup_button = make_button("device-floppy", tr("Back Up"),
                                tr("Keeps a copy of the selected save under Backups."));
    import_button = make_button(
        "download", tr("Import..."),
        tr("Copies a save from a backup, another emulator, a zip file or a folder over this "
           "game's save. The save it replaces is backed up first."));
    export_button = make_button("file-export", tr("Export..."),
                                tr("Saves the selected save as a zip file, to keep or share."));
    delete_button = make_button("trash", tr("Delete..."),
                                tr("Deletes the selected save (backed up first) or backup."));
    folder_button = make_button("folder-open", tr("Open Folder"),
                                tr("Opens the folder of the selected save or backup."));

    auto* actions = new QGridLayout;
    actions->setHorizontalSpacing(8);
    actions->setVerticalSpacing(6);
    actions->addWidget(backup_button, 0, 0);
    actions->addWidget(import_button, 0, 1);
    actions->addWidget(export_button, 0, 2);
    actions->addWidget(folder_button, 1, 0);
    actions->addWidget(delete_button, 1, 1);
    for (int column = 0; column < 3; ++column) {
        actions->setColumnStretch(column, 1);
    }

    auto* details = new QWidget(this);
    auto* details_layout = new QVBoxLayout(details);
    details_layout->setContentsMargins(0, 0, 0, 0);
    details_layout->addLayout(header);
    details_layout->addWidget(save_tree, 1);
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
    connect(save_tree, &QTreeWidget::itemSelectionChanged, this, [this] { UpdateButtons(); });
    connect(backup_button, &QPushButton::clicked, this, &SaveManagerDialog::BackUp);
    connect(import_button, &QPushButton::clicked, this, &SaveManagerDialog::Import);
    connect(export_button, &QPushButton::clicked, this, &SaveManagerDialog::Export);
    connect(delete_button, &QPushButton::clicked, this, &SaveManagerDialog::Delete);
    connect(folder_button, &QPushButton::clicked, this, &SaveManagerDialog::OpenFolder);

    Populate(selected_program_id);
}

SaveManagerDialog::~SaveManagerDialog() = default;

void SaveManagerDialog::Populate(u64 select_program_id) {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<u64> ids = TitlesWithSaves();
    for (const ShaderManagerGame& game : games) {
        ids.push_back(game.program_id);
    }
    if (select_program_id != 0) {
        ids.push_back(select_program_id);
    }
    std::ranges::sort(ids);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::erase(ids, u64{0});
    std::ranges::sort(ids, [this](u64 a, u64 b) {
        return GameTitle(a).localeAwareCompare(GameTitle(b)) < 0;
    });

    game_list->blockSignals(true);
    game_list->clear();
    int select_row = ids.empty() ? -1 : 0;
    for (const u64 id : ids) {
        u64 size = 0;
        for (const Save& save : SavesIn(SaveRoot(), id)) {
            size += FolderStats(save.path).bytes;
        }
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(
            GameTitle(id), size != 0 ? DataSize(size) : tr("No save data")));
        const ShaderManagerGame* game = FindGame(id);
        if (game != nullptr && !game->icon.isNull()) {
            item->setIcon(QIcon(game->icon));
        } else {
            item->setIcon(Icons::Get("device-floppy"));
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

void SaveManagerDialog::ShowSelectedGame() {
    save_tree->clear();
    const u64 id = SelectedProgramId();
    if (id == 0) {
        title_label->setText(tr("No games"));
        summary_label->setText(tr("Add a game folder to the game list, and its saves show up "
                                  "here."));
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
                              : Icons::Get("device-floppy").pixmap(48, 48));

    auto* own = new QTreeWidgetItem(save_tree, {tr("This emulator")});
    own->setFirstColumnSpanned(true);
    const std::vector<Save> saves = SavesIn(SaveRoot(), id);
    u64 total = 0;
    QTreeWidgetItem* first_save = nullptr;
    for (const Save& save : saves) {
        const Stats stats = FolderStats(save.path);
        total += stats.bytes;
        const bool linked = Common::FS::IsSymlink(save.path);
        QString details = tr("%n file(s)", "", static_cast<int>(stats.files));
        if (linked) {
            details += QStringLiteral(", ") + tr("linked to Ryujinx");
        }
        auto* item = AddItem(own, Kind::Own, save.path, save.user, ProfileName(save.user),
                             details, stats.bytes, stats.modified);
        if (first_save == nullptr) {
            first_save = item;
        }
    }
    if (saves.empty()) {
        auto* none = new QTreeWidgetItem(own, {tr("No save data yet")});
        none->setFirstColumnSpanned(true);
    }

    const std::vector<Backup> backups = FindBackups(id);
    if (!backups.empty()) {
        auto* group = new QTreeWidgetItem(save_tree, {tr("Backups")});
        group->setFirstColumnSpanned(true);
        for (const Backup& backup : backups) {
            const QString details =
                backup.automatic ? tr("%1, taken before a change").arg(ProfileName(backup.user))
                                 : ProfileName(backup.user);
            std::error_code error;
            AddItem(group, Kind::Backup, backup.file, backup.user,
                    QLocale().toString(backup.time, QLocale::ShortFormat), details,
                    fs::file_size(backup.file, error), backup.time);
        }
    }

    const std::vector<OtherSave> others = FindOtherSaves(id);
    if (!others.empty()) {
        auto* group = new QTreeWidgetItem(save_tree, {tr("Other emulators")});
        group->setFirstColumnSpanned(true);
        for (const OtherSave& other : others) {
            const Stats stats = FolderStats(other.save.path);
            AddItem(group, Kind::Other, other.save.path, other.save.user, other.emulator,
                    tr("%1, %n file(s)", "", static_cast<int>(stats.files))
                        .arg(ProfileName(other.save.user)),
                    stats.bytes, stats.modified);
        }
    }
    save_tree->expandAll();
    if (first_save != nullptr) {
        save_tree->setCurrentItem(first_save);
    }

    QString summary = saves.empty()
                          ? tr("No save data in this emulator yet.")
                          : tr("%1 in %n save(s).", "", static_cast<int>(saves.size()))
                                .arg(DataSize(total));
    if (!others.empty()) {
        summary += QLatin1Char(' ') + tr("Found in %n other emulator folder(s); select one and "
                                         "import it.",
                                         "", static_cast<int>(others.size()));
    }
    summary_label->setText(QStringLiteral("%1   %2\n%3").arg(ProgramIdText(id),
                                                         ToQString(SaveRoot()), summary));
    QApplication::restoreOverrideCursor();
    UpdateButtons();
}

void SaveManagerDialog::UpdateButtons() {
    const u64 id = SelectedProgramId();
    const QTreeWidgetItem* item = SelectedItem();
    const Kind kind = item != nullptr ? static_cast<Kind>(item->data(0, KindRole).toInt())
                                      : Kind::None;
    const bool can_change = id != 0 && id != running_program_id;
    backup_button->setEnabled(can_change && kind == Kind::Own);
    import_button->setEnabled(can_change);
    export_button->setEnabled(kind == Kind::Own);
    delete_button->setEnabled(can_change && (kind == Kind::Own || kind == Kind::Backup));
    folder_button->setEnabled(id != 0);

    switch (kind) {
    case Kind::Backup:
        import_button->setText(tr("Restore"));
        break;
    case Kind::Other:
        import_button->setText(tr("Import Selected"));
        break;
    default:
        import_button->setText(tr("Import..."));
        break;
    }

    if (id != 0 && id == running_program_id) {
        hint_label->setText(tr("Stop the game to change its saves."));
    } else if (kind == Kind::Backup) {
        hint_label->setText(tr("Restore puts this backup back in place of %1's save. The "
                               "current save is backed up first.")
                                .arg(ProfileName(item->data(0, UserRole).toString())));
    } else if (kind == Kind::Other) {
        hint_label->setText(tr("Import copies this save over the one in this emulator. The "
                               "current save is backed up first."));
    } else {
        hint_label->setText(tr("Every import, restore and delete backs up the save it replaces "
                               "first, so Backups can bring it back."));
    }
}

void SaveManagerDialog::BackUp() {
    const QTreeWidgetItem* item = SelectedOwnSave();
    if (item == nullptr) {
        return;
    }
    const u64 id = SelectedProgramId();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto file = MakeBackup(id, item->data(0, UserRole).toString(),
                                 ToPath(item->data(0, PathRole).toString()), false);
    QApplication::restoreOverrideCursor();
    if (!file) {
        QMessageBox::warning(this, tr("Back Up Save"), tr("Couldn't write the backup."));
        return;
    }
    Populate(id);
}

void SaveManagerDialog::Import() {
    const QTreeWidgetItem* item = SelectedItem();
    const Kind kind = item != nullptr ? static_cast<Kind>(item->data(0, KindRole).toInt())
                                      : Kind::None;
    if (kind == Kind::Backup) {
        ImportZip(ToPath(item->data(0, PathRole).toString()), item->data(0, UserRole).toString());
        return;
    }
    if (kind == Kind::Other) {
        ImportFolder(ToPath(item->data(0, PathRole).toString()),
                     item->data(0, UserRole).toString(),
                     tr("the %1 save").arg(item->text(0)));
        return;
    }
    QMenu menu(this);
    QAction* from_zip = menu.addAction(Icons::Get("file-zip"), tr("From a Zip File..."));
    QAction* from_folder = menu.addAction(Icons::Get("folder-open"), tr("From a Folder..."));
    const QAction* chosen = menu.exec(import_button->mapToGlobal(QPoint(0, import_button->height())));
    if (chosen == from_zip) {
        const QString file = QFileDialog::getOpenFileName(this, tr("Import Save"), QString{},
                                                          tr("Zip files (*.zip)"));
        if (!file.isEmpty()) {
            ImportZip(ToPath(file), QString{});
        }
    } else if (chosen == from_folder) {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Import Save From"));
        if (!dir.isEmpty()) {
            ImportFolder(ToPath(dir), QString{}, QDir::toNativeSeparators(dir));
        }
    }
}

std::optional<QString> SaveManagerDialog::TargetUser(const QString& source_user) {
    if (!source_user.isEmpty() &&
        (source_user == DeviceUser() || LocalProfileName(source_user).has_value())) {
        return source_user;
    }
    const std::vector<Save> saves = SavesIn(SaveRoot(), SelectedProgramId());
    if (source_user.isEmpty() && saves.size() == 1 && saves.front().user == DeviceUser()) {
        return DeviceUser();
    }
    const QString user = QString::fromStdString(GetProfileIDString());
    if (user.isEmpty()) {
        return std::nullopt;
    }
    return user;
}

bool SaveManagerDialog::ReplaceSave(const QString& user, const fs::path& source) {
    const u64 id = SelectedProgramId();
    const fs::path target = SaveRoot() / ToPath(user) / ToPath(ProgramIdText(id));
    std::error_code error;
    if (Common::FS::IsSymlink(target)) {
        QMessageBox::warning(this, tr("Save Manager"),
                             tr("This save is linked to Ryujinx. Unlink it from the game list "
                                "first. Nothing was changed."));
        return false;
    }
    if (FolderStats(target).files != 0 && !MakeBackup(id, user, target, true)) {
        QMessageBox::warning(this, tr("Save Manager"),
                             tr("Couldn't back up the current save, so nothing was changed."));
        return false;
    }
    fs::remove_all(target, error);
    if (!error) {
        fs::create_directories(target, error);
    }
    if (!error) {
        fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 error);
    }
    if (error) {
        QMessageBox::warning(this, tr("Save Manager"),
                             tr("Couldn't copy the save: %1. The previous save is under "
                                "Backups.")
                                 .arg(QString::fromStdString(error.message())));
        return false;
    }
    return true;
}

void SaveManagerDialog::ImportFolder(const fs::path& source, const QString& source_user,
                                     const QString& source_name) {
    const u64 id = SelectedProgramId();
    if (FolderStats(source).files == 0) {
        QMessageBox::information(this, tr("Import Save"), tr("That folder has no files."));
        return;
    }
    const std::optional<QString> user = TargetUser(source_user);
    if (!user) {
        return;
    }
    if (QMessageBox::question(this, tr("Import Save"),
                              tr("Replace %1's save of %2 with %3?\n\nThe current save is "
                                 "backed up first.")
                                  .arg(ProfileName(*user), GameTitle(id), source_name)) !=
        QMessageBox::Yes) {
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool replaced = ReplaceSave(*user, source);
    QApplication::restoreOverrideCursor();
    Populate(id);
    if (replaced) {
        QMessageBox::information(this, tr("Import Save"), tr("Save imported."));
    }
}

void SaveManagerDialog::ImportZip(const fs::path& zip, const QString& source_user) {
    const u64 id = SelectedProgramId();
    const std::optional<QString> user = TargetUser(source_user);
    if (!user) {
        return;
    }
    if (QMessageBox::question(this, tr("Import Save"),
                              tr("Replace %1's save of %2 with %3?\n\nThe current save is "
                                 "backed up first.")
                                  .arg(ProfileName(*user), GameTitle(id),
                                       ToQString(zip.filename()))) != QMessageBox::Yes) {
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const fs::path temp = BackupRoot(id) / ".extract";
    std::error_code error;
    fs::remove_all(temp, error);
    fs::create_directories(temp, error);
    const bool extracted = QtCommon::Zip::ExtractDir(ToQString(zip), ToQString(temp));
    // Zips made elsewhere often hold the save inside a folder named after the game or profile,
    // or in Ryujinx's "0".
    fs::path source = temp;
    for (;;) {
        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator(source, error)) {
            entries.push_back(entry);
        }
        if (entries.size() != 1 || !entries.front().is_directory(error)) {
            break;
        }
        const std::string name = Common::FS::PathToUTF8String(entries.front().path().filename());
        if (name != "0" && !((name.size() == 16 || name.size() == 32) && IsHex(name))) {
            break;
        }
        source = entries.front().path();
    }
    const bool replaced = extracted && FolderStats(source).files != 0 && ReplaceSave(*user, source);
    fs::remove_all(temp, error);
    QApplication::restoreOverrideCursor();
    Populate(id);
    if (!extracted) {
        QMessageBox::warning(this, tr("Import Save"),
                             tr("Couldn't read the zip file, or it's empty. Nothing was changed."));
    } else if (replaced) {
        QMessageBox::information(this, tr("Import Save"), tr("Save imported."));
    }
}

void SaveManagerDialog::Export() {
    const QTreeWidgetItem* item = SelectedOwnSave();
    if (item == nullptr) {
        return;
    }
    const u64 id = SelectedProgramId();
    QString name = tr("%1 [%2] %3 save %4.zip")
                       .arg(GameTitle(id), ProgramIdText(id), item->text(0),
                            QDate::currentDate().toString(Qt::ISODate));
    static const QRegularExpression invalid{QStringLiteral("[<>:\"/\\\\|?*]")};
    name.replace(invalid, QStringLiteral("_"));
    const QString file = QFileDialog::getSaveFileName(
        this, tr("Export Save"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(name),
        tr("Zip files (*.zip)"));
    if (file.isEmpty()) {
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool written = QtCommon::Zip::CompressDir(file, item->data(0, PathRole).toString());
    QApplication::restoreOverrideCursor();
    if (!written) {
        QMessageBox::warning(this, tr("Export Save"), tr("Couldn't write %1.").arg(file));
        return;
    }
    QMessageBox::information(this, tr("Export Save"),
                             tr("Exported to %1.\n\nTo use it, open the Save Manager on the "
                                "other computer, pick the game and import this file.")
                                 .arg(QDir::toNativeSeparators(file)));
}

void SaveManagerDialog::Delete() {
    const QTreeWidgetItem* item = SelectedItem();
    if (item == nullptr) {
        return;
    }
    const u64 id = SelectedProgramId();
    const fs::path path = ToPath(item->data(0, PathRole).toString());
    const QString user = item->data(0, UserRole).toString();
    std::error_code error;
    switch (static_cast<Kind>(item->data(0, KindRole).toInt())) {
    case Kind::Own:
        if (Common::FS::IsSymlink(path)) {
            QMessageBox::warning(this, tr("Delete Save"),
                                 tr("This save is linked to Ryujinx. Unlink it from the game "
                                    "list first."));
            return;
        }
        if (QMessageBox::question(this, tr("Delete Save"),
                                  tr("Delete %1's save of %2?\n\nIt is backed up first, so "
                                     "Backups can bring it back.")
                                      .arg(ProfileName(user), GameTitle(id))) !=
            QMessageBox::Yes) {
            return;
        }
        if (!MakeBackup(id, user, path, true)) {
            QMessageBox::warning(this, tr("Delete Save"),
                                 tr("Couldn't back up the save, so it wasn't deleted."));
            return;
        }
        fs::remove_all(path, error);
        break;
    case Kind::Backup:
        if (QMessageBox::question(this, tr("Delete Backup"),
                                  tr("Delete the backup of %1?").arg(item->text(0))) !=
            QMessageBox::Yes) {
            return;
        }
        fs::remove(path, error);
        break;
    default:
        return;
    }
    if (error) {
        QMessageBox::warning(this, tr("Save Manager"),
                             tr("Couldn't delete it: %1")
                                 .arg(QString::fromStdString(error.message())));
    }
    Populate(id);
}

void SaveManagerDialog::OpenFolder() {
    const QTreeWidgetItem* item = SelectedItem();
    fs::path path = SaveRoot();
    if (item != nullptr && static_cast<Kind>(item->data(0, KindRole).toInt()) != Kind::None) {
        path = ToPath(item->data(0, PathRole).toString());
        if (static_cast<Kind>(item->data(0, KindRole).toInt()) == Kind::Backup) {
            path = path.parent_path();
        }
    } else if (const auto saves = SavesIn(SaveRoot(), SelectedProgramId()); !saves.empty()) {
        path = saves.front().path;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(ToQString(path)));
}

const QTreeWidgetItem* SaveManagerDialog::SelectedItem() const {
    const auto selected = save_tree->selectedItems();
    return selected.isEmpty() ? nullptr : selected.front();
}

const QTreeWidgetItem* SaveManagerDialog::SelectedOwnSave() const {
    const QTreeWidgetItem* item = SelectedItem();
    return item != nullptr && static_cast<Kind>(item->data(0, KindRole).toInt()) == Kind::Own
               ? item
               : nullptr;
}

u64 SaveManagerDialog::SelectedProgramId() const {
    const QListWidgetItem* item = game_list->currentItem();
    return item != nullptr ? item->data(ProgramIdRole).toULongLong() : 0;
}

const ShaderManagerGame* SaveManagerDialog::FindGame(u64 program_id) const {
    const auto it = std::ranges::find(games, program_id, &ShaderManagerGame::program_id);
    return it != games.end() ? &*it : nullptr;
}

QString SaveManagerDialog::GameTitle(u64 program_id) const {
    const ShaderManagerGame* game = FindGame(program_id);
    if (game != nullptr && !game->title.isEmpty()) {
        return game->title;
    }
    return tr("Unknown game (%1)").arg(ProgramIdText(program_id));
}
