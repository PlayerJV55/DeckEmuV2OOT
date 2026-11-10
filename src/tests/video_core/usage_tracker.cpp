// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <initializer_list>
#include <random>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/buffer_cache/usage_tracker.h"

namespace {
constexpr u64 TRACKER_SIZE = 16384;
}

TEST_CASE("UsageTracker keeps ranges shorter than a block", "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker{TRACKER_SIZE};
    for (const u64 offset : {u64{0}, u64{1}, u64{63}, u64{100}, u64{4095}, u64{4097}}) {
        for (u64 size = 1; size < 64; ++size) {
            CAPTURE(offset, size);
            tracker.Reset();
            tracker.Track(offset, size);
            REQUIRE(tracker.IsUsed(offset, 1));
            REQUIRE(tracker.IsUsed(offset + size - 1, 1));
            if (offset + size <= 4032) {
                // Only the touched blocks: the last block of the page stays free.
                REQUIRE_FALSE(tracker.IsUsed(4032, 128));
            }
        }
    }
}

TEST_CASE("UsageTracker marks every block an unaligned range touches",
          "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker{TRACKER_SIZE};
    constexpr std::array<std::pair<u64, u64>, 6> ranges{
        {{60, 100}, {63, 2}, {4090, 100}, {4095, 4095}, {100, 8000}, {4032, 128}}};
    for (const auto& [offset, size] : ranges) {
        CAPTURE(offset, size);
        tracker.Reset();
        tracker.Track(offset, size);
        for (u64 byte = offset; byte < offset + size; ++byte) {
            CAPTURE(byte);
            REQUIRE(tracker.IsUsed(byte, 1));
        }
    }
}

TEST_CASE("UsageTracker ignores empty ranges and page-aligned ends",
          "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker{TRACKER_SIZE};
    tracker.Track(128, 0);
    REQUIRE_FALSE(tracker.IsUsed(0, TRACKER_SIZE));
    tracker.Track(512, 64);
    REQUIRE_FALSE(tracker.IsUsed(512, 0));

    // A range ending on a page boundary must not mark the next page.
    tracker.Reset();
    tracker.Track(0, 4096);
    REQUIRE(tracker.IsUsed(4032, 64));
    REQUIRE_FALSE(tracker.IsUsed(4096, 4096));
}

TEST_CASE("UsageTracker checks only the queried blocks of a single page",
          "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker{TRACKER_SIZE};
    tracker.Track(0, 100);
    REQUIRE(tracker.IsUsed(64, 16));
    REQUIRE(tracker.IsUsed(96, 4));
    REQUIRE_FALSE(tracker.IsUsed(128, 64));

    tracker.Reset();
    tracker.Track(4096 + 128, 16);
    REQUIRE(tracker.IsUsed(4096 + 128, 16));
    REQUIRE_FALSE(tracker.IsUsed(4096 + 256, 64));
    REQUIRE_FALSE(tracker.IsUsed(4096, 64));
}

TEST_CASE("UsageTracker never misses an overlapping range", "[video_core][usage_tracker]") {
    VideoCommon::UsageTracker tracker{TRACKER_SIZE};
    std::array<bool, TRACKER_SIZE> used{};
    std::mt19937 random{0x5e1e};
    const auto range = [&] {
        const u64 offset = random() % TRACKER_SIZE;
        const u64 size = random() % (std::min<u64>(TRACKER_SIZE - offset, 512) + 1);
        return std::pair{offset, size};
    };
    for (u32 step = 0; step < 2048; ++step) {
        if (random() % 8 == 0) {
            tracker.Reset();
            used.fill(false);
        } else {
            const auto [offset, size] = range();
            tracker.Track(offset, size);
            std::fill_n(used.begin() + offset, size, true);
        }
        for (u32 probe = 0; probe < 8; ++probe) {
            const auto [offset, size] = range();
            const bool overlaps =
                std::any_of(used.begin() + offset, used.begin() + offset + size,
                            [](bool written) { return written; });
            CAPTURE(step, offset, size);
            REQUIRE((!overlaps || tracker.IsUsed(offset, size)));
        }
    }
}
