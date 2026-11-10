// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"

// Mario Kart World's water. Both fixes act on one exact native program each (keys from
// Shader::Ampere::NativeProgramKey), so no other program or title is touched.
namespace VideoCommon::MkwWater {

/// The wave displacement program (GenericEnvironment::LowerNativeCode patches it).
inline constexpr u64 WaveProgramKey = 0x5a4f3726590efb17ULL;
/// The program writing the CPU-read collision height map (RasterizerVulkan::DispatchCompute).
inline constexpr u64 CollisionProgramKey = 0x6e0ae200ee9aa220ULL;

/// EDEN_NX2_MKW_WATER=0 turns both off.
[[nodiscard]] inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_MKW_WATER");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

} // namespace VideoCommon::MkwWater
