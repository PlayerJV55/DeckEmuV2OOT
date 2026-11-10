// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <QWidget>
#include "yuzu/configuration/configuration_shared.h"

namespace Core {
class System;
}

namespace ConfigurationShared {
class Builder;
}

/// The frame generation settings (Graphics > Frame Generation)
class ConfigureGraphicsFrameGen : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureGraphicsFrameGen(
        const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
        const ConfigurationShared::Builder& builder, QWidget* parent = nullptr);
    ~ConfigureGraphicsFrameGen() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

private:
    const Core::System& system;

    std::vector<std::function<void(bool)>> apply_funcs;
};
