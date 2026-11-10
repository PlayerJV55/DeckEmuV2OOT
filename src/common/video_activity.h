// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/common_types.h"

namespace Common {

inline std::atomic<s64> g_last_video_frame{};

/// A video decoder produced a frame (Switch 2 vdec or NVDEC).
inline void NoteVideoFrame() {
    g_last_video_frame.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                             std::memory_order_relaxed);
}

/// Whether a video decoder produced a frame in the last 250 ms.
[[nodiscard]] inline bool VideoFramesRecent() {
    const std::chrono::steady_clock::duration last{
        g_last_video_frame.load(std::memory_order_relaxed)};
    return std::chrono::steady_clock::now().time_since_epoch() - last <
           std::chrono::milliseconds{250};
}

} // namespace Common
