// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>

#include "common/logging.h"
#include "common/nx2_title_defaults.h"
#include "common/settings.h"

namespace Common::Nx2 {

std::optional<std::string_view> FindTitleDefault(std::string_view name) {
    const u64 program_id = Settings::GetCurrentProgramID();
    const auto entry = std::ranges::find_if(TitleDefaults, [&](const TitleDefault& candidate) {
        return candidate.program_id == program_id && candidate.name == name;
    });
    if (entry == TitleDefaults.end()) {
        return std::nullopt;
    }
    const char* const enabled = std::getenv("EDEN_NX2_TITLE_DEFAULTS");
    if (enabled != nullptr && enabled[0] == '0') {
        LOG_INFO(Common, "{}: the default {} for {:016X} is off (EDEN_NX2_TITLE_DEFAULTS=0)", name,
                 entry->value, program_id);
        return std::nullopt;
    }
    LOG_INFO(Common, "{}={}: the default for {:016X}", name, entry->value, program_id);
    return entry->value;
}

std::string_view Default(std::string_view name, std::string_view global_default) {
    return FindTitleDefault(name).value_or(global_default);
}

std::string_view Setting(const char* name, std::string_view global_default) {
    if (const char* const value = std::getenv(name)) {
        return value;
    }
    return Default(name, global_default);
}

} // namespace Common::Nx2
