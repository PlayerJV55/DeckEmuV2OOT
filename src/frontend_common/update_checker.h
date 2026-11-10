// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "common/common_types.h"
#include "common/net/net.h"

namespace UpdateChecker {

// Nemyu publishes its releases on GitLab with tags like nemyu-v0.1.7 and
// nemyu-v0.1.7-experimental.4, and one nemyu-<tag>-<platform>.zip per platform beside a
// .sha256 file holding its digest.
constexpr std::string_view GitLabHost = "https://gitlab.com";
constexpr std::string_view GitLabProject = "nemyu-emulator/nemyu";

struct Version {
    u32 major{};
    u32 minor{};
    u32 patch{};
    // Experimental build number; empty for a normal release, which is newer than every
    // experimental build of the same version.
    std::optional<u32> experimental;

    std::strong_ordering operator<=>(const Version& other) const;
    bool operator==(const Version& other) const = default;
};

std::optional<Version> ParseTag(std::string_view tag);

// Release asset platform of this build, such as windows-x86_64; empty where Nemyu publishes no
// package, so the update can only be opened on GitLab.
std::string_view CurrentPlatform();

// The platform package and its digest file in a release, matched by their exact GitLab URLs.
std::optional<Common::Net::Asset> FindPackage(const Common::Net::Release& release,
                                              std::string_view platform);
std::optional<Common::Net::Asset> FindChecksum(const Common::Net::Release& release,
                                               const Common::Net::Asset& package);

// The lowercase hex SHA-256 in a sha256sum-style file naming filename, or in a file holding only
// the digest.
std::optional<std::string> ParseChecksum(std::string_view text, std::string_view filename);

// The newest release that is newer than build_tag. An experimental build is offered both kinds of
// releases, a normal release only normal releases. With a platform, releases without its package
// are skipped.
std::optional<Common::Net::Release> SelectUpdate(std::span<const Common::Net::Release> releases,
                                                 std::string_view build_tag,
                                                 std::string_view platform);

enum class Failure {
    None,
    // A local build, which has no release tag to compare.
    NotReleaseBuild,
    // GitLab refused the request because of rate limiting.
    RateLimited,
    // GitLab answered with an HTTP error.
    Http,
    // No usable answer arrived, for example when offline.
    Network,
};

// Classify a failed release request by its HTTP status, or 0 when no response arrived.
Failure ClassifyFailure(int status);

struct CheckResult {
    std::optional<Common::Net::Release> update;
    Failure failure{Failure::None};
    int status{};
};

CheckResult Check();
std::optional<Common::Net::Release> GetUpdate();

} // namespace UpdateChecker
