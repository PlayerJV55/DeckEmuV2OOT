// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/dma_pusher.h"

using Tegra::Nx2NoteRunningList;
using Tegra::Nx2ReportTimeNs;

TEST_CASE("GPU time reports show when the work was submitted", "[video_core][report_time]") {
    // The host's time spent on the commands must not show: two reports of one list agree.
    REQUIRE(Nx2ReportTimeNs(5'000, 1'000, 0) == 1'000);
    REQUIRE(Nx2ReportTimeNs(9'000, 1'000, 1'000) == 1'000);
    // No list running yet: the current time.
    REQUIRE(Nx2ReportTimeNs(5'000, 0, 0) == 5'000);
    // A submit time ahead of the clock (another core's reading) shows as now.
    REQUIRE(Nx2ReportTimeNs(5'000, 6'000, 0) == 5'000);
    REQUIRE(Nx2ReportTimeNs(5'000, 5'000, 0) == 5'000);
    // Never behind an earlier report (an older list on another channel).
    REQUIRE(Nx2ReportTimeNs(9'000, 2'000, 3'000) == 3'000);
    REQUIRE(Nx2ReportTimeNs(9'000, 4'000, 3'000) == 4'000);
}

TEST_CASE("Reports show the running list's submit time and never go back",
          "[video_core][report_time]") {
    Tegra::Nx2ReportClock clock;
    Tegra::CommandList list;
    list.submit_ns = 1'000;
    Nx2NoteRunningList(list);
    REQUIRE(Tegra::g_nx2_list_submit_ns.load() == 1'000);
    REQUIRE(clock.Next(5'000) == 1'000);
    REQUIRE(clock.Next(7'000) == 1'000);
    // A list without a submit time keeps the last one.
    Nx2NoteRunningList(Tegra::CommandList{});
    REQUIRE(Tegra::g_nx2_list_submit_ns.load() == 1'000);
    list.submit_ns = 2'000;
    Nx2NoteRunningList(list);
    REQUIRE(clock.Next(9'000) == 2'000);
    // An older list (another channel) does not move reports back.
    list.submit_ns = 1'500;
    Nx2NoteRunningList(list);
    REQUIRE(clock.Next(9'500) == 2'000);
}
