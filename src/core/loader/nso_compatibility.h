// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include "common/common_types.h"

namespace Loader {
struct NSOHeader;

// Minimal nnCompat platform-query override; the caller selects compatibility mode.
std::optional<u64> ApplyOuncePlatformCompatibility(std::span<u8> image, const NSOHeader& header);
} // namespace Loader
