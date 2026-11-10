// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/arm/dynarmic/nx2_jit_warm.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/result.h"
#include "core/memory.h"

namespace Core::Nx2JitWarm {

namespace {

// Dynarmic's A64 location hash: the PC in bits 0-55, FPCR mode bits in 56 and 59-63, single
// stepping in 57 (A64::LocationDescriptor::UniqueHash).
constexpr u64 PcMask = (u64{1} << 56) - 1;
constexpr u64 SingleStepBit = u64{1} << 57;
// A saved key: region index in bits 48-63, the hash's top byte in 40-47, the offset in 0-39.
constexpr u64 OffsetMask = (u64{1} << 40) - 1;
constexpr std::size_t MaxPerCore = 600000;
constexpr std::size_t RegionHashBytes = 64 * 1024;
constexpr u32 FileMagic = 0x4D574A4E; // "NJWM"
constexpr u32 FileVersion = 1;
constexpr auto WriteInterval = std::chrono::seconds{60};
constexpr std::size_t NumCores = Core::Hardware::NUM_CPU_CORES;

struct Region {
    u64 base;
    u64 size;
    u64 hash;
};

struct State {
    std::mutex mutex;
    bool loaded{};
    u64 program_id{};
    const Kernel::KProcess* process{};
    int attached{};
    std::array<std::shared_future<void>, NumCores> warm_ups; ///< Each core's warm-up
    std::vector<Region> regions; ///< This boot's executable code regions, by address
    std::array<std::vector<u64>, NumCores> boot_lists; ///< Taken by each core's Attach
    std::size_t loaded_keys{};
    // Owned by the write job (one at a time) and by the last Detach once the job is done.
    std::array<std::unordered_set<u64>, NumCores> keys;
    std::vector<std::pair<std::size_t, std::vector<u64>>> pending;
    std::future<void> writer;
    std::chrono::steady_clock::time_point last_write{};
};

State& GetState() {
    static State state;
    return state;
}

std::filesystem::path ListPath(u64 program_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "nx2_jit" /
           fmt::format("{:016X}.bin", program_id);
}

/// The process's read-execute code regions (its modules' text), with a hash of their start so a
/// region of another build is not taken for this one.
std::vector<Region> ScanRegions(Kernel::KProcess* process) {
    std::vector<Region> regions;
    auto& page_table = process->GetPageTable();
    auto& memory = process->GetMemory();
    std::vector<u8> head(RegionHashBytes);
    u64 address = 0;
    while (true) {
        Kernel::KMemoryInfo mem_info{};
        Kernel::Svc::PageInfo page_info{};
        if (R_FAILED(page_table.QueryInfo(std::addressof(mem_info), std::addressof(page_info),
                                          address))) {
            break;
        }
        const auto info = mem_info.GetSvcMemoryInfo();
        if (info.permission == Kernel::Svc::MemoryPermission::ReadExecute &&
            (info.state == Kernel::Svc::MemoryState::Code ||
             info.state == Kernel::Svc::MemoryState::AliasCode)) {
            const std::size_t bytes = static_cast<std::size_t>(
                (std::min<u64>)(info.size, RegionHashBytes));
            if (memory.ReadBlock(info.base_address, head.data(), bytes)) {
                const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(head.data()),
                                                    bytes) ^
                                 info.size;
                regions.push_back({info.base_address, info.size, hash});
            }
        }
        const u64 next = info.base_address + info.size;
        if (next <= address) {
            break;
        }
        address = next;
    }
    return regions;
}

std::optional<u64> ToKey(const std::vector<Region>& regions, u64 location) {
    if ((location & SingleStepBit) != 0) {
        return std::nullopt;
    }
    const u64 pc = location & PcMask;
    const auto it = std::upper_bound(regions.begin(), regions.end(), pc,
                                     [](u64 value, const Region& region) {
                                         return value < region.base;
                                     });
    if (it == regions.begin()) {
        return std::nullopt;
    }
    const Region& region = *(it - 1);
    const u64 offset = pc - region.base;
    if (offset >= region.size || offset > OffsetMask) {
        return std::nullopt;
    }
    const u64 index = static_cast<u64>(it - 1 - regions.begin());
    return (index << 48) | ((location >> 56) << 40) | offset;
}

u64 FromKey(const std::vector<Region>& regions, u64 key) {
    const Region& region = regions[key >> 48];
    return (region.base + (key & OffsetMask)) | (((key >> 40) & 0xff) << 56);
}

/// Reads the list and keeps the keys whose region is in this boot, renumbered to this boot's
/// regions. Holds state.mutex.
void LoadList(State& state) {
    std::ifstream file{ListPath(state.program_id), std::ios::binary};
    if (!file) {
        return;
    }
    const auto read = [&file](auto& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
        return static_cast<bool>(file);
    };
    u32 magic{}, version{}, region_count{};
    if (!read(magic) || !read(version) || magic != FileMagic || version != FileVersion ||
        !read(region_count) || region_count > 4096) {
        LOG_WARNING(Core_ARM, "nx2: JIT warm list for {:016X} is unreadable; starting a new one",
                    state.program_id);
        return;
    }
    // Saved region -> this boot's region (same size and start), or none.
    std::vector<int> mapping(region_count, -1);
    std::vector<bool> taken(state.regions.size());
    for (u32 i = 0; i < region_count; ++i) {
        u64 size{}, hash{};
        if (!read(size) || !read(hash)) {
            return;
        }
        for (std::size_t j = 0; j < state.regions.size(); ++j) {
            if (!taken[j] && state.regions[j].size == size && state.regions[j].hash == hash) {
                mapping[i] = static_cast<int>(j);
                taken[j] = true;
                break;
            }
        }
    }
    u32 core_count{};
    if (!read(core_count)) {
        return;
    }
    std::size_t total = 0, kept = 0;
    for (u32 core = 0; core < core_count; ++core) {
        u32 count{};
        if (!read(count) || count > 2 * MaxPerCore) {
            break;
        }
        std::vector<u64> saved(count);
        file.read(reinterpret_cast<char*>(saved.data()),
                  static_cast<std::streamsize>(count * sizeof(u64)));
        if (!file) {
            break;
        }
        total += count;
        if (core >= NumCores) {
            continue;
        }
        auto& keys = state.keys[core];
        auto& list = state.boot_lists[core];
        keys.reserve(count);
        list.reserve(count);
        for (const u64 key : saved) {
            const u64 region = key >> 48;
            if (region >= region_count || mapping[region] < 0 || keys.size() >= MaxPerCore) {
                continue;
            }
            const u64 renumbered = (static_cast<u64>(mapping[region]) << 48) |
                                   (key & ((u64{1} << 48) - 1));
            if (keys.insert(renumbered).second) {
                list.push_back(FromKey(state.regions, renumbered));
                ++kept;
            }
        }
    }
    state.loaded_keys = kept;
    LOG_INFO(Core_ARM,
             "nx2: JIT warm list for {:016X}: {} of {} blocks are in this boot's code ({} of {} "
             "code regions match)",
             state.program_id, kept, total,
             std::count_if(mapping.begin(), mapping.end(), [](int value) { return value >= 0; }),
             region_count);
}

/// Merges the queued blocks and writes the list. Only one runs at a time.
void WriteList(State& state) {
    std::vector<std::pair<std::size_t, std::vector<u64>>> pending;
    {
        std::scoped_lock lock{state.mutex};
        pending.swap(state.pending);
    }
    const auto start = std::chrono::steady_clock::now();
    std::size_t added = 0;
    for (auto& [core, locations] : pending) {
        auto& keys = state.keys[core];
        for (const u64 location : locations) {
            if (keys.size() >= MaxPerCore) {
                break;
            }
            if (const auto key = ToKey(state.regions, location); key && keys.insert(*key).second) {
                ++added;
            }
        }
    }
    if (added == 0) {
        return;
    }
    const auto path = ListPath(state.program_id);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temporary = path;
    temporary += ".tmp";
    std::size_t total = 0;
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        if (!file) {
            LOG_WARNING(Core_ARM, "nx2: could not write the JIT warm list {}", path.string());
            return;
        }
        const auto write = [&file](const auto& value) {
            file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        write(FileMagic);
        write(FileVersion);
        write(static_cast<u32>(state.regions.size()));
        for (const Region& region : state.regions) {
            write(region.size);
            write(region.hash);
        }
        write(static_cast<u32>(NumCores));
        std::vector<u64> keys;
        for (const auto& set : state.keys) {
            keys.assign(set.begin(), set.end());
            std::sort(keys.begin(), keys.end());
            write(static_cast<u32>(keys.size()));
            file.write(reinterpret_cast<const char*>(keys.data()),
                       static_cast<std::streamsize>(keys.size() * sizeof(u64)));
            total += keys.size();
        }
        if (!file) {
            LOG_WARNING(Core_ARM, "nx2: could not write the JIT warm list {}", path.string());
            return;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        LOG_WARNING(Core_ARM, "nx2: could not replace the JIT warm list {}: {}", path.string(),
                    ec.message());
        return;
    }
    LOG_INFO(Core_ARM, "nx2: JIT warm list saved: {} blocks ({} new) in {} ms", total, added,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count());
}

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_JIT_WARM");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

bool Attach(Kernel::KProcess* process, std::size_t core, const WarmUpFn& warm_up) {
    if (!Enabled() || process == nullptr || core >= NumCores) {
        return false;
    }
    State& state = GetState();
    std::shared_future<void> own;
    {
        std::scoped_lock lock{state.mutex};
        const u64 program_id = process->GetProgramId();
        if (state.loaded && (state.program_id != program_id || state.process != process)) {
            return false;
        }
        if (!state.loaded) {
            state.loaded = true;
            state.program_id = program_id;
            state.process = process;
            state.regions = ScanRegions(process);
            state.last_write = std::chrono::steady_clock::now();
            LoadList(state);
            // Every core's JIT is idle until its first run, which waits for this: warm them all
            // up at once instead of each when it first runs.
            state.attached = static_cast<int>(NumCores);
            for (std::size_t index = 0; index < NumCores; ++index) {
                state.warm_ups[index] =
                    std::async(std::launch::async,
                               [warm_up, index,
                                list = std::exchange(state.boot_lists[index], {})]() mutable {
                                   if (!warm_up(index, std::move(list))) {
                                       Detach(index, {});
                                   }
                               })
                        .share();
            }
        }
        own = state.warm_ups[core];
    }
    if (own.valid()) {
        own.wait();
    }
    return true;
}

void Wait(const Kernel::KProcess* process, std::size_t core) {
    if (!Enabled() || core >= NumCores) {
        return;
    }
    State& state = GetState();
    std::shared_future<void> own;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.loaded || state.process != process) {
            return;
        }
        own = state.warm_ups[core];
    }
    if (own.valid()) {
        own.wait();
    }
}

