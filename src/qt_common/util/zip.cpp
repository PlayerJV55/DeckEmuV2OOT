// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "qt_common/util/compress.h"
#include "qt_common/util/zip.h"

namespace QtCommon::Zip {
namespace {

bool KeepGoing(std::size_t, std::size_t) {
    return true;
}

} // Anonymous namespace

bool CompressDir(const QString& zip_file, const QString& dir) {
    return Compress::compressDir(zip_file, dir, Compress::Options(), KeepGoing);
}

bool ExtractDir(const QString& zip_file, const QString& dir) {
    return !Compress::extractDir(zip_file, dir, KeepGoing).isEmpty();
}

} // namespace QtCommon::Zip
