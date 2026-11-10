// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <ratio>
#include <string>
#include <thread>

#include "common/adpf.h"
#include "common/core_pinning.h"
#include "common/error.h"
#include "common/logging.h"
#include "common/assert.h"
#include "common/thread.h"

#ifndef _WIN32
namespace {
std::chrono::steady_clock::time_point EventDeadline(std::chrono::nanoseconds timeout) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (timeout <= nanoseconds::zero()) {
        return now;
    }
    const auto now_ns = duration_cast<nanoseconds>(now.time_since_epoch());
    const auto deadline =
        now_ns > nanoseconds::max() - timeout ? nanoseconds::max() : now_ns + timeout;
    return steady_clock::time_point{duration_cast<steady_clock::duration>(deadline)};
}
} // namespace
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__HAIKU__)
#include <kernel/OS.h>
#elif defined(_WIN32)
#include <windows.h>
#include "common/string_util.h"
#include "common/windows/timer_resolution.h"
#else
#if defined(__FreeBSD__)
#include <sys/cpuset.h>
#include <sys/_cpuset.h>
#include <pthread_np.h>
// Compatibility with CPUset
#define cpu_set_t cpuset_t
#elif defined(__DragonFly__) || defined(__OpenBSD__) || defined(__Bitrig__)
#include <pthread_np.h>
#endif
#include <pthread.h>
#include <sched.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/resource.h>
#include <algorithm>

namespace {
constexpr int NICE_AUDIO = -16;
constexpr int NICE_URGENT_DISPLAY = -8;
constexpr int NICE_DISPLAY = -4;
constexpr int NICE_DEFAULT = 0;
constexpr int NICE_BACKGROUND = 10;

int LowestAllowedNice() {
    static const int lowest = [] {
        rlimit limit{};
        if (getrlimit(RLIMIT_NICE, &limit) != 0) {
            return 0;
        }
        if (limit.rlim_cur >= 40) {
            return -20;
        }
        return 20 - static_cast<int>(limit.rlim_cur);
    }();
    return lowest;
}

int NiceValueForPriority(Common::ThreadPriority priority) {
    const int wanted = [priority] {
        switch (priority) {
        case Common::ThreadPriority::Low: return NICE_BACKGROUND;
        case Common::ThreadPriority::Normal: return NICE_DEFAULT;
        case Common::ThreadPriority::High: return NICE_DISPLAY;
        case Common::ThreadPriority::VeryHigh: return NICE_URGENT_DISPLAY;
        case Common::ThreadPriority::Critical: return NICE_AUDIO;
        default: return NICE_DEFAULT;
        }
    }();
    return (std::max)(wanted, (std::min)(NICE_DEFAULT, LowestAllowedNice()));
}
} // Anonymous namespace
#endif

#ifdef __ANDROID__
#include <sys/utsname.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace {
constexpr size_t ANDROID_MINIMUM_PERFORMANCE_CORES = 4;

// A core counts as a performance core while it is within this much of the fastest one.
constexpr s64 ANDROID_PERFORMANCE_CAPACITY_PERCENT = 50;

constexpr std::chrono::nanoseconds ANDROID_POLICY_POLL_INTERVAL = std::chrono::milliseconds{500};

enum class CoreGroup {
    Unrestricted,
    Performance,
    Efficiency,
};

struct ThreadPolicy {
    pid_t tid;
    CoreGroup group;
    s32 nice_value;
    bool has_nice;
};

struct CoreInfo {
    s64 weight;
    u64 midr;
    s32 cpu;
};

struct CpuTopologyState {
    std::mutex topology_mutex;
    cpu_set_t allowed{};
    cpu_set_t performance{};
    cpu_set_t efficiency{};
    bool separated = false;
    bool initialized = false;

    pid_t canary_tid = 0;
    cpu_set_t canary_mask{};
    bool canary_valid = false;

    std::atomic<s64> next_poll_ns{0};

    std::mutex policy_mutex;
    std::vector<ThreadPolicy> policies;
};

CpuTopologyState& State() {
    static CpuTopologyState* const state = new CpuTopologyState();
    return *state;
}

struct PolicyRegistration {
    ~PolicyRegistration() {
        const pid_t tid = gettid();
        ::Common::ADPF::RemoveCurrentThread();
        CpuTopologyState& state = State();
        std::scoped_lock lock{state.policy_mutex};
        std::erase_if(state.policies,
                      [tid](const ThreadPolicy& policy) { return policy.tid == tid; });
    }
};

thread_local PolicyRegistration t_policy_registration;

