// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "hid_core/hid_types.h"

namespace Service::HID {

// The sensor IC information GetSixAxisSensorIcInformation reports before a game changes it: the
// gyroscope's 2000 dps and the accelerometer's 8 G ranges with their calibration bounds. All zero,
// the ranges invalidate the guest's sensor fusion. EDEN_NX2_SIXAXIS_LEGACY=1 reports zeros again.
inline Core::HID::SixAxisSensorIcInformation DefaultSixAxisSensorIcInformation() {
    static const bool legacy = [] {
        const char* value = std::getenv("EDEN_NX2_SIXAXIS_LEGACY");
        return value != nullptr && value[0] == '1';
    }();
    if (legacy) {
        return {};
    }
    return {
        .angular_rate = 2000.0f,
        .unknown_gyro_data1 = {-10.0f, -10.0f, -10.0f, 10.0f, 10.0f, 10.0f},
        .unknown_gyro_data2 = {0.95f, -0.003f, -0.003f, -0.003f, 0.95f, -0.003f, -0.003f, -0.003f,
                               0.95f},
        .unknown_gyro_data3 = {1.05f, 0.003f, 0.003f, 0.003f, 1.05f, 0.003f, 0.003f, 0.003f,
                               1.05f},
        .acceleration_range = 8.0f,
        .unknown_accel_data1 = {-0.0612f, -0.0612f, -0.0612f, 0.0612f, 0.0612f, 0.0612f},
        .unknown_accel_data2 = {0.95f, -0.003f, -0.003f, -0.003f, 0.95f, -0.003f, -0.003f, -0.003f,
                                0.95f},
        .unknown_accel_data3 = {1.05f, 0.003f, 0.003f, 0.003f, 1.05f, 0.003f, 0.003f, 0.003f,
                                1.05f},
    };
}

} // namespace Service::HID
