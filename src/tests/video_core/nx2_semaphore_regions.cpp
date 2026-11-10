// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/nx2.h"

namespace {
constexpr u64 FirstRegion = 0x5cd1c000;
constexpr u64 RegionStride = 0x4000;
constexpr u32 RegionCount = 200;
} // Anonymous namespace

TEST_CASE("Semaphore regions mirror past the old 32-region limit", "[video_core][nx2]") {
    Tegra::Nx2ClearSemaphoreRegions();
    for (u32 i = 0; i < RegionCount; ++i) {
        Tegra::Nx2AddSemaphoreRegion(FirstRegion + i * RegionStride);
    }
    for (u32 i = 0; i < RegionCount; ++i) {
        const u64 start = FirstRegion + i * RegionStride;
        CAPTURE(i);
        REQUIRE(Tegra::Nx2SemaphoreMirror(start + 0x1000) == start);
        REQUIRE(Tegra::Nx2SemaphoreMirror(start + 0x1ffc) == start + 0xffc);
        REQUIRE(Tegra::Nx2SemaphoreMirror(start) == 0);
        REQUIRE(Tegra::Nx2SemaphoreMirror(start + 0x2000) == 0);
    }
    Tegra::Nx2ClearSemaphoreRegions();
    REQUIRE(Tegra::Nx2SemaphoreMirror(FirstRegion + 0x1000) == 0);
}

TEST_CASE("Semaphore regions are added once and removed by start", "[video_core][nx2]") {
    Tegra::Nx2ClearSemaphoreRegions();
    Tegra::Nx2AddSemaphoreRegion(FirstRegion);
    Tegra::Nx2AddSemaphoreRegion(FirstRegion);
    Tegra::Nx2AddSemaphoreRegion(FirstRegion + RegionStride);
    Tegra::Nx2RemoveSemaphoreRegion(FirstRegion);
    REQUIRE(Tegra::Nx2SemaphoreMirror(FirstRegion + 0x1000) == 0);
    REQUIRE(Tegra::Nx2SemaphoreMirror(FirstRegion + RegionStride + 0x1000) ==
            FirstRegion + RegionStride);

    // Starts need not be page aligned.
    Tegra::Nx2AddSemaphoreRegion(FirstRegion + 0x800);
    REQUIRE(Tegra::Nx2SemaphoreMirror(FirstRegion + 0x17fc) == 0);
    REQUIRE(Tegra::Nx2SemaphoreMirror(FirstRegion + 0x1800) == FirstRegion + 0x800);
    REQUIRE(Tegra::Nx2SemaphoreMirror(FirstRegion + 0x27fc) == FirstRegion + 0x17fc);
    REQUIRE(Tegra::Nx2SemaphoreMirror(FirstRegion + 0x2800) == 0);
    Tegra::Nx2ClearSemaphoreRegions();
}
