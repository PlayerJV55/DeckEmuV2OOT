// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Core::Frontend {
class EmuWindow;
}

namespace Tegra {
class GPU;
}

namespace VideoCore {

class RendererBase;
void CreateGPU(std::optional<Tegra::GPU>& gpu, Core::Frontend::EmuWindow& emu_window, Core::System& system);

/// Whether the last frame a Switch 2 game showed was drawn at the resolution scale.
enum class Switch2FrameScale : u8 {
    Unknown, ///< No Switch 2 frame shown in this session yet.
    Native,  ///< Shown at the console's resolution.
    Scaled,  ///< Shown at the resolution scale.
};

/// For the status bar; safe to call from any thread.
[[nodiscard]] Switch2FrameScale GetSwitch2FrameScale();
void SetSwitch2FrameScale(Switch2FrameScale scale);

} // namespace VideoCore