s32 PossibleCpuCount() {
    std::ifstream file("/sys/devices/system/cpu/possible");
    std::string list;
    if (file && std::getline(file, list) && !list.empty()) {
        s64 highest = -1;
        const char* cursor = list.c_str();
        while (*cursor != '\0') {
            char* end = nullptr;
            const s64 value = std::strtol(cursor, &end, 10);
            if (end == cursor) {
                break;
            }
            highest = (std::max)(highest, value);
            cursor = end;
            while (*cursor == '-' || *cursor == ',') {
                ++cursor;
            }
        }
        if (highest >= 0) {
            return static_cast<s32>((std::min<s64>)(highest + 1, CPU_SETSIZE));
        }
    }
    const s64 configured = sysconf(_SC_NPROCESSORS_CONF);
    if (configured > 0) {
        return static_cast<s32>((std::min<s64>)(configured, CPU_SETSIZE));
    }
    return static_cast<s32>((std::min<s64>)(std::thread::hardware_concurrency(), CPU_SETSIZE));
}

s64 ReadCpuScalar(s32 cpu, const char* node) {
    s64 value = 0;
    std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/" + node);
    if (!file || !(file >> value) || value <= 0) {
        return 0;
    }
    return value;
}

u64 ReadCpuMidr(s32 cpu) {
    u64 midr = 0;
    std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                       "/regs/identification/midr_el1");
    if (!file || !(file >> std::hex >> midr)) {
        return 0;
    }
    return midr;
}

std::vector<CoreInfo> CollectCores(const cpu_set_t& allowed, s32 total, const char* node,
                                   bool require_all) {
    std::vector<CoreInfo> cores;
    for (s32 cpu = 0; cpu < total; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) {
            continue;
        }
        const s64 weight = ReadCpuScalar(cpu, node);
        if (weight <= 0) {
            if (require_all) {
                return {};
            }
            LOG_WARNING(Common, "Could not read {} for CPU {}, treating it as an efficiency core",
                        node, cpu);
            continue;
        }
        cores.push_back(CoreInfo{weight, ReadCpuMidr(cpu), cpu});
    }
    return cores;
}

bool WeightsAreUniform(const std::vector<CoreInfo>& cores) {
    return std::all_of(cores.begin(), cores.end(),
                       [&](const CoreInfo& core) { return core.weight == cores.front().weight; });
}

bool MidrsAreDistinct(const std::vector<CoreInfo>& cores) {
    return std::none_of(cores.begin(), cores.end(),
                        [](const CoreInfo& core) { return core.midr == 0; }) &&
           std::any_of(cores.begin(), cores.end(),
                       [&](const CoreInfo& core) { return core.midr != cores.front().midr; });
}

void ComputeTopologyLocked(CpuTopologyState& state) {
    state.initialized = true;
    state.separated = false;
    CPU_ZERO(&state.allowed);
    CPU_ZERO(&state.performance);
    CPU_ZERO(&state.efficiency);

    if (sched_getaffinity(getpid(), sizeof(state.allowed), &state.allowed) != 0) {
        LOG_WARNING(Common, "Could not query process CPU affinity: {}",
                    ::Common::GetLastErrorMsg());
        return;
    }

    const s32 total = PossibleCpuCount();
    auto cores = CollectCores(state.allowed, total, "cpu_capacity", true);
    if (cores.empty() || (WeightsAreUniform(cores) && MidrsAreDistinct(cores))) {
        auto by_frequency = CollectCores(state.allowed, total, "cpufreq/cpuinfo_max_freq", false);
        if (!by_frequency.empty()) {
            cores = std::move(by_frequency);
        }
    }
    if (cores.empty()) {
        LOG_WARNING(Common, "Could not determine CPU topology, thread placement is disabled");
        return;
    }

    if (WeightsAreUniform(cores)) {
        if (MidrsAreDistinct(cores)) {
            LOG_WARNING(Common, "CPU clusters differ but rank identically, thread placement is "
                                "disabled");
        } else {
            LOG_INFO(Common, "CPU cores are symmetric, thread placement is disabled");
        }
        return;
    }

    std::sort(cores.begin(), cores.end(), [](const CoreInfo& lhs, const CoreInfo& rhs) {
        if (lhs.weight != rhs.weight) {
            return lhs.weight > rhs.weight;
        }
        return lhs.cpu < rhs.cpu;
    });

    const s64 fastest = cores.front().weight;
    size_t taken = 0;
    for (const auto& core : cores) {
        const bool fast_enough =
            core.weight * 100 >= fastest * ANDROID_PERFORMANCE_CAPACITY_PERCENT;
        if (!fast_enough && taken >= ANDROID_MINIMUM_PERFORMANCE_CORES) {
            break;
        }
        CPU_SET(core.cpu, &state.performance);
        ++taken;
    }
    if (taken == 0) {
        return;
    }

    for (s32 cpu = 0; cpu < total; ++cpu) {
        if (CPU_ISSET(cpu, &state.allowed) && !CPU_ISSET(cpu, &state.performance)) {
            CPU_SET(cpu, &state.efficiency);
        }
    }

    state.separated = CPU_COUNT(&state.efficiency) > 0;
    LOG_INFO(Common, "CPU topology: {} performance cores, {} efficiency cores, separation {}",
             CPU_COUNT(&state.performance), CPU_COUNT(&state.efficiency),
             state.separated ? "enabled" : "unavailable");
}

