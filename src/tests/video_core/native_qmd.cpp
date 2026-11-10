// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/native_qmd.h"

TEST_CASE("QMD release formats are validated before release", "[video_core][qmd]") {
    using Scheduler = Tegra::NativeQmdScheduler;
    Scheduler::Descriptor descriptor{};
    descriptor[18] = 0x30;
    descriptor[52] = 0x9000;
    descriptor[54] = 42;

    const auto run = [&](u32 flags, bool accepted) {
        Scheduler::Reset();
        Scheduler scheduler;
        descriptor[53] = flags;
        u32 releases{};
        const auto error = scheduler.Signal(
            0x10000, 3,
            [&](u64, Scheduler::Descriptor& out) {
                out = descriptor;
                return true;
            },
            [&](u64 address, u32 payload, u32 actual_flags) {
                CHECK(address == 0x9000);
                CHECK(payload == 42);
                CHECK(actual_flags == flags);
                ++releases;
                return true;
            });
        CHECK(error.empty() == accepted);
        CHECK(releases == (accepted ? 1U : 0U));
    };
    constexpr u32 enabled = 1U << 23;
    run(enabled, true);
    run(enabled | (1U << 30), true);
    run(enabled | (1U << 26), false);
    run(enabled | (1U << 19), false);
    run(enabled | (1U << 31), false);
    Scheduler::Reset();
}

TEST_CASE("Shifted QMD releases are preserved without dispatch", "[video_core][qmd]") {
    using Scheduler = Tegra::NativeQmdScheduler;
    Scheduler::Descriptor descriptor{};
    descriptor[24] = 0x30;
    descriptor[18] = 4;
    descriptor[52] = 0x9000;
    descriptor[53] = (1U << 23) | (1U << 30);
    descriptor[54] = 42;
    u32 releases{};
    u32 dispatches{};
    const auto run = [&] {
        Scheduler::Reset();
        Scheduler scheduler;
        return scheduler.Signal(
            0x10000, 3,
            [&](u64, Scheduler::Descriptor& out) { out = descriptor; return true; },
            [&](u64 address, u32 payload, u32) {
                CHECK(address == 0x9000);
                CHECK(payload == 42);
                ++releases;
                return true;
            },
            [&](const Scheduler::Descriptor&) { ++dispatches; return true; });
    };
    CHECK(run().empty());
    CHECK(releases == 1);
    CHECK(dispatches == 0);

    descriptor[10] = 0x100;
    CHECK_FALSE(run().empty());
    CHECK(releases == 1);
    Scheduler::Reset();
}
