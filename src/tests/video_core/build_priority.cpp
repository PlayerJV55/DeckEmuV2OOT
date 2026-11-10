// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/thread_worker.h"
#include "video_core/build_priority.h"

namespace {
namespace BuildPriority = VideoCommon::BuildPriority;
using namespace std::chrono_literals;

constexpr auto Never = [] { return false; };

/// Builds on the workers, held until released, recording the order they finish in
struct Builds {
    std::mutex mutex;
    std::vector<int> finished;
    std::atomic_bool released{false};

    void Finish(int id) {
        std::scoped_lock lock{mutex};
        finished.push_back(id);
    }

    void Queue(Common::ThreadWorker& workers, int id) {
        workers.QueueWork([this, id] {
            released.wait(false);
            Finish(id);
        });
    }

    void Release() {
        released = true;
        released.notify_all();
    }
};

} // Anonymous namespace

TEST_CASE("BuildPriority: a background build starts after the queued builds", "[video_core]") {
    Common::ThreadWorker workers(2, "BuildPriorityTest");
    Builds builds;
    // NeedNow (or the boot set) on the workers, queued before the background build asks
    builds.Queue(workers, 1);
    builds.Queue(workers, 2);
    builds.Queue(workers, 3);

    std::atomic_bool waiting{false};
    std::chrono::steady_clock::duration waited{};
    std::thread background{[&] {
        waited = BuildPriority::YieldToForeground(
            workers,
            [&] {
                waiting = true;
                waiting.notify_all();
                return false;
            },
            10s);
        builds.Finish(100);
    }};
    waiting.wait(false);
    // Queued while the background build waits: it goes ahead too.
    builds.Queue(workers, 4);
    builds.Release();
    background.join();
    workers.WaitForRequests();

    REQUIRE(builds.finished.size() == 5);
    REQUIRE(builds.finished.back() == 100);
    REQUIRE(waited > 0ns);
    REQUIRE(waited < 10s);
}

TEST_CASE("BuildPriority: a background build starts at once on idle workers", "[video_core]") {
    Common::ThreadWorker workers(1, "BuildPriorityTest");
    REQUIRE(BuildPriority::YieldToForeground(workers, Never, 10s) == 0ns);

    std::atomic_bool ran{false};
    workers.QueueWork([&ran] { ran = true; });
    workers.WaitForRequests();
    REQUIRE(ran);
    REQUIRE(BuildPriority::YieldToForeground(workers, Never, 10s) == 0ns);
}

TEST_CASE("BuildPriority: a background build waits at most max_wait", "[video_core]") {
    Common::ThreadWorker workers(1, "BuildPriorityTest");
    Builds builds;
    builds.Queue(workers, 1);
    const auto waited = BuildPriority::YieldToForeground(workers, Never, 50ms);
    builds.Release();
    workers.WaitForRequests();

    REQUIRE(waited >= 50ms);
    REQUIRE(waited < 5s);
    REQUIRE(builds.finished.size() == 1);
}

TEST_CASE("BuildPriority: a stop request ends the wait", "[video_core]") {
    Common::ThreadWorker workers(1, "BuildPriorityTest");
    Builds builds;
    builds.Queue(workers, 1);
    std::atomic_bool stop{false};
    std::thread stopper{[&stop] {
        std::this_thread::sleep_for(20ms);
        stop = true;
    }};
    const auto waited =
        BuildPriority::YieldToForeground(workers, [&stop] { return stop.load(); }, 10s);
    stopper.join();
    builds.Release();
    workers.WaitForRequests();

    REQUIRE(waited > 0ns);
    REQUIRE(waited < 5s);
}
