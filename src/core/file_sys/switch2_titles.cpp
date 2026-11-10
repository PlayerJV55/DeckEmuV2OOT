// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <charconv>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/file_sys/switch2_titles.h"
#include "switch2_titles_json.h"

namespace FileSys {

namespace {

using TitleMap = std::unordered_map<u64, Switch2Title>;

TitleMap ParseTitles(std::string_view text, std::string_view source) {
    TitleMap titles;
    const auto json = nlohmann::json::parse(text.data(), text.data() + text.size(), nullptr, false);
    if (!json.is_object()) {
        LOG_WARNING(Loader, "Ignoring {}, it is not a valid title list", source);
        return titles;
    }

    const auto get_string = [](const nlohmann::json& object, const char* key) {
        const auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    for (const auto& item : json.items()) {
        const std::string& key = item.key();
        const nlohmann::json& value = item.value();
        // Keys other than program IDs, such as "_source", are comments.
        u64 program_id{};
        const char* const end = key.data() + key.size();
        if (key.size() != 16 || std::from_chars(key.data(), end, program_id, 16).ptr != end) {
            continue;
        }
        Switch2Title title;
        if (value.is_string()) {
            title.name = value.get<std::string>();
        } else if (value.is_object()) {
            title.name = get_string(value, "name");
            title.cover = get_string(value, "cover");
        }
        titles.insert_or_assign(program_id, std::move(title));
    }
    return titles;
}

const TitleMap& BundledTitles() {
    static const TitleMap titles = ParseTitles(
        {reinterpret_cast<const char*>(SWITCH2_TITLES_JSON), sizeof(SWITCH2_TITLES_JSON)},
        "the bundled title list");
    return titles;
}

// Read on every lookup so edits apply without a restart; lookups are rare and the file small.
std::optional<Switch2Title> FindUserTitle(u64 program_id) {
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "titles.json";
    if (!Common::FS::Exists(path)) {
        return std::nullopt;
    }
    auto titles = ParseTitles(
        Common::FS::ReadStringFromFile(path, Common::FS::FileType::BinaryFile),
        Common::FS::PathToUTF8String(path));
    const auto it = titles.find(program_id);
    if (it == titles.end()) {
        return std::nullopt;
    }
    return std::move(it->second);
}

} // namespace

std::optional<Switch2Title> FindSwitch2Title(u64 program_id) {
    const auto& bundled = BundledTitles();
    const auto it = bundled.find(program_id);
    Switch2Title title = it != bundled.end() ? it->second : Switch2Title{};
    if (auto user = FindUserTitle(program_id)) {
        if (!user->name.empty()) {
            title.name = std::move(user->name);
        }
        if (!user->cover.empty()) {
            title.cover = std::move(user->cover);
        }
    }
    if (title.name.empty() && title.cover.empty()) {
        return std::nullopt;
    }
    return title;
}

std::string MakeCoverSlug(std::string_view title) {
    constexpr std::string_view EAcute = "\xC3\xA9";
    std::string slug;
    slug.reserve(title.size());
    for (size_t i = 0; i < title.size(); ++i) {
        char c = title[i];
        if (title.substr(i, EAcute.size()) == EAcute) {
            c = 'e';
            i += EAcute.size() - 1;
        } else if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        } else if (c == ' ') {
            c = '-';
        }

        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            slug += c;
        } else if (c == '-' && !slug.empty() && slug.back() != '-') {
            slug += c;
        }
    }
    if (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug;
}

std::string GetCoverSlug(u64 program_id, std::string_view title) {
    if (const auto entry = FindSwitch2Title(program_id); entry && !entry->cover.empty()) {
        return entry->cover;
    }
    return MakeCoverSlug(title);
}

} // namespace FileSys
