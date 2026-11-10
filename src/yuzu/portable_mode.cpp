// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QUrl>

#include "common/common_types.h"
#include "common/fs/fs.h"
#include "common/fs/fs_paths.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "yuzu/portable_mode.h"

namespace fs = std::filesystem;

namespace PortableMode {
namespace {

struct FileCopy {
    fs::path from;
    fs::path to;
    u64 size;
};

struct CopyPlan {
    std::vector<fs::path> dirs;
    std::vector<FileCopy> files;
    u64 bytes{};
    /// Data folders and where they land, the longest first, to rewrite paths kept in the config.
    std::vector<std::pair<fs::path, fs::path>> moves;
};

QString ToQString(const fs::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

bool IsWithin(const fs::path& path, const fs::path& parent) {
    const fs::path relative = path.lexically_normal().lexically_relative(parent.lexically_normal());
    return !relative.empty() && *relative.begin() != "..";
}

fs::path UniqueSibling(const fs::path& path, const std::string& suffix) {
    std::error_code ec;
    fs::path candidate = path;
    candidate += suffix;
    for (int i = 2; fs::exists(candidate, ec); ++i) {
        candidate = path;
        candidate += suffix + "-" + std::to_string(i);
    }
    return candidate;
}

CopyPlan PlanCopy(const fs::path& target) {
    using Common::FS::EdenPath;
    const fs::path root = Common::FS::GetEdenPath(EdenPath::EdenDir);
    std::vector<std::pair<fs::path, fs::path>> sources{{root, target}};
    // Elsewhere than Windows the config and cache folders live apart from the data.
    for (const auto& [dir, name] : {std::pair{EdenPath::ConfigDir, CONFIG_DIR},
                                    std::pair{EdenPath::CacheDir, CACHE_DIR}}) {
        const fs::path& source = Common::FS::GetEdenPath(dir);
        if (!IsWithin(source, root)) {
            sources.emplace_back(source, target / name);
        }
    }

    CopyPlan plan;
    for (const auto& [source, destination] : sources) {
        plan.moves.emplace_back(source, destination);
        std::error_code ec;
        if (!fs::is_directory(source, ec)) {
            continue;
        }
        plan.dirs.push_back(destination);
        fs::recursive_directory_iterator it{source, fs::directory_options::skip_permission_denied,
                                            ec};
        for (; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
            const fs::path& path = it->path();
            const fs::path relative = path.lexically_relative(source);
            // The log is in use and rebuilt every session.
            if (source == root && it.depth() == 0 && path.filename() == LOG_DIR) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_directory(ec)) {
                plan.dirs.push_back(destination / relative);
            } else if (it->is_regular_file(ec)) {
                const u64 size = it->file_size(ec);
                plan.files.push_back({path, destination / relative, ec ? 0 : size});
                plan.bytes += ec ? 0 : size;
            }
        }
    }
    std::ranges::sort(plan.moves, [](const auto& a, const auto& b) {
        return a.first.native().size() > b.first.native().size();
    });
    return plan;
}

/// Points the folders kept in the copied config files (NAND, SD, save, screenshot, ...) at the
/// new location; QSettings files keep Windows paths with doubled backslashes.
void RewriteConfigPaths(const fs::path& config_dir,
                        const std::vector<std::pair<fs::path, fs::path>>& moves) {
    std::vector<std::pair<std::string, std::string>> replacements;
    for (const auto& [from, to] : moves) {
        const std::string old_path = Common::FS::PathToUTF8String(from.lexically_normal());
        const std::string new_path = Common::FS::PathToUTF8String(to.lexically_normal());
        std::string old_generic = old_path;
        std::string new_generic = new_path;
        std::ranges::replace(old_generic, '\\', '/');
        std::ranges::replace(new_generic, '\\', '/');
        std::string old_escaped;
        std::string new_escaped;
        for (const char c : old_path) {
            old_escaped += c == '\\' ? std::string(2, c) : std::string(1, c);
        }
        for (const char c : new_path) {
            new_escaped += c == '\\' ? std::string(2, c) : std::string(1, c);
        }
        replacements.emplace_back(old_escaped, new_escaped);
        replacements.emplace_back(old_path, new_path);
        replacements.emplace_back(old_generic, new_generic);
    }

    std::error_code ec;
    fs::recursive_directory_iterator it{config_dir, ec};
    for (; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".ini") {
            continue;
        }
        std::ifstream in{it->path(), std::ios::binary};
        std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        in.close();
        bool changed = false;
        for (const auto& [from, to] : replacements) {
            if (from == to) {
                continue;
            }
            for (size_t pos = text.find(from); pos != std::string::npos;
                 pos = text.find(from, pos + to.size())) {
                text.replace(pos, from.size(), to);
                changed = true;
            }
        }
        if (changed) {
            std::ofstream out{it->path(), std::ios::binary | std::ios::trunc};
            out << text;
        }
    }
}

/// Copies the plan with a progress dialog; returns an error message, empty on success.
QString CopyWithProgress(QWidget* parent, const CopyPlan& plan, bool& cancelled) {
    std::atomic<u64> copied{};
    std::atomic<bool> cancel{};
    std::atomic<bool> finished{};
    std::string error;
    std::jthread worker{[&] {
        std::error_code ec;
        for (const fs::path& dir : plan.dirs) {
            if (!fs::create_directories(dir, ec) && ec) {
                error = Common::FS::PathToUTF8String(dir) + ": " + ec.message();
                finished = true;
                return;
            }
        }
        for (const FileCopy& file : plan.files) {
            if (cancel) {
                break;
            }
            if (!fs::copy_file(file.from, file.to, fs::copy_options::overwrite_existing, ec)) {
                error = Common::FS::PathToUTF8String(file.from) + ": " + ec.message();
                break;
            }
            copied += file.size;
        }
        finished = true;
    }};

    QProgressDialog progress(QObject::tr("Copying Nemyu's data to the portable folder..."),
                             QObject::tr("Cancel"), 0, 1000, parent);
    progress.setWindowTitle(QObject::tr("Portable Mode"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    const QLocale locale;
    while (!finished) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (progress.wasCanceled()) {
            cancel = true;
        }
        const u64 done = copied;
        progress.setValue(plan.bytes == 0 ? 0 : static_cast<int>(done * 1000 / plan.bytes));
        progress.setLabelText(QObject::tr("Copying Nemyu's data to the portable folder...\n%1 of %2")
                                  .arg(locale.formattedDataSize(static_cast<qint64>(done)),
                                       locale.formattedDataSize(static_cast<qint64>(plan.bytes))));
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    worker.join();
    cancelled = cancel;
    return QString::fromStdString(error);
}

std::optional<Restart> Enable(QWidget* parent, const fs::path& target) {
    std::error_code ec;
    if (fs::exists(target, ec)) {
        if (!fs::is_directory(target, ec)) {
            QMessageBox::warning(parent, QObject::tr("Portable Mode"),
                                 QObject::tr("A file named \"%1\" is in the way of the portable "
                                             "folder. Remove or rename it and try again.")
                                     .arg(ToQString(target)));
            return std::nullopt;
        }
        const auto answer = QMessageBox::question(
            parent, QObject::tr("Portable Mode"),
            QObject::tr("The portable folder already exists:\n%1\n\nRestart Nemyu to use it?")
                .arg(ToQString(target)));
        return answer == QMessageBox::Yes ? std::optional<Restart>{Restart{}} : std::nullopt;
    }

    const fs::path previous = fs::path{target} += ".disabled";
    const bool has_previous = fs::is_directory(previous, ec);
    const CopyPlan plan = PlanCopy(target);
    const QLocale locale;

    QMessageBox box(QMessageBox::Question, QObject::tr("Portable Mode"),
                    QObject::tr("Portable mode keeps all of Nemyu's data (settings, keys, saves, "
                                "shader caches) in this folder instead of %1:\n%2\n\n"
                                "Nemyu and its data can then be moved or backed up as one folder. "
                                "Nemyu restarts to apply it.")
                        .arg(ToQString(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir)),
                             ToQString(target)),
                    QMessageBox::Cancel, parent);
    QPushButton* copy = box.addButton(
        QObject::tr("Copy my data (%1)").arg(locale.formattedDataSize(static_cast<qint64>(plan.bytes))),
        QMessageBox::AcceptRole);
    QPushButton* reuse = has_previous ? box.addButton(QObject::tr("Use the previous portable data"),
                                                      QMessageBox::AcceptRole)
                                      : nullptr;
    QPushButton* fresh = box.addButton(QObject::tr("Start empty"), QMessageBox::AcceptRole);
    box.setDefaultButton(copy);
    box.exec();

    if (box.clickedButton() == reuse && reuse != nullptr) {
        return Restart{previous, target};
    }
    if (box.clickedButton() == fresh) {
        if (!fs::create_directories(target, ec)) {
            QMessageBox::warning(parent, QObject::tr("Portable Mode"),
                                 QObject::tr("Could not create %1:\n%2")
                                     .arg(ToQString(target), QString::fromStdString(ec.message())));
            return std::nullopt;
        }
        return Restart{};
    }
    if (box.clickedButton() != copy) {
        return std::nullopt;
    }

    const auto space = fs::space(target.parent_path(), ec);
    constexpr u64 headroom = 512ULL << 20;
    if (!ec && space.available < plan.bytes + headroom) {
        QMessageBox::warning(
            parent, QObject::tr("Portable Mode"),
            QObject::tr("Not enough free space: the copy needs %1 and the drive has %2 free.")
                .arg(locale.formattedDataSize(static_cast<qint64>(plan.bytes)),
                     locale.formattedDataSize(static_cast<qint64>(space.available))));
        return std::nullopt;
    }

    bool cancelled = false;
    const QString error = CopyWithProgress(parent, plan, cancelled);
    if (cancelled || !error.isEmpty()) {
        fs::remove_all(target, ec);
        if (!error.isEmpty()) {
            QMessageBox::warning(parent, QObject::tr("Portable Mode"),
                                 QObject::tr("The copy failed, nothing was changed:\n%1").arg(error));
        }
        return std::nullopt;
    }
    RewriteConfigPaths(target / CONFIG_DIR, plan.moves);
    return Restart{};
}

std::optional<Restart> Disable(QWidget* parent, const fs::path& target) {
    const fs::path previous = UniqueSibling(target, ".disabled");
    QMessageBox box(
        QMessageBox::Question, QObject::tr("Portable Mode"),
        QObject::tr("Nemyu is in portable mode and keeps its data in:\n%1\n\n"
                    "Turning it off renames that folder to \"%2\", where your data stays, and "
                    "restarts Nemyu with its usual data folder. Turning it on again later can "
                    "bring that data back.")
            .arg(ToQString(target), ToQString(previous.filename())),
        QMessageBox::Cancel, parent);
    QPushButton* disable =
        box.addButton(QObject::tr("Turn off and restart"), QMessageBox::AcceptRole);
    QPushButton* open = box.addButton(QObject::tr("Open folder"), QMessageBox::ActionRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == open) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(ToQString(target)));
        return std::nullopt;
    }
    if (box.clickedButton() != disable) {
        return std::nullopt;
    }
    return Restart{target, previous};
}

} // namespace

