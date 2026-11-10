// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

#include "common/common_types.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"

using Service::Nvidia::Devices::Nx2GpuMemoryRegistry;

namespace {

using Release = std::pair<u64, u64>;

// Listens to the registry like an address space, recording the releases it is sent.
class ReleaseRecorder {
public:
    explicit ReleaseRecorder(std::vector<Release>& releases) {
        auto& registry = Nx2GpuMemoryRegistry::Get();
        registry.Reset();
        registry.AddListener(
            this, [](u64, u64, u64) {}, [](u64, u64) {},
            [&releases](u64 cpu_address, u64 size) { releases.emplace_back(cpu_address, size); });
    }

    ~ReleaseRecorder() {
        auto& registry = Nx2GpuMemoryRegistry::Get();
        registry.RemoveListener(this);
        registry.Reset();
    }

    ReleaseRecorder(const ReleaseRecorder&) = delete;
    ReleaseRecorder& operator=(const ReleaseRecorder&) = delete;
};

} // Anonymous namespace

TEST_CASE("Nx2 destroyed memory releases its device ranges once no mapping uses it",
          "[nx2_mapping]") {
    std::vector<Release> releases;
    ReleaseRecorder recorder{releases};
    auto& registry = Nx2GpuMemoryRegistry::Get();

    // Sub 101 maps the memory at its own address, 211 part of it elsewhere too.
    registry.Register(0x11be802000, 0x556000);
    registry.MapAt(0x11a0000000, 0x11be812000, 0x10000);

    // Sub 102: its own mapping goes, the release waits for the 211 mapping.
    registry.Unmap(0x11be802000, 0x556000);
    registry.ReleaseWhenUnmapped(0x11be802000, 0x556000);
    CHECK(releases.empty());

    registry.Unmap(0x11a0000000, 0x10000);
    CHECK(releases == std::vector<Release>{{0x11be802000, 0x556000}});

    // Released once only.
    registry.Unmap(0x11a0000000, 0x10000);
    CHECK(releases.size() == 1U);
}

TEST_CASE("Nx2 destroyed memory waits for new memory mapped over its address", "[nx2_mapping]") {
    std::vector<Release> releases;
    ReleaseRecorder recorder{releases};
    auto& registry = Nx2GpuMemoryRegistry::Get();

    registry.Register(0x11be802000, 0x556000);
    registry.Unmap(0x11be802000, 0x556000);
    // New memory at the same address before the release could run (a 211 mapping kept the old
    // memory mapped until now).
    registry.Register(0x11be802000, 0x1546000);
    registry.ReleaseWhenUnmapped(0x11be802000, 0x556000);
    CHECK(releases.empty());

    registry.Unmap(0x11be802000, 0x1546000);
    CHECK(releases == std::vector<Release>{{0x11be802000, 0x556000}});
}
