// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/nx2_title_defaults.h"
#include "video_core/build_priority.h"

namespace VideoCommon::BuildPriority {
namespace {
/// How often a waiting build checks its stop request
constexpr std::chrono::milliseconds StopCheck{10};
} // Anonymous namespace

bool Enabled() {
    return Common::Nx2::Setting("EDEN_NX2_BUILD_PRIORITY", "0") != "0";
}

std::chrono::steady_clock::duration YieldToForeground(
    Common::ThreadWorker& foreground, const std::function<bool()>& stop,
    std::chrono::steady_clock::duration max_wait) {
    using Clock = std::chrono::steady_clock;
    if (foreground.WaitForIdle(std::chrono::nanoseconds{0})) {
        return {};
    }
    const auto begin = Clock::now();
    const auto deadline = begin + max_wait;
    for (auto now = begin; now < deadline && !stop(); now = Clock::now()) {
        if (foreground.WaitForIdle(std::min<Clock::duration>(deadline - now, StopCheck))) {
            break;
        }
    }
    return Clock::now() - begin;
}

} // namespace VideoCommon::BuildPriority
