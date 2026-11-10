// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"
#include "common/scratch_buffer.h"
#include "common/thread_worker.h"

namespace Tegra::Texture {

/// Smallest unswizzled layer split between the calling thread and a texture worker.
inline constexpr size_t ParallelUnswizzleMinBytes = 2 * 1024 * 1024;

/// Unswizzles a large 2D block-linear layer in two halves, one on an idle texture worker
/// (EDEN_NX2_PERF_OFF=unswizzle_workers keeps it on the calling thread). Only layers with a pitch
/// that isn't a multiple of 4 bytes, which UnswizzleTexture copies a byte or two at a time.
class UnswizzleWorker {
public:
    /// Returns false, having written nothing, when the layer doesn't qualify or no worker is idle.
    bool TryUnswizzle(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment, bool native_tiling, bool verify,
                      Common::ThreadWorker* workers = nullptr);

private:
    /// Copy of the input both halves read, so guest memory can't change between them.
    Common::ScratchBuffer<u8> owned;
};

} // namespace Tegra::Texture