void EnsureTopologyLocked(CpuTopologyState& state) {
    if (!state.initialized) {
        ComputeTopologyLocked(state);
    }
}

void RefreshTopologyLocked(CpuTopologyState& state) {
    if (!state.initialized) {
        ComputeTopologyLocked(state);
        return;
    }
    cpu_set_t current;
    CPU_ZERO(&current);
    if (sched_getaffinity(getpid(), sizeof(current), &current) != 0) {
        return;
    }
    if (std::memcmp(&current, &state.allowed, sizeof(current)) != 0) {
        ComputeTopologyLocked(state);
    }
}

bool ApplyCoreGroupLocked(CpuTopologyState& state, pid_t tid, CoreGroup group,
                          bool* gone = nullptr) {
    const bool restrict_group = group != CoreGroup::Unrestricted && state.separated;
    const cpu_set_t* mask = &state.allowed;
    if (restrict_group) {
        mask = group == CoreGroup::Performance ? &state.performance : &state.efficiency;
    }
    if (CPU_COUNT(mask) == 0) {
        return false;
    }
    if (sched_setaffinity(tid, sizeof(*mask), mask) != 0) {
        if (gone != nullptr && errno == ESRCH) {
            *gone = true;
            return false;
        }
        LOG_WARNING(Common, "Could not restrict thread {} to its core group: {}", tid,
                    ::Common::GetLastErrorMsg());
        return false;
    }
    return true;
}

bool KernelPreservesRequestedAffinity() {
    utsname info{};
    if (uname(&info) != 0) {
        return false;
    }
    s32 major = 0;
    s32 minor = 0;
    if (std::sscanf(info.release, "%d.%d", &major, &minor) != 2) {
        return false;
    }
    return major > 6 || (major == 6 && minor >= 2);
}

void SnapshotCanaryLocked(CpuTopologyState& state) {
    state.canary_valid = false;
    if (!state.separated) {
        return;
    }
    for (const auto& policy : state.policies) {
        if (policy.group == CoreGroup::Unrestricted) {
            continue;
        }
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (sched_getaffinity(policy.tid, sizeof(mask), &mask) != 0) {
            continue;
        }
        state.canary_tid = policy.tid;
        state.canary_mask = mask;
        state.canary_valid = true;
        return;
    }
}

bool DueForPoll(CpuTopologyState& state) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    s64 next = state.next_poll_ns.load(std::memory_order_relaxed);
    if (now_ns < next) {
        return false;
    }
    return state.next_poll_ns.compare_exchange_strong(
        next, now_ns + ANDROID_POLICY_POLL_INTERVAL.count(), std::memory_order_relaxed);
}

ThreadPolicy& AcquirePolicyLocked(CpuTopologyState& state, pid_t tid) {
    for (auto& policy : state.policies) {
        if (policy.tid == tid) {
            return policy;
        }
    }
    return state.policies.emplace_back(ThreadPolicy{tid, CoreGroup::Unrestricted, 0, false});
}

void SetCurrentThreadCoreGroup(CoreGroup group) {
    const pid_t tid = gettid();
    (void)&t_policy_registration;

    CoreGroup effective = group;
    if (tid == getpid() && group != CoreGroup::Unrestricted) {
        LOG_WARNING(Common, "Refusing to place the main thread: the CPU topology is read from it");
        effective = CoreGroup::Unrestricted;
    }

    CpuTopologyState& state = State();
    std::scoped_lock topology_lock{state.topology_mutex};
    EnsureTopologyLocked(state);
    ApplyCoreGroupLocked(state, tid, effective);

    std::scoped_lock policy_lock{state.policy_mutex};
    AcquirePolicyLocked(state, tid).group = effective;
    if (!state.canary_valid) {
        SnapshotCanaryLocked(state);
    }
}

void RememberCurrentThreadNice(pid_t tid, s32 nice_value) {
    (void)&t_policy_registration;
    CpuTopologyState& state = State();
    std::scoped_lock lock{state.policy_mutex};
    ThreadPolicy& policy = AcquirePolicyLocked(state, tid);
    policy.nice_value = nice_value;
    policy.has_nice = true;
}
} // Anonymous namespace
#endif

#ifdef _WIN32
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/ranges.h>

namespace {
// With fewer P-core logical processors the busy threads (guest cores, GPU, GPUBack, VulkanWorker,
// HostTiming) would queue behind each other while the E-cores sit idle.
constexpr size_t WINDOWS_MINIMUM_PERFORMANCE_CPU_SETS = 12;

struct HybridCpuSets {
    std::vector<ULONG> performance;
    std::vector<ULONG> efficiency;
};

// The first lookup can run on a worker started before the logger, so the summary is kept and
// logged when the first performance thread is placed.
std::string cpu_sets_summary;

size_t CountDistinct(std::vector<u32> values) {
    std::sort(values.begin(), values.end());
    return static_cast<size_t>(std::unique(values.begin(), values.end()) - values.begin());
}

// The CPU sets of this process with their physical core; empty when they cannot be read.
std::vector<Common::LogicalProcessor> ReadCpuSetProcessors() {
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<u64> buffer((length + sizeof(u64) - 1) / sizeof(u64));
    if (length == 0 ||
        !GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                                    length, &length, GetCurrentProcess(), 0)) {
        LOG_WARNING(Common, "Could not read CPU sets, thread placement is disabled: {}",
                    ::Common::GetLastErrorMsg());
        return {};
    }

