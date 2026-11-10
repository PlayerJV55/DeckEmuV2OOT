// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"

namespace Common {

struct MemoryInfo {
    u64 TotalPhysicalMemory{};
    u64 TotalSwapMemory{};
};

/**
 * Gets the memory info of the host system
 * @return Reference to a MemoryInfo struct with the physical and swap memory sizes in bytes
 */
[[nodiscard]] const MemoryInfo& GetMemInfo();

/**
 * Gets how much memory can still be committed right now (RAM plus page file on Windows).
 * @return The free commit in bytes, or nullopt where it is not tracked like this
 */
[[nodiscard]] std::optional<u64> GetAvailableCommitMemory();

} // namespace Common
