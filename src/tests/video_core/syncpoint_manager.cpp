// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/syncpoint_manager.h"

using Tegra::Host1x::SyncpointManager;

TEST_CASE("Syncpoint actions run outside the guard, in value order",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    std::vector<u32> order;
    bool nested = false;
    // An action that registers another one takes the guard itself; under the guard this
    // deadlocked.
    manager->RegisterGuestAction(0, 2, [&] {
        order.push_back(2);
        manager->RegisterGuestAction(1, 1, [&nested] { nested = true; });
    });
    manager->RegisterGuestAction(0, 1, [&order] { order.push_back(1); });
    manager->RegisterGuestAction(0, 3, [&order] { order.push_back(3); });
    manager->IncrementGuest(0);
    manager->IncrementGuest(0);
    REQUIRE(order == std::vector<u32>{1, 2});
    manager->IncrementGuest(1);
    REQUIRE(nested);
    manager->IncrementGuest(0);
    REQUIRE(order == std::vector<u32>{1, 2, 3});
}

TEST_CASE("A syncpoint action can cancel itself and a later due action",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    bool second = false;
    SyncpointManager::ActionHandle first_handle{};
    // Of two actions for the same value, the one registered last runs first.
    const auto second_handle = manager->RegisterGuestAction(0, 1, [&second] { second = true; });
    first_handle = manager->RegisterGuestAction(0, 1, [&] {
        manager->DeregisterGuestAction(0, first_handle); // must not wait for itself
        manager->DeregisterGuestAction(0, second_handle);
    });
    manager->IncrementGuest(0);
    REQUIRE_FALSE(second); // not taken out yet when the first one cancelled it
}

TEST_CASE("A cancel from another thread waits only for a running action",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    std::atomic<bool> release{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> second{false};
    // Of two actions for the same value, the one registered last runs first.
    const auto second_handle =
        manager->RegisterGuestAction(0, 1, [&second] { second = true; });
    const auto first_handle = manager->RegisterGuestAction(0, 1, [&] {
        entered = true;
        while (!release) {
            std::this_thread::yield();
        }
        finished = true;
    });
    std::thread incrementer{[&] { manager->IncrementGuest(0); }};
    while (!entered) {
        std::this_thread::yield();
    }
    // The guard is free while the first action blocks.
    manager->RegisterGuestAction(1, 1, [] {});
    // The second action has not been taken out: the cancel removes it without waiting.
    manager->DeregisterGuestAction(0, second_handle);
    CHECK_FALSE(finished.load()); // threads still to join: no REQUIRE before that
    std::thread releaser{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release = true;
    }};
    // The first one is running: the cancel returns only after it ran.
    manager->DeregisterGuestAction(0, first_handle);
    CHECK(finished.load());
    releaser.join();
    incrementer.join();
    REQUIRE_FALSE(second.load());
}