    std::vector<Common::LogicalProcessor> processors;
    const auto* const bytes = reinterpret_cast<const u8*>(buffer.data());
    for (ULONG offset = 0; offset < length;) {
        const auto& info = *reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(bytes + offset);
        if (info.Size == 0) {
            break;
        }
        if (info.Type == CpuSetInformation) {
            processors.push_back(Common::LogicalProcessor{
                .id = static_cast<u32>(info.CpuSet.Id),
                .core = (static_cast<u32>(info.CpuSet.Group) << 8) | info.CpuSet.CoreIndex,
                .efficiency_class = info.CpuSet.EfficiencyClass,
            });
        }
        offset += info.Size;
    }
    return processors;
}

const std::vector<Common::LogicalProcessor>& GetCpuSetProcessors() {
    static const std::vector<Common::LogicalProcessor> processors = ReadCpuSetProcessors();
    return processors;
}

// Only hybrid CPUs (more than one efficiency class) are placed. EDEN_CPU_SETS=0 disables it,
// EDEN_CPU_SETS=1 also places threads when there are few P-cores.
std::optional<HybridCpuSets> ReadHybridCpuSets() {
    const char* const env = std::getenv("EDEN_CPU_SETS");
    if (env != nullptr && env[0] == '0') {
        cpu_sets_summary = "CPU set placement disabled by EDEN_CPU_SETS=0";
        return std::nullopt;
    }

    const std::vector<Common::LogicalProcessor>& processors = GetCpuSetProcessors();
    if (processors.empty()) {
        return std::nullopt;
    }
    u8 highest_class = 0;
    u8 lowest_class = 0xFF;
    for (const Common::LogicalProcessor& processor : processors) {
        highest_class = (std::max)(highest_class, processor.efficiency_class);
        lowest_class = (std::min)(lowest_class, processor.efficiency_class);
    }
    if (highest_class == lowest_class) {
        cpu_sets_summary = fmt::format(
            "CPU sets: {} logical processors of one class, thread placement is disabled",
            processors.size());
        return std::nullopt;
    }

    HybridCpuSets cpu_sets;
    std::vector<u32> performance_cores;
    std::vector<u32> efficiency_cores;
    for (const Common::LogicalProcessor& processor : processors) {
        const bool performance = processor.efficiency_class == highest_class;
        (performance ? cpu_sets.performance : cpu_sets.efficiency).push_back(processor.id);
        (performance ? performance_cores : efficiency_cores).push_back(processor.core);
    }
    const bool enabled = (env != nullptr && env[0] == '1') ||
                         cpu_sets.performance.size() >= WINDOWS_MINIMUM_PERFORMANCE_CPU_SETS;
    cpu_sets_summary = fmt::format(
        "CPU sets: {} performance cores ({} logical), {} efficiency cores ({} logical), "
        "thread placement {}",
        CountDistinct(std::move(performance_cores)), cpu_sets.performance.size(),
        CountDistinct(std::move(efficiency_cores)), cpu_sets.efficiency.size(),
        enabled ? "enabled" : "disabled (few performance cores, EDEN_CPU_SETS=1 forces it)");
    if (!enabled) {
        return std::nullopt;
    }
    return cpu_sets;
}

const std::optional<HybridCpuSets>& GetHybridCpuSets() {
    static const std::optional<HybridCpuSets> cpu_sets = ReadHybridCpuSets();
    return cpu_sets;
}

void LogCpuSetsOnce() {
    static std::once_flag logged;
    std::call_once(logged, [] {
        GetHybridCpuSets();
        if (!cpu_sets_summary.empty()) {
            LOG_INFO(Common, "{}", cpu_sets_summary);
        }
    });
}

// Selected CPU sets are a soft preference, unlike affinity masks.
void SelectCurrentThreadCpuSets(const std::vector<ULONG>& ids) {
    if (!SetThreadSelectedCpuSets(GetCurrentThread(), ids.empty() ? nullptr : ids.data(),
                                  static_cast<ULONG>(ids.size()))) {
        LOG_WARNING(Common, "Could not select CPU sets for the current thread: {}",
                    ::Common::GetLastErrorMsg());
    }
}

// EXECUTION_SPEED with a zero state keeps the thread on high QoS; no control bits hand the
// choice back to Windows.
void SetCurrentThreadPowerThrottling(ULONG control_mask) {
    THREAD_POWER_THROTTLING_STATE state{
        .Version = THREAD_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = control_mask,
        .StateMask = 0,
    };
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
}

