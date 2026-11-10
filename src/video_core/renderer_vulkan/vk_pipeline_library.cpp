// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <exception>
#include <vector>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_pipeline_library.h"

namespace Vulkan {

namespace {
constexpr std::array<const char*, 2> PART_NAMES{"pre-rasterization", "fragment"};

double Ms(u64 ns) {
    return static_cast<double>(ns) / 1'000'000.0;
}
} // Anonymous namespace

void GraphicsPipelineLibraryCache::Stat::Add(std::chrono::nanoseconds time) {
    const u64 ns = static_cast<u64>(time.count());
    count.fetch_add(1, std::memory_order_relaxed);
    total_ns.fetch_add(ns, std::memory_order_relaxed);
    u64 previous = max_ns.load(std::memory_order_relaxed);
    while (previous < ns &&
           !max_ns.compare_exchange_weak(previous, ns, std::memory_order_relaxed)) {
    }
}

bool GplOptimizeEnabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_GPL_OPTIMIZE");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

std::shared_ptr<const GplLibrary> GraphicsPipelineLibraryCache::GetOrCreate(
    GplPart part, const std::string& key, const Builder& build, u64 name) {
    std::shared_ptr<Entry> entry;
    bool owner = false;
    {
        std::scoped_lock lock{mutex};
        auto [it, inserted] = entries.try_emplace(key);
        if (inserted) {
            it->second = std::make_shared<Entry>();
            lru.push_front(&it->first);
            it->second->lru = lru.begin();
            owner = true;
        } else {
            lru.splice(lru.begin(), lru, it->second->lru);
        }
        entry = it->second;
    }
    const size_t index = static_cast<size_t>(part);
    if (!owner) {
        // Another worker may be building it: wait for it instead of building it twice.
        std::unique_lock lock{entry->mutex};
        if (!entry->done) {
            library_waits[index].fetch_add(1, std::memory_order_relaxed);
            entry->condition.wait(lock, [&entry] { return entry->done; });
        }
        library_hits[index].fetch_add(1, std::memory_order_relaxed);
        return entry->library;
    }
    // Built outside the cache's lock: other libraries build at the same time on other workers.
    const auto begin = std::chrono::steady_clock::now();
    std::shared_ptr<const GplLibrary> library;
    try {
        library = std::make_shared<const GplLibrary>(build());
        if (!library->pipeline) {
            library.reset();
        }
    } catch (const std::exception& exception) {
        LOG_ERROR(Render_Vulkan, "Nx2Perf gpl: {} library {:016x} failed: {}",
                  PART_NAMES[static_cast<size_t>(part)], name, exception.what());
        library.reset();
    } catch (...) {
        LOG_ERROR(Render_Vulkan, "Nx2Perf gpl: {} library {:016x} failed",
                  PART_NAMES[static_cast<size_t>(part)], name);
        library.reset();
    }
    if (library) {
        library_builds[index].Add(std::chrono::steady_clock::now() - begin);
    } else {
        library_failures[index].fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::scoped_lock lock{entry->mutex};
        entry->library = library;
        entry->done = true;
    }
    entry->condition.notify_all();
    std::scoped_lock lock{mutex};
    Trim(MaxUnusedLibraries);
    return library;
}

size_t GraphicsPipelineLibraryCache::DropUnused() {
    std::scoped_lock lock{mutex};
    return Trim(0);
}

void GraphicsPipelineLibraryCache::RecordLink(bool optimized, std::chrono::nanoseconds time) {
    links[optimized ? 1 : 0].Add(time);
}

void GraphicsPipelineLibraryCache::RecordPipeline(bool linked, std::chrono::nanoseconds time) {
    (linked ? linked_pipelines : whole_pipelines).Add(time);
}

void GraphicsPipelineLibraryCache::RecordFallback() {
    fallbacks.fetch_add(1, std::memory_order_relaxed);
}

void GraphicsPipelineLibraryCache::Report() {
    const auto now = std::chrono::steady_clock::now();
    if (now - report_window < std::chrono::seconds{10}) {
        return;
    }
    report_window = now;
    struct Taken {
        u32 count;
        double total_ms;
        double max_ms;

        double Mean() const {
            return count == 0 ? 0.0 : total_ms / static_cast<double>(count);
        }
    };
    const auto take = [](Stat& stat) {
        return Taken{
            .count = stat.count.exchange(0, std::memory_order_relaxed),
            .total_ms = Ms(stat.total_ns.exchange(0, std::memory_order_relaxed)),
            .max_ms = Ms(stat.max_ns.exchange(0, std::memory_order_relaxed)),
        };
    };
    const auto take_count = [](std::atomic<u32>& counter) {
        return counter.exchange(0, std::memory_order_relaxed);
    };
    const Taken fast = take(links[0]);
    const Taken optimized = take(links[1]);
    const Taken linked = take(linked_pipelines);
    const Taken whole = take(whole_pipelines);
    const Taken pre_built = take(library_builds[0]);
    const Taken fragment_built = take(library_builds[1]);
    const u32 pre_hits = take_count(library_hits[0]);
    const u32 fragment_hits = take_count(library_hits[1]);
    const u32 pre_waits = take_count(library_waits[0]);
    const u32 fragment_waits = take_count(library_waits[1]);
    const u32 pre_failed = take_count(library_failures[0]);
    const u32 fragment_failed = take_count(library_failures[1]);
    const u32 fallback_count = take_count(fallbacks);
    if (fast.count + optimized.count + linked.count + whole.count + pre_built.count +
            fragment_built.count + pre_hits + fragment_hits + pre_failed + fragment_failed ==
        0) {
        return;
    }
    size_t cached;
    {
        std::scoped_lock lock{mutex};
        cached = entries.size();
    }
    LOG_INFO(Render_Vulkan,
             "Nx2Perf gpl in the last 10 s: {} pipelines linked, {:.2f} ms each with their "
             "libraries (max {:.1f} ms); {} built whole, {:.2f} ms each (max {:.1f} ms, {} after a "
             "failed library or link); fast links {} ({:.3f} ms each, max {:.3f} ms), optimized "
             "links {} ({:.1f} ms each, max {:.1f} ms); libraries built: pre-rasterization {} "
             "({:.1f} ms each, max {:.1f} ms), fragment {} ({:.1f} ms each, max {:.1f} ms); "
             "reused: pre-rasterization {} ({} waited for a build), fragment {} ({} waited); "
             "failed {}/{}; {} libraries cached",
             linked.count, linked.Mean(), linked.max_ms, whole.count, whole.Mean(), whole.max_ms,
             fallback_count, fast.count, fast.Mean(), fast.max_ms, optimized.count,
             optimized.Mean(), optimized.max_ms, pre_built.count, pre_built.Mean(),
             pre_built.max_ms, fragment_built.count, fragment_built.Mean(), fragment_built.max_ms,
             pre_hits, pre_waits, fragment_hits, fragment_waits, pre_failed, fragment_failed,
             cached);
}

size_t GraphicsPipelineLibraryCache::Trim(size_t keep) {
    // Oldest first. A library a pipeline holds has more owners than the cache's entry; one still
    // building is left alone.
    std::vector<std::list<const std::string*>::iterator> unused;
    for (auto it = lru.end(); it != lru.begin();) {
        --it;
        Entry& entry = *entries.at(**it);
        std::scoped_lock lock{entry.mutex};
        if (entry.done && entry.library.use_count() <= 1) {
            unused.push_back(it);
        }
    }
    if (unused.size() <= keep) {
        return 0;
    }
    const size_t drop = unused.size() - keep;
    for (size_t index = 0; index < drop; ++index) {
        const auto it = unused[index];
        const auto entry = entries.find(**it);
        lru.erase(it);
        entries.erase(entry);
    }
    return drop;
}

} // namespace Vulkan
