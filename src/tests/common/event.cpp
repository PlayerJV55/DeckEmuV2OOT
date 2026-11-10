// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <barrier>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/thread.h"

#if !defined(_WIN32) || defined(ARCHITECTURE_x86_64)
TEST_CASE("Event extreme timeout waits for and consumes a signal", "[common][event]") {
    Common::Event event;
    std::jthread setter([&] {
        std::mutex mutex;
        std::unique_lock lock{mutex};
        std::condition_variable timer;
        timer.wait_for(lock, std::chrono::milliseconds{20});
        event.Set();
    });
    const bool signalled = event.WaitFor(std::chrono::nanoseconds::max());
    setter.join();
    CHECK(signalled);
    CHECK_FALSE(event.IsSet());
}
#endif

TEST_CASE("Event nonpositive timeout still consumes an existing signal", "[common][event]") {
    Common::Event event;
    CHECK_FALSE(event.WaitFor(std::chrono::nanoseconds::min()));
    CHECK_FALSE(event.WaitFor(std::chrono::nanoseconds::zero()));
    event.Set();
    CHECK(event.WaitFor(std::chrono::nanoseconds::min()));
    CHECK_FALSE(event.IsSet());
    event.Set();
    CHECK(event.WaitFor(std::chrono::nanoseconds::zero()));
    CHECK_FALSE(event.IsSet());
}

TEST_CASE("Event finite timeout does not complete before its deadline", "[common][event]") {
    Common::Event event;
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(event.WaitFor(std::chrono::milliseconds{2}));
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{2});
}

TEST_CASE("Event SleepFor times out without returning early", "[common][event]") {
    using namespace std::chrono_literals;
    Common::Event event;
    for (const std::chrono::microseconds timeout : {200us, 500us, 2000us, 20000us}) {
        const auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(event.SleepFor(timeout));
        CHECK(std::chrono::steady_clock::now() - start >= timeout);
    }
}

TEST_CASE("Event SleepFor consumes signals and is interrupted by Set", "[common][event]") {
    using namespace std::chrono_literals;
    Common::Event event;
    CHECK_FALSE(event.SleepFor(0ns));
    event.Set();
    CHECK(event.SleepFor(0ns));
    CHECK_FALSE(event.IsSet());
    for (int i = 0; i < 20; ++i) {
        std::jthread setter([&] {
            std::this_thread::sleep_for(2ms);
            event.Set();
        });
        const auto start = std::chrono::steady_clock::now();
        CHECK(event.SleepFor(2s));
        CHECK(std::chrono::steady_clock::now() - start < 1s);
    }
    CHECK_FALSE(event.SleepFor(0ns));
}

TEST_CASE("Concurrent Event sleeps keep their deadlines and wake on Set", "[common][event]") {
    using namespace std::chrono_literals;
    Common::Event event;
    std::barrier ready(3);
    bool short_result{}, long_result{};
    std::chrono::steady_clock::duration short_elapsed{}, long_elapsed{};
    std::jthread short_wait([&] {
        ready.arrive_and_wait();
        const auto start = std::chrono::steady_clock::now();
        short_result = event.SleepFor(20ms);
        short_elapsed = std::chrono::steady_clock::now() - start;
    });
    std::jthread long_wait([&] {
        ready.arrive_and_wait();
        const auto start = std::chrono::steady_clock::now();
        long_result = event.SleepFor(5s);
        long_elapsed = std::chrono::steady_clock::now() - start;
    });
    ready.arrive_and_wait();
    short_wait.join();
    event.Set();
    long_wait.join();
    CHECK_FALSE(short_result);
    CHECK(short_elapsed >= 20ms);
    CHECK(long_result);
    CHECK(long_elapsed < 4s);
}