void Add(std::size_t core, std::vector<u64>&& locations) {
    if (locations.empty() || core >= NumCores) {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.loaded) {
        return;
    }
    state.pending.emplace_back(core, std::move(locations));
    const auto now = std::chrono::steady_clock::now();
    const bool idle = !state.writer.valid() ||
                      state.writer.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
    if (idle && now - state.last_write >= WriteInterval) {
        state.last_write = now;
        state.writer = std::async(std::launch::async, [&state] { WriteList(state); });
    }
}

void Detach(std::size_t core, std::vector<u64>&& locations) {
    if (core >= NumCores) {
        return;
    }
    State& state = GetState();
    std::future<void> writer;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.loaded || state.attached == 0) {
            return;
        }
        if (!locations.empty()) {
            state.pending.emplace_back(core, std::move(locations));
        }
        if (--state.attached != 0) {
            return;
        }
        writer = std::move(state.writer);
    }
    if (writer.valid()) {
        writer.wait();
    }
    WriteList(state);
    std::scoped_lock lock{state.mutex};
    state.loaded = false;
    state.program_id = 0;
    state.process = nullptr;
    state.warm_ups = {};
    state.regions.clear();
    state.boot_lists = {};
    state.loaded_keys = 0;
    for (auto& keys : state.keys) {
        keys = {};
    }
    state.pending.clear();
}

} // namespace Core::Nx2JitWarm
