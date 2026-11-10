// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>

#include "common/common_types.h"

namespace Vulkan {

/// EDEN_NX2_FB_LAYERS_MAX=1 gives framebuffers the most layers of their attachments and clears
/// the requested layers unclipped, as before.
inline bool FramebufferLayersMax() {
    static const bool max_layers = [] {
        const char* value = std::getenv("EDEN_NX2_FB_LAYERS_MAX");
        return value != nullptr && value[0] == '1';
    }();
    return max_layers;
}

struct FramebufferClearLayers {
    u32 count;
    bool covers_all_layers;
};

/// The layers of a clear that exist in a framebuffer of framebuffer_layers layers.
constexpr FramebufferClearLayers GetFramebufferClearLayers(u32 base_layer, u32 layer_count,
                                                           u32 framebuffer_layers) {
    if (base_layer >= framebuffer_layers || layer_count == 0) {
        return {};
    }
    const u32 count = (std::min)(layer_count, framebuffer_layers - base_layer);
    return {count, base_layer == 0 && count == framebuffer_layers};
}

} // namespace Vulkan
