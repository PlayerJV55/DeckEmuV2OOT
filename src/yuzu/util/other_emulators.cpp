// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include "common/fs/path_util.h"
#include "yuzu/util/other_emulators.h"

namespace fs = std::filesystem;

namespace OtherEmulators {
namespace {

// Other emulators that keep their data in a folder named like this one's, beside it.
constexpr std::array<const char*, 12> Names{
    "eden",  "eglantine", "lichii", "frizti", "nyamu", "orchid",
    "droplet", "citron",  "sudachi", "yuzu",  "suyu",  "torzu"};

} // Anonymous namespace

std::vector<Folder> Equivalents(const fs::path& ours) {
    const fs::path app = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir).filename();
    fs::path anchor;
    for (fs::path path = ours; path.has_parent_path() && path != path.parent_path();
         path = path.parent_path()) {
        if (path.filename() == app) {
            anchor = path;
            break;
        }
    }
    if (anchor.empty()) {
        return {};
    }
    const fs::path relative = ours.lexically_relative(anchor);
    std::vector<Folder> found;
    std::error_code error;
    for (const char* name : Names) {
        const fs::path root = anchor.parent_path() / name / relative;
        if (!fs::is_directory(root, error) || fs::equivalent(root, ours, error)) {
            continue;
        }
        QString label = QString::fromLatin1(name);
        label[0] = label[0].toUpper();
        found.push_back({label, root});
    }
    return found;
}

} // namespace OtherEmulators
