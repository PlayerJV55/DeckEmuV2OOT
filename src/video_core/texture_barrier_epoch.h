// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCommon {

/// Counts the guest's texture barriers (Maxwell3D fragment_barrier, invalidate_texture_data_cache,
/// tiled_cache_barrier and wait_for_idle, and the host class's cache invalidation). Texture reads
/// are ordered after earlier colour writes only across one; TextureCache::SynchronizeAliases uses
/// it to bound feedback copies. Bumped on the thread that runs the draws, in their order.
inline std::atomic<u64> texture_barrier_epoch{};

inline void NoteTextureBarrier() {
    texture_barrier_epoch.fetch_add(1, std::memory_order_relaxed);
}

} // namespace VideoCommon
