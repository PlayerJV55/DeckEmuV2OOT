// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

/// Zip helpers for code outside qt_common, which has no access to QuaZip's headers.
namespace QtCommon::Zip {

/// Writes the files under `dir` into a new zip file, paths relative to `dir`.
bool CompressDir(const QString& zip_file, const QString& dir);

/// Extracts a zip file into `dir`. False when it can't be read or holds nothing.
bool ExtractDir(const QString& zip_file, const QString& dir);

} // namespace QtCommon::Zip