void ResetCurrentThreadPlacement() {
    if (GetHybridCpuSets()) {
        SelectCurrentThreadCpuSets({});
        SetCurrentThreadPowerThrottling(0);
    }
}

// EDEN_NX2_EVENT_WAIT_LEGACY=1 brings back the timed Event wait from before zortos293/Eglantine
// d7ce3d8c08: a MWAITX or UMWAIT wake the signal did not cause returned false before the deadline,
// and the fallback returned true at the deadline.
bool LegacyEventWait() {
    static const bool legacy = [] {
        const char* value = std::getenv("EDEN_NX2_EVENT_WAIT_LEGACY");
        return value != nullptr && value[0] == '1';
    }();
    return legacy;
}
} // Anonymous namespace
#endif

#include "common/cpu_features.h"
#ifdef ARCHITECTURE_x86_64
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#include "common/x64/rdtsc.h"
#endif

namespace Common {

void ApplyProcessPrioritySwitch() {
#ifdef _WIN32
    // EDEN_NX2_HIGH_PRIORITY=1: the whole process runs at the high priority class, as Selevet's
    // performance builds 1-6 did by default. Other programs then yield the CPU to the emulator.
    const char* value = std::getenv("EDEN_NX2_HIGH_PRIORITY");
    if (value != nullptr && value[0] == '1') {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }
#endif
}

void SetCurrentThreadPriority(ThreadPriority new_priority) {
#ifdef _WIN32
    int windows_priority = [&]() {
        switch (new_priority) {
        case ThreadPriority::Low: return THREAD_PRIORITY_BELOW_NORMAL;
        case ThreadPriority::Normal: return THREAD_PRIORITY_NORMAL;
        case ThreadPriority::High: return THREAD_PRIORITY_ABOVE_NORMAL;
        case ThreadPriority::VeryHigh: return THREAD_PRIORITY_HIGHEST;
        case ThreadPriority::Critical: return THREAD_PRIORITY_TIME_CRITICAL;
        default: return THREAD_PRIORITY_NORMAL;
        }
    }();
    SetThreadPriority(GetCurrentThread(), windows_priority);
#elif defined(__HAIKU__)
    // TODO: We have priorities for 3D rendering applications - may help lavapipe?
    int priority = [&]() {
        switch (new_priority) {
        case ThreadPriority::Low: return B_LOW_PRIORITY;
        case ThreadPriority::Normal: return B_NORMAL_PRIORITY;
        case ThreadPriority::High: return B_DISPLAY_PRIORITY;
        case ThreadPriority::VeryHigh: return B_URGENT_DISPLAY_PRIORITY;
        case ThreadPriority::Critical: return B_URGENT_PRIORITY;
        default: return B_NORMAL_PRIORITY;
        }
    }();
    set_thread_priority(find_thread(NULL), priority);
#elif defined(__ANDROID__)
    const int nice_value = NiceValueForPriority(new_priority);
    const pid_t tid = gettid();
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice_value) != 0) {
        LOG_WARNING(Common, "Could not set thread nice value to {}: {}", nice_value,
                    GetLastErrorMsg());
        return;
    }
    RememberCurrentThreadNice(tid, nice_value);
#elif defined(__linux__)
    const int nice_value = NiceValueForPriority(new_priority);
    if (setpriority(PRIO_PROCESS, 0, nice_value) != 0) {
        LOG_DEBUG(Common, "Could not set thread nice value to {}: {}", nice_value,
                  GetLastErrorMsg());
    }
#else
    const s32 max_prio = sched_get_priority_max(SCHED_OTHER);
    const s32 min_prio = sched_get_priority_min(SCHED_OTHER);
    if (max_prio > min_prio) {
        const u32 level = (std::min)(static_cast<u32>(new_priority), 4U);
        sched_param params{};
        params.sched_priority =
            min_prio + static_cast<s32>(static_cast<u32>(max_prio - min_prio) * level) / 4;
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &params);
    }
#endif
}

u32 GetCurrentThreadBoostId() {
#ifdef _WIN32
    return static_cast<u32>(GetCurrentThreadId());
#else
    return 0;
#endif
}

bool BoostThreadPriority([[maybe_unused]] u32 thread_id, [[maybe_unused]] s32& previous) {
#ifdef _WIN32
    const HANDLE thread = OpenThread(THREAD_SET_LIMITED_INFORMATION |
                                         THREAD_QUERY_LIMITED_INFORMATION,
                                     FALSE, static_cast<DWORD>(thread_id));
    if (thread == nullptr) {
        return false;
    }
    previous = GetThreadPriority(thread);
    const bool boosted = previous != THREAD_PRIORITY_ERROR_RETURN &&
                         previous < THREAD_PRIORITY_HIGHEST &&
                         SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST) != FALSE;
    CloseHandle(thread);
    return boosted;
#else
    return false;
#endif
}

void RestoreCurrentThreadPriority([[maybe_unused]] s32 previous) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), previous);
#endif
}