fs::path PortableDir() {
#ifdef _WIN32
    return Common::FS::GetExeDirectory() / PORTABLE_DIR;
#else
    return Common::FS::GetCurrentDir() / PORTABLE_DIR;
#endif
}

bool IsActive() {
    std::error_code ec;
    return fs::equivalent(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir), PortableDir(),
                          ec);
}

std::optional<Restart> Configure(QWidget* parent) {
    return IsActive() ? Disable(parent, PortableDir()) : Enable(parent, PortableDir());
}

bool LaunchRestart(const Restart& restart) {
    const QString pid = QString::number(QCoreApplication::applicationPid());
#ifdef _WIN32
    const auto quote = [](const QString& text) {
        return QStringLiteral("'") + QString{text}.replace(QStringLiteral("'"), QStringLiteral("''")) +
               QStringLiteral("'");
    };
    const QString exe = QCoreApplication::applicationFilePath();
    QString script = QStringLiteral("Wait-Process -Id %1 -Timeout 60 -ErrorAction SilentlyContinue\n")
                         .arg(pid);
    if (!restart.from.empty()) {
        script += QStringLiteral("$from = %1\n"
                                 "for ($i = 0; $i -lt 40 -and (Test-Path -LiteralPath $from); $i++) {\n"
                                 "  Move-Item -LiteralPath $from -Destination %2 -ErrorAction SilentlyContinue\n"
                                 "  if (Test-Path -LiteralPath $from) { Start-Sleep -Milliseconds 250 }\n"
                                 "}\n")
                      .arg(quote(ToQString(restart.from)), quote(ToQString(restart.to)));
    }
    script += QStringLiteral("Start-Process -FilePath %1 -WorkingDirectory %2")
                  .arg(quote(exe), quote(QCoreApplication::applicationDirPath()));
    if (!restart.game.empty()) {
        // Start-Process joins its arguments unquoted; a Windows path can't hold a double quote
        script += QStringLiteral(" -ArgumentList %1")
                      .arg(quote(QStringLiteral("-g \"%1\"").arg(ToQString(restart.game))));
    }
    script += QLatin1Char('\n');
    const QByteArray encoded =
        QByteArray(reinterpret_cast<const char*>(script.utf16()), script.size() * 2).toBase64();
    return QProcess::startDetached(
        QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
         QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
         QStringLiteral("-EncodedCommand"), QString::fromLatin1(encoded)});
#else
    // An AppImage's own path disappears with its mount when this process exits.
    const QString exe =
        qEnvironmentVariable("APPIMAGE", QCoreApplication::applicationFilePath());
    const QString script = QStringLiteral(
        "while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; "
        "if [ -n \"$2\" ]; then mv -- \"$2\" \"$3\"; fi; "
        "if [ -n \"$5\" ]; then exec \"$4\" -g \"$5\"; fi; exec \"$4\"");
    return QProcess::startDetached(QStringLiteral("sh"),
                                   {QStringLiteral("-c"), script, QStringLiteral("sh"), pid,
                                    ToQString(restart.from), ToQString(restart.to), exe,
                                    ToQString(restart.game)},
                                   ToQString(Common::FS::GetCurrentDir()));
#endif
}

} // namespace PortableMode
