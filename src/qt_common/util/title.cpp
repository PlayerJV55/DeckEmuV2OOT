// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QObject>

#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/title.h"

namespace QtCommon::Title {

QString LooseExeFSName(const std::string& physical_name) {
    const QFileInfo file_info(QString::fromStdString(physical_name));
    if (file_info.fileName() != QStringLiteral("main")) {
        return {};
    }
    QDir dir = file_info.dir();
    if (dir.dirName().compare(QStringLiteral("exefs"), Qt::CaseInsensitive) != 0 || !dir.cdUp()) {
        return {};
    }

    // Storage levels sit between the game folder and its content directories.
    const auto is_storage_level = [](const QString& name) {
        if (name.compare(QStringLiteral("base"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("update"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("updates"), Qt::CaseInsensitive) == 0) {
            return true;
        }
        return !name.isEmpty() && std::all_of(name.cbegin(), name.cend(), [](QChar c) {
            return c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
                   (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
        });
    };

    bool is_update = false;
    while (is_storage_level(dir.dirName())) {
        if (dir.dirName().compare(QStringLiteral("update"), Qt::CaseInsensitive) == 0 ||
            dir.dirName().compare(QStringLiteral("updates"), Qt::CaseInsensitive) == 0) {
            is_update = true;
        }
        if (!dir.cdUp()) {
            return {};
        }
    }

    const QString name = dir.dirName();
    if (name.isEmpty()) {
        return {};
    }
    if (is_update && !name.contains(QStringLiteral("update"), Qt::CaseInsensitive)) {
        return QObject::tr("%1 (update)").arg(name);
    }
    return name;
}

std::string GetName(Loader::AppLoader& loader, const std::string& physical_name) {
    std::string name;
    if (loader.ReadTitle(name) == Loader::ResultStatus::Success && !name.empty() && name != " ") {
        return name;
    }
    return LooseExeFSName(physical_name).toStdString();
}

std::string GetName(Loader::AppLoader& loader) {
    const auto& file = loader.GetFile();
    return GetName(loader, file ? file->GetFullPath() : std::string{});
}

std::string GetDisplayVersion(Loader::AppLoader& loader) {
    u64 program_id{};
    if (QtCommon::system && loader.ReadProgramId(program_id) == Loader::ResultStatus::Success) {
        const FileSys::PatchManager patch{program_id, QtCommon::system->GetFileSystemController(),
                                          QtCommon::system->GetContentProvider()};
        if (const auto metadata = patch.GetControlMetadata(); metadata.first != nullptr) {
            if (std::string version = metadata.first->GetVersionString(); !version.empty()) {
                return version;
            }
        }
    }

    std::string version;
    if (loader.ReadDisplayVersion(version) != Loader::ResultStatus::Success) {
        return {};
    }
    return version;
}

} // namespace QtCommon::Title
