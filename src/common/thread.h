// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include "common/common_types.h"
#include "common/polyfill_thread.h"

namespace Common {

class Event {
public:
#ifdef _WIN32
    ~Event();
#endif

    void Set() {
        std::scoped_lock lk{mutex};
        if (!is_set) {
            is_set = true;
#ifdef _WIN32
            if (sleepers != 0) {
                WakeSleeper();
            }
#endif
            condvar.notify_one();
        }
    }

    void Wait() {
        std::unique_lock lk{mutex};
        condvar.wait(lk, [&] { return is_set.load(); });
        is_set = false;
    }

    bool WaitFor(const std::chrono::nanoseconds time);

    /// Like WaitFor, but always blocks in the OS. On x86 Windows, WaitFor keeps the thread
    /// running in a hardware wait; this sleeps on a high-resolution waitable timer instead.
    bool SleepFor(const std::chrono::nanoseconds time);

    template<class Clock, class Duration>
    bool WaitUntil(const std::chrono::time_point<Clock, Duration> time) {
        std::unique_lock lk{mutex};
        if (!condvar.wait_until(lk, time, [this] { return is_set.load(); }))
            return false;
        is_set = false;
        return true;
    }

    void Reset() {
        std::unique_lock lk{mutex};
        // no other action required, since wait loops on the predicate and any lingering signal will
        // get cleared on the first iteration
        is_set = false;
    }

    [[nodiscard]] bool IsSet() const {
        return is_set;
    }

private:
#ifdef _WIN32
    void WakeSleeper();
#endif

    alignas(64) std::atomic<bool> is_set{false};
    std::condition_variable condvar;
    std::mutex mutex;
#ifdef _WIN32
    void* sleep_wake{}; ///< Auto-reset event Set() signals while a SleepFor waits
    u32 sleepers{};     ///< Guarded by mutex
#endif
};

class Barrier {
public:
    explicit Barrier(std::size_t count_) : count(count_) {}

    /// Blocks until all "count" threads have called Sync()
    bool Sync(std::stop_token token = {}) {
        std::unique_lock lk{mutex};
        const std::size_t current_generation = generation;

        if (++waiting == count) {
            generation++;
            waiting = 0;
            condvar.notify_all();
            return true;
        } else {
            condvar.wait(lk, token,
                        [this, current_generation] { return current_generation != generation; });
            return !token.stop_requested();
        }
    }

private:
    std::condition_variable_any condvar;
    std::mutex mutex;
    std::size_t count;
    std::size_t waiting = 0;
    std::size_t generation = 0; // Incremented once each time the barrier is used
};

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

enum class ThreadPlacement : u32 {
    Default = 0,
    Background = 1,
    Efficiency = 2,
};

void SetCurrentThreadPriority(ThreadPriority new_priority);

/// The calling thread's id for BoostThreadPriority; 0 where boosting is not supported.
u32 GetCurrentThreadBoostId();
/// Raises another thread of this process to the highest normal priority and stores its previous
/// priority, which the thread restores with RestoreCurrentThreadPriority. Windows only.
bool BoostThreadPriority(u32 thread_id, s32& previous);
void RestoreCurrentThreadPriority(s32 previous);
void SetCurrentThreadName(const char* name);
void SetCurrentThreadToPerformanceCores();

/// EDEN_NX2_PIN_THREADS=1 (Windows, off by default): one fast physical core each for the split GPU thread's halves,
/// and the guest cores kept off them, through CPU sets. Call it after SetCurrentThreadToPerformanceCores.
enum class PinnedRole : u32 { GpuFront, GpuBack, GuestCore };
void PinCurrentThread(PinnedRole role);
void SetCurrentThreadToEfficiencyCores();
void SetCurrentThreadToBackgroundWork();
void SetCurrentThreadToAllCores();

void RefreshThreadPolicies();
void PollThreadPolicies();

/// Raises the process priority class when EDEN_NX2_HIGH_PRIORITY=1 (Windows).
void ApplyProcessPrioritySwitch();

} // namespace Common
