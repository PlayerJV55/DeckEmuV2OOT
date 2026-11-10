// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <time.h>
#endif

#include "common/thread.h"
#include "video_core/back_split_pool.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon::BackSplit {
namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

// Pauses between two reads of the clock while a worker polls for a job.
constexpr u32 ClockCheckSpins = 64;
// How often a measured worker publishes its CPU time while it runs or polls.
constexpr u64 CpuPublishNs = 1'000'000;

} // Anonymous namespace

std::optional<u64> CurrentThreadCpuNs() {
#ifdef _WIN32
    FILETIME creation_time{}, exit_time{}, kernel_time{}, user_time{};
    if (!GetThreadTimes(GetCurrentThread(), &creation_time, &exit_time, &kernel_time,
                        &user_time)) {
        return std::nullopt;
    }
    const u64 kernel_ticks = (u64{kernel_time.dwHighDateTime} << 32) | kernel_time.dwLowDateTime;
    const u64 user_ticks = (u64{user_time.dwHighDateTime} << 32) | user_time.dwLowDateTime;
    return (kernel_ticks + user_ticks) * 100;
#elif defined(__linux__)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
        return std::nullopt;
    }
    return static_cast<u64>(time.tv_sec) * 1'000'000'000 + static_cast<u64>(time.tv_nsec);
#else
    return std::nullopt;
#endif
}

Pool::Pool(u32 workers, u64 spin_ns_, bool measure_) : spin_ns{spin_ns_}, measure{measure_} {
    worker_data = std::make_unique<WorkerData[]>(workers);
    threads.reserve(workers);
    for (u32 index = 0; index < workers; ++index) {
        threads.emplace_back([this, index](std::stop_token stop) { WorkerLoop(stop, index); });
    }
}

Pool::~Pool() {
    for (auto& thread : threads) {
        thread.request_stop();
    }
    posted.fetch_add(1, std::memory_order_seq_cst);
    posted.notify_all();
    threads.clear();
}

void Pool::Post(Job& job, Job::Func func, void* argument) {
    job.func = func;
    job.argument = argument;
    job.state.store(Job::Posted, std::memory_order_release);
    const u64 sequence = posted.load(std::memory_order_relaxed);
    ring[sequence % Slots].store(&job, std::memory_order_release);
    posted.store(sequence + 1, std::memory_order_seq_cst);
    // One job wakes one sleeping worker; the others stay asleep.
    if (sleepers.load(std::memory_order_seq_cst) != 0) {
        posted.notify_one();
    }
}

JoinResult Pool::Join(Job& job) {
    u32 expected = Job::Posted;
    if (job.state.compare_exchange_strong(expected, Job::Running, std::memory_order_acquire,
                                          std::memory_order_acquire)) {
        job.func(job.argument);
        job.state.store(Job::Idle, std::memory_order_relaxed);
        return {.ran_here = true};
    }
    JoinResult result{};
    if (expected != Job::Done) {
        const u64 start = Nx2Profile::Now();
        while (job.state.load(std::memory_order_acquire) != Job::Done) {
            Pause();
        }
        result.wait_ns = Nx2Profile::Now() - start;
    }
    job.state.store(Job::Idle, std::memory_order_relaxed);
    return result;
}

PoolTotals Pool::Totals() const {
    PoolTotals totals{};
    for (size_t index = 0; index < threads.size(); ++index) {
        const WorkerData& data = worker_data[index];
        totals.worker_jobs += data.jobs.load(std::memory_order_relaxed);
        totals.worker_ns += data.ns.load(std::memory_order_relaxed);
        totals.worker_cpu_ns += data.cpu_ns.load(std::memory_order_relaxed);
        totals.worker_sleeps += data.sleeps.load(std::memory_order_relaxed);
    }
    return totals;
}

void Pool::TryRun(Job& job, WorkerData& data) {
    u32 expected = Job::Posted;
    if (!job.state.compare_exchange_strong(expected, Job::Running, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
        return;
    }
    const u64 start = measure ? Nx2Profile::Now() : 0;
    job.func(job.argument);
    if (measure) {
        data.ns.store(data.ns.load(std::memory_order_relaxed) + Nx2Profile::Now() - start,
                      std::memory_order_relaxed);
    }
    data.jobs.store(data.jobs.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    job.state.store(Job::Done, std::memory_order_release);
}

void Pool::WorkerLoop(std::stop_token stop, u32 index) {
    Common::SetCurrentThreadName(("GPUBackSplit" + std::to_string(index)).c_str());
    WorkerData& data = worker_data[index];
    u64 last_publish = 0;
    const auto publish_cpu = [&](u64 now, bool force) {
        if (!measure || (!force && now - last_publish < CpuPublishNs)) {
            return;
        }
        last_publish = now;
        if (const std::optional<u64> cpu_ns = CurrentThreadCpuNs()) {
            data.cpu_ns.store(*cpu_ns, std::memory_order_relaxed);
        }
    };
    u64 seen = posted.load(std::memory_order_acquire);
    u64 idle_start = Nx2Profile::Now();
    u32 spins = 0;
    while (!stop.stop_requested()) {
        const u64 now_posted = posted.load(std::memory_order_acquire);
        if (now_posted == seen) {
            // Poll for spin_ns after the last job, then sleep until a post wakes this worker:
            // an idle back costs no core, and the joins run what a sleeping worker misses.
            Pause();
            if (++spins % ClockCheckSpins != 0) {
                continue;
            }
            const u64 now = Nx2Profile::Now();
            publish_cpu(now, false);
            if (now - idle_start < spin_ns) {
                continue;
            }
            publish_cpu(now, true);
            data.sleeps.store(data.sleeps.load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
            sleepers.fetch_add(1, std::memory_order_seq_cst);
            if (posted.load(std::memory_order_seq_cst) == seen && !stop.stop_requested()) {
                posted.wait(seen, std::memory_order_seq_cst);
            }
            sleepers.fetch_sub(1, std::memory_order_seq_cst);
            spins = 0;
            idle_start = Nx2Profile::Now();
            continue;
        }
        // A worker that fell a ring behind skips what was overwritten: those jobs are joined
        // (and run, if no other worker took them) by the posting thread anyway.
        if (now_posted - seen > Slots) {
            seen = now_posted - Slots;
        }
        for (; seen != now_posted; ++seen) {
            if (Job* const job = ring[seen % Slots].load(std::memory_order_acquire)) {
                TryRun(*job, data);
            }
        }
        spins = 0;
        idle_start = Nx2Profile::Now();
        publish_cpu(idle_start, false);
    }
}

} // namespace VideoCommon::BackSplit
