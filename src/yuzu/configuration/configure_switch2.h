// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <QWidget>
#include "yuzu/configuration/configuration_shared.h"

class QGroupBox;

namespace Core {
class System;
}

namespace ConfigurationShared {
class Builder;
}

/// The settings only Switch 2 games use (Switch 2 > Switch 2)
class ConfigureSwitch2 : public ConfigurationShared::Tab {
    Q_OBJECT

public:
    explicit ConfigureSwitch2(const Core::System& system_,
                              std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
                              const ConfigurationShared::Builder& builder,
                              QWidget* parent = nullptr);
    ~ConfigureSwitch2() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

    /// Hides the Mario Kart World setting, for another game's properties.
    void HideMarioKartWorldFrameRate();

private:
    const Core::System& system;

    std::vector<std::function<void(bool)>> apply_funcs;

    QGroupBox* mkw_group{};
};
