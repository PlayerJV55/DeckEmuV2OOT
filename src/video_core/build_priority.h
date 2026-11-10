// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <functional>

#include "common/thread_worker.h"

/// EDEN_NX2_BUILD_PRIORITY (off by default): pipeline builds the game needs go ahead of the ones
/// made ahead of need, after the shader job priorities of Lichii #1036. NeedNow is a draw or
/// dispatch waiting for its pipeline, Soon the boot load of the disk cache, both on the pipeline
/// workers; Background is the prebuild of the cached pipelines boot left out and the compute
/// warm-up, on threads of their own. A background build waits for its turn while the pipeline
/// workers have builds queued or running, instead of taking CPU time and the driver from them.
namespace VideoCommon::BuildPriority {

/// The longest a background build waits, so that steady work on the pipeline workers can't hold
/// the background builds back for good. Lichii lets a background job compete with Soon ones after
/// as long.
inline constexpr std::chrono::seconds MaxYield{2};

/// Reads the switch; called once per pipeline cache.
[[nodiscard]] bool Enabled();

/// Called by a background build before it starts: returns once `foreground` has nothing queued or
/// running, `stop` returns true, or `max_wait` has passed. Returns how long it waited, zero when
/// `foreground` was idle.
std::chrono::steady_clock::duration YieldToForeground(
    Common::ThreadWorker& foreground, const std::function<bool()>& stop,
    std::chrono::steady_clock::duration max_wait = MaxYield);

} // namespace VideoCommon::BuildPriority
