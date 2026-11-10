// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_clean_pages.h"

TEST_CASE("Clean pages remember only pages a cache held nothing on", "[video_core][nx2_clean_pages]") {
    VideoCommon::Nx2CleanPages pages;
    constexpr DAddr base = 0x10'0000;
    constexpr size_t Page = 0x1000;
    u64 serial = 7;
    std::vector<DAddr> touched{base + Page};
    const auto clean_serial = [&](DAddr addr, size_t size) -> u64 {
        for (const DAddr held : touched) {
            if (held < addr + size && addr < held + 4) {
                return 0;
            }
        }
        return serial;
    };
    CHECK_FALSE(pages.Contains(base, 0x40, serial));
    pages.Fill(base, Page * 2, clean_serial);
    CHECK(pages.Contains(base + 0x100, 0x40, serial));
    // The second page holds something: a range reaching into it is still invalidated.
    CHECK_FALSE(pages.Contains(base + Page - 0x20, 0x40, serial));
    CHECK_FALSE(pages.Contains(base + Page, 0x40, serial));
    // The cache starts holding something new: nothing remembered counts.
    ++serial;
    CHECK_FALSE(pages.Contains(base + 0x100, 0x40, serial));
    // Long ranges aren't remembered.
    touched.clear();
    pages.Fill(base, Page * 17, clean_serial);
    CHECK_FALSE(pages.Contains(base, 0x40, serial));
    pages.Fill(base, Page * 16, clean_serial);
    CHECK(pages.Contains(base, Page * 16, serial));
    // A page that maps to the same entry replaces it.
    pages.Fill(base + Page * VideoCommon::Nx2CleanPages::Size, 0x40, clean_serial);
    CHECK_FALSE(pages.Contains(base, 0x40, serial));
}