void SetCurrentThreadName(const char* name) {
#ifdef _MSC_VER
    // Sets the debugger-visible name of the current thread.
    if (auto pf = (decltype(&SetThreadDescription))(void*)GetProcAddress(GetModuleHandle(TEXT("KernelBase.dll")), "SetThreadDescription"); pf)
        pf(GetCurrentThread(), UTF8ToUTF16W(name).data()); // Windows 10+
    else
        ; // No-op
#elif  defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__HAIKU__)
    rename_thread(find_thread(NULL), name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__) || defined(__CYGWIN__) || defined(__sun__) || defined(__glibc__) || defined(__managarm__)
    int ret = pthread_setname_np(pthread_self(), name);
    if (ret == ERANGE) {
        // Linux limits thread names to 15 characters and will outright reject any
        // attempt to set a longer name with ERANGE.
        char buf[16];
        size_t const len = std::min<size_t>(std::strlen(name), sizeof(buf) - 1);
        std::memcpy(buf, name, len);
        buf[len] = '\0';
        pthread_setname_np(pthread_self(), buf);
    }
#elif defined(_WIN32)
    // MinGW with the POSIX threading model does not support pthread_setname_np
    // See for reference
    // https://gitlab.freedesktop.org/mesa/mesa/-/blame/main/src/util/u_thread.c?ref_type=heads#L75
    (void)name;
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void SetCurrentThreadToPerformanceCores() {
#if defined(__ANDROID__)
    if (ADPF::AddCurrentThread(ADPF::Session::Render)) {
        SetCurrentThreadCoreGroup(CoreGroup::Unrestricted);
        return;
    }
    SetCurrentThreadCoreGroup(CoreGroup::Performance);
#elif defined(_WIN32)
    LogCpuSetsOnce();
    if (const auto& cpu_sets = GetHybridCpuSets()) {
        SelectCurrentThreadCpuSets(cpu_sets->performance);
        SetCurrentThreadPowerThrottling(THREAD_POWER_THROTTLING_EXECUTION_SPEED);
    }
#endif
}

void PinCurrentThread(PinnedRole role) {
#ifdef _WIN32
    // EDEN_NX2_PIN_THREADS=1 (off by default). Frizti measured Lake Hylia 29 -> 54 fps in its
    // worst window on a Ryzen 7 7800X3D. On a 6 P-core + 4 E-core laptop the earlier version, a
    // hard affinity mask, made Ocarina ~2% slower and Mario Kart World stall for 5.5 s at a time.
    // CPU sets replace the ones SetCurrentThreadToPerformanceCores selected, so it runs first.
    static const char* const setting = std::getenv("EDEN_NX2_PIN_THREADS");
    if (setting == nullptr || setting[0] != '1') {
        return;
    }
    struct PinnedCpuSets {
        std::vector<ULONG> front;
        std::vector<ULONG> back;
        std::vector<ULONG> guest;
    };
    static const PinnedCpuSets pinned = [] {
        const GpuThreadPinning plan = PlanGpuThreadPinning(GetCpuSetProcessors());
        if (plan.front.empty()) {
            LOG_WARNING(Common,
                        "GPU thread pinning skipped: {} physical performance cores, needs {}",
                        plan.performance_cores, MinCoresForGpuThreadPinning);
            return PinnedCpuSets{};
        }
        LOG_INFO(Common,
                 "GPU thread pinning: front on CPU sets {}, back on {}, guest cores on {} (the "
                 "other {} of {} physical performance cores)",
                 fmt::join(plan.front, " "), fmt::join(plan.back, " "),
                 fmt::join(plan.guest, " "), plan.performance_cores - 2, plan.performance_cores);
        return PinnedCpuSets{{plan.front.begin(), plan.front.end()},
                             {plan.back.begin(), plan.back.end()},
                             {plan.guest.begin(), plan.guest.end()}};
    }();
    const std::vector<ULONG>& ids = role == PinnedRole::GpuFront  ? pinned.front
                                    : role == PinnedRole::GpuBack ? pinned.back
                                                                  : pinned.guest;
    if (!ids.empty()) {
        SelectCurrentThreadCpuSets(ids);
    }
#endif
}

void SetCurrentThreadToEfficiencyCores() {
#if defined(__ANDROID__)
    if (ADPF::AddCurrentThread(ADPF::Session::Background)) {
        SetCurrentThreadCoreGroup(CoreGroup::Unrestricted);
        return;
    }
    SetCurrentThreadCoreGroup(CoreGroup::Efficiency);
#elif defined(_WIN32)
    if (const auto& cpu_sets = GetHybridCpuSets()) {
        SelectCurrentThreadCpuSets(cpu_sets->efficiency);
    }
#endif
}

void SetCurrentThreadToBackgroundWork() {
#if defined(__ANDROID__)
    ADPF::AddCurrentThread(ADPF::Session::Background);
    SetCurrentThreadCoreGroup(CoreGroup::Unrestricted);
#elif defined(_WIN32)
    // Shader builders stay unrestricted (as on Android) so first-run builds can use every core.
    ResetCurrentThreadPlacement();
#endif
}

void SetCurrentThreadToAllCores() {
#if defined(__ANDROID__)
    ADPF::RemoveCurrentThread();
    SetCurrentThreadCoreGroup(CoreGroup::Unrestricted);
#elif defined(_WIN32)
    ResetCurrentThreadPlacement();
#endif
}

void RefreshThreadPolicies() {
#if defined(__ANDROID__)
    CpuTopologyState& state = State();
    std::scoped_lock topology_lock{state.topology_mutex};
    RefreshTopologyLocked(state);

    std::scoped_lock policy_lock{state.policy_mutex};
    std::erase_if(state.policies, [&state](const ThreadPolicy& policy) {
        bool gone = false;
        if (policy.has_nice &&
            setpriority(PRIO_PROCESS, static_cast<id_t>(policy.tid), policy.nice_value) != 0 &&
            errno == ESRCH) {
            gone = true;
        }
        if (!gone) {
            ApplyCoreGroupLocked(state, policy.tid, policy.group, &gone);
        }
        return gone;
    });
    SnapshotCanaryLocked(state);
#endif
}

void PollThreadPolicies() {
#if defined(__ANDROID__)
    static const bool needed = !KernelPreservesRequestedAffinity();
    if (!needed) {
        return;
    }

    CpuTopologyState& state = State();
    if (!DueForPoll(state)) {
        return;
    }

    pid_t tid;
    cpu_set_t expected;
    {
        std::scoped_lock lock{state.topology_mutex};
        if (!state.canary_valid) {
            return;
        }
        tid = state.canary_tid;
        expected = state.canary_mask;
    }

    cpu_set_t current;
    CPU_ZERO(&current);
    if (sched_getaffinity(tid, sizeof(current), &current) == 0 &&
        std::memcmp(&current, &expected, sizeof(current)) == 0) {
        return;
    }
    RefreshThreadPolicies();
#endif
}

#ifdef ARCHITECTURE_x86_64
// On Linux and UNIX systems, a futex would nominally be used to cover the costs
// the idea is that it's intuitivelly cheaper to use a direct instruction as opposed to a full futex call
// the underlying libc++ implementation uses pthread_cond_timedwait which MAY invoke a futex
// Let's pretend the OS is too expensive to jump into, and avoid ANY context switches
// this should *IN THEORY* lower CPU usage while just waiting for stuff effectively
// For windows the minimal quanta resolution is about 500us, and normal CRT cond var is 1.5ms(?)
// so may as well avoid that too
// Let's just give ALL platforms the same mechanisms (almost) for when they have umonitor OR waitpkg
#ifdef __clang__
__attribute__((target("waitpkg,mwaitx")))
#elif defined(__GNUC__)
#pragma GCC target("waitpkg")
#pragma GCC target("mwaitx")
#endif
bool Event::WaitFor(const std::chrono::nanoseconds time) {
#ifdef _WIN32
    const bool legacy = LegacyEventWait();
    const auto consume_signal = [this] {
        bool expected = true;
        return is_set.compare_exchange_strong(expected, false, std::memory_order_acq_rel);
    };
    if (consume_signal()) {
        return true;
    }
    if (time <= std::chrono::nanoseconds::zero()) {
        return false;
    }
    auto const start = Common::X64::FencedRDTSC();
    auto const& caps = Common::g_cpu_caps;
    // NsToTicks overflows for timeouts like nanoseconds::max, and the wrapped absolute deadline
    // ended the wait at once (UMWAIT takes it as is). Ten years is as good as forever.
    constexpr std::chrono::nanoseconds max_wait = std::chrono::hours{24 * 365 * 10};
    [[maybe_unused]] auto const end =
        start + Common::g_wall_clock.NsToTicks((std::min)(time, max_wait));
    if (caps.monitorx) {
        while (true) {
            // Armed monitor, as per manual, MWAITX must be conditional if the condition isn't satisfied
            // to prevent a race condition.
            _mm_monitorx(reinterpret_cast<u64*>(std::addressof(is_set)), 0, 0);
            if (!is_set.load()) {
                // RDTSC may be fenced here due to atomic load
#ifdef _MSC_VER
                auto const now = __rdtsc();
#else
                auto const now = _rdtsc();
#endif
                if (end > now) {
                    // Clamp before narrowing: min<u32> truncated the remaining 64-bit
                    // interval first (and could produce zero, disabling the timer).
                    const u32 cycles = static_cast<u32>((std::min)(
                        u64{(std::numeric_limits<u32>::max)()}, end - now));
                    // See here: https://github.com/torvalds/linux/blob/948a64995aca6820abefd17f1a4258f5835c5ad9/arch/x86/lib/delay.c#L93
                    // MWAITX accepts a 32-bit input timer which determines the total number of cycles to wait for
                    // NOT THE TOTAL ABSOLUTE TSC VALUE, it's just a delta
                    // BIT[1] = use a timer
                    // Hint = 0: Use C1 state when sleepy (means slower wakeup but better savings)
                    _mm_mwaitx(1 << 1, 0u, cycles);
                    if (legacy && !is_set.load())
                        return false;
                } else
                    return false; //timeout
            }
            // Interrupts and unrelated writes to the monitored cache line can wake
            // MWAITX. Only the event predicate or the actual deadline ends the wait.
            if (consume_signal())
                return true;
        }
    } else if (caps.waitpkg) {
        // #UD If CPUID.7.0:ECX.WAITPKG[bit 5]=0.
        while (true) {
            _umonitor(std::addressof(is_set));
            // Do not issue a failed compare-exchange between UMONITOR and UMWAIT:
            // the RMW itself can invalidate the monitored line and prevent sleeping.
            if (is_set.load()) {
                if (consume_signal()) {
                    return true;
                }
                continue;
            }
            if (Common::X64::FencedRDTSC() >= end) {
                return false;
            }
            // CF reports the OS-imposed wait limit, not whether our event was
            // signalled. Spurious/limited wakes must recheck predicate and time.
            const bool os_limit = _umwait(0, end) != 0; // umwait is absolute time
            if (legacy && !os_limit) {
                return false;
            }
        }
    } else {
#ifdef _MSC_VER
        while (!is_set.load() && end > __rdtsc())
            Common::Windows::SleepForOneTick();
#else
        while (!is_set.load() && end > _rdtsc())
            Common::Windows::SleepForOneTick();
#endif
        if (legacy) {
            if (is_set.load())
                Reset();
            return true;
        }
        return consume_signal();
    }
#else
    std::unique_lock lk{mutex};
    if (!condvar.wait_until(lk, EventDeadline(time), [this] { return is_set.load(); }))
        return false;
    is_set = false;
    return true;
#endif
}
#else
bool Event::WaitFor(const std::chrono::nanoseconds time) {
#ifdef _WIN32
    const auto consume_signal = [this] {
        bool expected = true;
        return is_set.compare_exchange_strong(expected, false, std::memory_order_acq_rel);
    };
    if (consume_signal()) {
        return true;
    }
    if (time <= std::chrono::nanoseconds::zero()) {
        return false;
    }
    auto const end = Common::g_wall_clock.GetTimeNS() + time;
    while (!is_set.load() && end > Common::g_wall_clock.GetTimeNS())
        Common::Windows::SleepForOneTick();
    if (LegacyEventWait()) {
        if (is_set.load())
            Reset();
        return true;
    }
    return consume_signal();
#else
    std::unique_lock lk{mutex};
    if (!condvar.wait_until(lk, EventDeadline(time), [this] { return is_set.load(); }))
        return false;
    is_set = false;
    return true;
#endif
}
#endif

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

Event::~Event() {
    if (sleep_wake) {
        CloseHandle(sleep_wake);
    }
}

void Event::WakeSleeper() {
    SetEvent(sleep_wake);
}

// Timer and wake event as in Eglantine #538 (a51de161ed, pretend7979): a high-resolution timer
// can expire between timer-resolution ticks.
bool Event::SleepFor(const std::chrono::nanoseconds time) {
    struct Timer {
        Timer() {
            handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
            if (!handle) {
                handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            }
        }
        ~Timer() {
            if (handle) {
                CloseHandle(handle);
            }
        }
        HANDLE handle;
    };
    static thread_local Timer timer;
    using TimerTicks = std::chrono::duration<s64, std::ratio<1, 10'000'000>>;
    // As in WaitFor: ten years is as good as forever, and keeps the deadline from overflowing.
    constexpr std::chrono::nanoseconds max_wait = std::chrono::hours{24 * 365 * 10};

    std::unique_lock lk{mutex};
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>((std::min)(time, max_wait));
    if (!sleep_wake) {
        sleep_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    while (!is_set.load()) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::nanoseconds::zero()) {
            return false;
        }
        LARGE_INTEGER due{};
        due.QuadPart = -std::chrono::ceil<TimerTicks>(remaining).count();
        if (!sleep_wake || !timer.handle ||
            !SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0)) {
            condvar.wait_until(lk, deadline, [this] { return is_set.load(); });
            continue;
        }
        // Set() signals sleep_wake while a sleeper waits; the timeout only backs up the timer.
        ++sleepers;
        lk.unlock();
        const HANDLE handles[]{static_cast<HANDLE>(sleep_wake), timer.handle};
        const s64 backstop = std::chrono::ceil<std::chrono::milliseconds>(remaining).count() + 1;
        const DWORD result = WaitForMultipleObjects(
            2, handles, FALSE, static_cast<DWORD>((std::min<s64>)(backstop, INFINITE - 1)));
        lk.lock();
        --sleepers;
        if (result == WAIT_FAILED) {
            condvar.wait_until(lk, deadline, [this] { return is_set.load(); });
        }
    }
    is_set = false;
    return true;
}
#else
bool Event::SleepFor(const std::chrono::nanoseconds time) {
    return WaitFor(time);
}
#endif

} // namespace Common
