// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include "common/common_types.h"

namespace FileSys {

struct Switch2Title {
    std::string name;
    std::string cover; ///< Box art name to use instead of the one made from the name, if set
};

/**
 * Names a program that has no control data, as loose Switch 2 dumps usually come without one.
 * Looks in titles.json in the user data directory, then in the table bundled from Switchbrew
 * (dist/switch2_titles.json, see tools/switch2_titles.py). Both map program IDs to a name or to
 * an object: {"0400C3F00006E000": "Mario Kart World",
 *             "04007C6028AA8000": {"name": "...", "cover": "yoshi-and-the-mysterious-book"}}
 * A user entry may set only one of the two fields to override the bundled one.
 */
[[nodiscard]] std::optional<Switch2Title> FindSwitch2Title(u64 program_id);

/**
 * Box art name for a title, as the box art repositories name them:
 * "Hyrule Warriors: Age of Imprisonment" -> "hyrule-warriors-age-of-imprisonment".
 * An e with acute accent becomes e, other non-ASCII characters (trademark signs, curly quotes)
 * are dropped.
 */
[[nodiscard]] std::string MakeCoverSlug(std::string_view title);

/// The title table's cover for program_id if it has one, else MakeCoverSlug(title).
[[nodiscard]] std::string GetCoverSlug(u64 program_id, std::string_view title);

} // namespace FileSys
