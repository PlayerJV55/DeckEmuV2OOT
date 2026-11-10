// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Release selection, failure classification and the skip/remind flow are adapted from Lichii
// #82 and #504 (zortos293/Lichii), moved from GitHub's build-N tags to Nemyu's GitLab releases.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <tuple>
#include <fmt/format.h>
#include "common/logging.h"
#include "common/scm_rev.h"
#include "frontend_common/update_checker.h"

namespace UpdateChecker {

namespace {

std::optional<u32> ParseNumber(std::string_view text) {
    u32 number{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() ||
        (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    return number;
}

std::string DownloadPath(std::string_view tag, std::string_view filename) {
    return fmt::format("/{}/-/releases/{}/downloads/{}", GitLabProject, tag, filename);
}

std::optional<Common::Net::Asset> FindAsset(const Common::Net::Release& release,
                                            std::string filename, std::string name) {
    const auto path = DownloadPath(release.tag, filename);
    if (std::ranges::find(release.assets, fmt::format("{}{}", GitLabHost, path)) ==
        release.assets.end()) {
        return std::nullopt;
    }
    return Common::Net::Asset{.name = std::move(name),
                              .url = std::string{GitLabHost},
                              .path = path,
                              .filename = std::move(filename)};
}

} // namespace

std::strong_ordering Version::operator<=>(const Version& other) const {
    if (const auto order = std::tie(major, minor, patch) <=>
                           std::tie(other.major, other.minor, other.patch);
        order != 0) {
        return order;
    }
    if (experimental && other.experimental) {
        return *experimental <=> *other.experimental;
    }
    return other.experimental.has_value() <=> experimental.has_value();
}

std::optional<Version> ParseTag(std::string_view tag) {
    constexpr std::string_view prefix = "nemyu-v";
    constexpr std::string_view experimental = "-experimental.";
    if (!tag.starts_with(prefix)) {
        return std::nullopt;
    }
    tag.remove_prefix(prefix.size());

    Version version;
    if (const auto pos = tag.find(experimental); pos != std::string_view::npos) {
        version.experimental = ParseNumber(tag.substr(pos + experimental.size()));
        if (!version.experimental || *version.experimental == 0) {
            return std::nullopt;
        }
        tag = tag.substr(0, pos);
    }

    const auto first = tag.find('.');
    const auto second = first == std::string_view::npos ? first : tag.find('.', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    const auto major = ParseNumber(tag.substr(0, first));
    const auto minor = ParseNumber(tag.substr(first + 1, second - first - 1));
    const auto patch = ParseNumber(tag.substr(second + 1));
    if (!major || !minor || !patch) {
        return std::nullopt;
    }
    version.major = *major;
    version.minor = *minor;
    version.patch = *patch;
    return version;
}

std::string_view CurrentPlatform() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x86_64";
#else
    return {};
#endif
}

std::optional<Common::Net::Asset> FindPackage(const Common::Net::Release& release,
                                              std::string_view platform) {
    if (platform.empty()) {
        return std::nullopt;
    }
    return FindAsset(release, fmt::format("nemyu-{}-{}.zip", release.tag, platform), "Standard");
}

std::optional<Common::Net::Asset> FindChecksum(const Common::Net::Release& release,
                                               const Common::Net::Asset& package) {
    return FindAsset(release, package.filename + ".sha256", "SHA-256");
}

std::optional<std::string> ParseChecksum(std::string_view text, std::string_view filename) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    const auto start = std::ranges::find_if_not(text, is_space);
    const auto digest_end = std::find_if(start, text.end(), is_space);
    std::string digest{start, digest_end};
    if (digest.size() != 64 ||
        !std::ranges::all_of(digest, [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        return std::nullopt;
    }

    const auto name_start = std::find_if_not(digest_end, text.end(), is_space);
    std::string_view name{name_start, std::find_if(name_start, text.end(), is_space)};
    if (name.starts_with('*')) {
        name.remove_prefix(1);
    }
    if (!name.empty() && name != filename) {
        return std::nullopt;
    }

    std::ranges::transform(digest, digest.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return digest;
}

std::optional<Common::Net::Release> SelectUpdate(std::span<const Common::Net::Release> releases,
                                                 std::string_view build_tag,
                                                 std::string_view platform) {
    const auto current = ParseTag(build_tag);
    if (!current) {
        return std::nullopt;
    }

    std::optional<Version> newest = current;
    const Common::Net::Release* selected = nullptr;
    for (const auto& release : releases) {
        const auto version = ParseTag(release.tag);
        if (!version || *version <= *newest) {
            continue;
        }
        if (version->experimental && !current->experimental) {
            continue;
        }
        if (!platform.empty() && !FindPackage(release, platform)) {
            continue;
        }
        newest = version;
        selected = &release;
    }
    return selected ? std::make_optional(*selected) : std::nullopt;
}

Failure ClassifyFailure(int status) {
    if (status == 403 || status == 429) {
        return Failure::RateLimited;
    }
    return status >= 400 ? Failure::Http : Failure::Network;
}

CheckResult Check() {
    if (!ParseTag(Common::g_build_release_tag)) {
        return {.failure = Failure::NotReleaseBuild};
    }

    std::string project{GitLabProject};
    project.replace(project.find('/'), 1, "%2F");
    const auto response = Common::Net::Fetch(
        std::string{GitLabHost}, fmt::format("/api/v4/projects/{}/releases?per_page=50", project));
    if (!response.body) {
        LOG_WARNING(Frontend, "Update check failed with status {}", response.status);
        return {.failure = ClassifyFailure(response.status), .status = response.status};
    }

    const auto releases = Common::Net::Release::ListFromGitLabJson(*response.body);
    auto update = SelectUpdate(releases, Common::g_build_release_tag, CurrentPlatform());
    if (update) {
        LOG_INFO(Frontend, "Nemyu {} is available", update->tag);
    }
    return {.update = std::move(update)};
}

std::optional<Common::Net::Release> GetUpdate() {
    return Check().update;
}

} // namespace UpdateChecker
