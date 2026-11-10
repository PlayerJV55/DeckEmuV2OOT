// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "common/common_types.h"

namespace Kernel {
class KProcess;
}

namespace Core::Nx2JitWarm {

/// EDEN_NX2_JIT_WARM=1 (opt-in): each core's JIT keeps a list of the code it compiled, by module
/// and offset (the game's modules load at a different address every boot), and compiles that
/// code again before the core first runs the game, instead of when the game first reaches it.
[[nodiscard]] bool Enabled();

/// How much of a core's code cache the warm-up leaves free for code it has not seen.
inline constexpr std::size_t KeepFreeBytes = 128ULL * 1024 * 1024;

/// Compiles one core's blocks from earlier sessions (location hashes for this boot's module
/// addresses) on that core's JIT. Runs on a worker thread before that core first runs the game.
/// False when the core has no JIT to warm.
using WarmUpFn = std::function<bool(std::size_t core, std::vector<u64>&& locations)>;

/// Called on a core's thread before its first guest run. The first core of a program reads the
/// list and starts every core's warm-up at once, each on its own worker thread; every core then
/// waits for its own. False when this core does not take part (switch off, or another program
/// holds the list).
bool Attach(Kernel::KProcess* process, std::size_t core, const WarmUpFn& warm_up);

/// Waits for this core's warm-up, if one is running (before its JIT goes away).
void Wait(const Kernel::KProcess* process, std::size_t core);

/// New blocks compiled on this core (location hashes). Queued; a background job merges and
/// writes them at most once a minute.
void Add(std::size_t core, std::vector<u64>&& locations);

/// The core's JIT is going away. The last core attached to the program writes the list.
void Detach(std::size_t core, std::vector<u64>&& locations);

} // namespace Core::Nx2JitWarm
