// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2.h"

// EDEN_NX2_CMDPAGE_DIAG: for command lists on pages GPU work writes (nx2.h), whether the work
// writes the lists' own words or only other bytes of their 4 KiB pages, and what the front's
// slow path (DmaPusher::FlushGpuWrittenCommands: drain the split, flush the range) costs. Writes
// are noted where the buffer cache marks ranges written (a draw's whole storage binding), on
// pages a command list was read from. Every 2 s the front logs the counts.
namespace Tegra::Nx2CmdPageDiag {

inline bool g_enabled = std::getenv("EDEN_NX2_CMDPAGE_DIAG") != nullptr;

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_enabled = std::getenv("EDEN_NX2_CMDPAGE_DIAG") != nullptr;
}

inline constexpr u64 PageBytes = u64{1} << Nx2WrittenPageBits;
inline constexpr u32 PageWords = static_cast<u32>(PageBytes / sizeof(u32));

/// What wrote a slow-path entry's pages since the front last flushed them; later ones win.
enum class Kind : u32 {
    NoWrite,        ///< Nothing: the drain and flush found nothing new
    OtherElsewhere, ///< Dispatches or copies wrote other words of the pages
    DrawElsewhere,  ///< Draws wrote other words of the pages (false sharing)
    OtherOnEntry,   ///< Dispatches or copies wrote the entry's words
    DrawOnEntry,    ///< Draws wrote the entry's words
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Kind::Count)> kind_names{
    "no write", "dispatch/copy elsewhere", "draw elsewhere", "dispatch/copy on entry",
    "draw on entry",
};

/// How the front read an entry: its submitted copy, a safe read, guest memory (no GPU-written
/// page), GPU memory as indirect draw arguments, or guest memory after a drain and flush.
enum class Path : u32 { Submitted, Safe, Memory, Indirect, Slow, Count };

/// The buffer cache's writes come from a draw while the rasterizer prepares one (DrawScope).
inline thread_local bool t_in_draw{};

struct DrawScope {
    DrawScope() : active{g_enabled} {
        if (active) {
            t_in_draw = true;
        }
    }
    ~DrawScope() {
        if (active) {
            t_in_draw = false;
        }
    }
    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;
    bool active;
};

struct Page {
    std::bitset<PageWords> draw_words;  ///< Written by draws since flushed
    std::bitset<PageWords> other_words; ///< Written by dispatches and copies since flushed
    bool ever_draw{};
};

/// One slow-path entry's view of one of its pages.
struct PageView {
    Kind kind{Kind::NoWrite};
    u32 draw_first{PageWords}; ///< Draw-written words of the page, [first, last]
    u32 draw_last{};
};

struct State {
    std::mutex mutex;
    std::unordered_map<u64, Page> pages; ///< Pages command lists were read from on the slow path
    u64 draw_pages{};

    std::array<std::atomic<u64>, static_cast<size_t>(Path::Count)> entries{};
    std::atomic<u64> slow_flushed{};
    std::atomic<u64> slow_ns{};
    std::array<std::atomic<u64>, static_cast<size_t>(Kind::Count)> kinds{};
    std::array<std::atomic<u64>, static_cast<size_t>(Kind::Count)> kinds_changed{};
    std::atomic<u64> changed_words{};
    std::atomic<u64> draw_writes{};
    std::atomic<u64> draw_writes_whole_page{};
    std::atomic<u64> other_writes{};
    std::array<u32, static_cast<size_t>(Kind::Count)> details_logged{};
    std::chrono::steady_clock::time_point last_log{std::chrono::steady_clock::now()};
};

inline State& Get() {
    static State state;
    return state;
}

inline u64 Take(std::atomic<u64>& counter) {
    return counter.exchange(0, std::memory_order_relaxed);
}

/// Called with the mutex held: GPU work wrote [device_addr, device_addr + size) over the list
/// page at page_addr.
inline void NoteWriteOnPage(State& state, Page& page, u64 page_addr, u64 device_addr, u64 size,
                            bool draw) {
    const u64 begin = (std::max)(device_addr, page_addr) - page_addr;
    const u64 end = (std::min)(device_addr + size, page_addr + PageBytes) - page_addr;
    if (draw) {
        state.draw_writes.fetch_add(1, std::memory_order_relaxed);
        if (begin == 0 && end == PageBytes) {
            state.draw_writes_whole_page.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        state.other_writes.fetch_add(1, std::memory_order_relaxed);
    }
    auto& words = draw ? page.draw_words : page.other_words;
    for (u64 word = begin / sizeof(u32); word < (end + sizeof(u32) - 1) / sizeof(u32); ++word) {
        words.set(word);
    }
    if (draw && !page.ever_draw) {
        page.ever_draw = true;
        ++state.draw_pages;
    }
}

/// The back: GPU work wrote [device_addr, device_addr + size). Only list pages are tracked.
inline void NoteWrite(u64 device_addr, u64 size) {
    if (size == 0) {
        return;
    }
    State& state = Get();
    const bool draw = t_in_draw;
    const u64 first = device_addr & ~(PageBytes - 1);
    const u64 end = device_addr + size;
    std::scoped_lock lock{state.mutex};
    if (state.pages.empty()) {
        return;
    }
    // Native storage bindings span up to 8 MiB: walk whichever of the two is shorter.
    if (state.pages.size() < (end - first) / PageBytes) {
        for (auto& [page_addr, page] : state.pages) {
            if (page_addr + PageBytes > device_addr && page_addr < end) {
                NoteWriteOnPage(state, page, page_addr, device_addr, size, draw);
            }
        }
        return;
    }
    for (u64 page_addr = first; page_addr < end; page_addr += PageBytes) {
        if (const auto it = state.pages.find(page_addr); it != state.pages.end()) {
            NoteWriteOnPage(state, it->second, page_addr, device_addr, size, draw);
        }
    }
}

/// The front, with the split drained: what wrote the page since it last flushed it, and over the
/// entry's bytes [begin, end) of it. Starts tracking the page and forgets its writes (the caller
/// flushed it).
inline PageView TakePage(u64 page_addr, u32 begin, u32 end) {
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    Page& page = state.pages[page_addr];
    std::bitset<PageWords> entry;
    for (u32 word = begin / sizeof(u32); word < (end + sizeof(u32) - 1) / sizeof(u32); ++word) {
        entry.set(word);
    }
    PageView view;
    if ((page.draw_words & entry).any()) {
        view.kind = Kind::DrawOnEntry;
    } else if ((page.other_words & entry).any()) {
        view.kind = Kind::OtherOnEntry;
    } else if (page.draw_words.any()) {
        view.kind = Kind::DrawElsewhere;
    } else if (page.other_words.any()) {
        view.kind = Kind::OtherElsewhere;
    }
    for (u32 word = 0; word < PageWords; ++word) {
        if (page.draw_words.test(word)) {
            view.draw_first = (std::min)(view.draw_first, word);
            view.draw_last = word;
        }
    }
    page.draw_words.reset();
    page.other_words.reset();
    return view;
}

inline void Log() {
    State& state = Get();
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - state.last_log).count();
    if (seconds < 2.0) {
        return;
    }
    state.last_log = now;
    const auto path = [&](Path p) { return Take(state.entries[static_cast<size_t>(p)]); };
    std::array<u64, static_cast<size_t>(Kind::Count)> kinds{};
    std::array<u64, static_cast<size_t>(Kind::Count)> changed{};
    for (size_t i = 0; i < kinds.size(); ++i) {
        kinds[i] = Take(state.kinds[i]);
        changed[i] = Take(state.kinds_changed[i]);
    }
    u64 draw_pages{};
    u64 list_pages{};
    {
        std::scoped_lock lock{state.mutex};
        draw_pages = state.draw_pages;
        list_pages = state.pages.size();
    }
    LOG_INFO(HW_GPU,
             "cmdpage diag {:.1f}s: entries submitted {} safe {} memory {} indirect {} slow {} | "
             "slow flushed {} in {:.1f} ms | slow by writer (entries/changed): {} {}/{}, {} {}/{}, "
             "{} {}/{}, {} {}/{}, {} {}/{}; changed words {} | list page writes draw {} (whole "
             "page {}) dispatch/copy {}; list pages {} (draw-written {})",
             seconds, path(Path::Submitted), path(Path::Safe), path(Path::Memory),
             path(Path::Indirect), path(Path::Slow), Take(state.slow_flushed),
             static_cast<double>(Take(state.slow_ns)) / 1e6, kind_names[0], kinds[0], changed[0],
             kind_names[1], kinds[1], changed[1], kind_names[2], kinds[2], changed[2],
             kind_names[3], kinds[3], changed[3], kind_names[4], kinds[4], changed[4],
             Take(state.changed_words), Take(state.draw_writes),
             Take(state.draw_writes_whole_page), Take(state.other_writes), list_pages, draw_pages);
}

/// The front parsed an entry through `path`.
inline void CountEntry(Path path) {
    if (!g_enabled) {
        return;
    }
    Get().entries[static_cast<size_t>(path)].fetch_add(1, std::memory_order_relaxed);
    Log();
}

/// The front drained (and flushed, if `flushed`) a slow-path entry; `first` is its first page's
/// view.
inline void RecordSlow(Kind kind, u64 changed_words, bool flushed, u64 ns, u64 address, u64 page,
                       u32 begin, u32 end, const PageView& first) {
    State& state = Get();
    const size_t index = static_cast<size_t>(kind);
    state.slow_flushed.fetch_add(flushed ? 1 : 0, std::memory_order_relaxed);
    state.slow_ns.fetch_add(ns, std::memory_order_relaxed);
    state.kinds[index].fetch_add(1, std::memory_order_relaxed);
    state.kinds_changed[index].fetch_add(changed_words != 0 ? 1 : 0, std::memory_order_relaxed);
    state.changed_words.fetch_add(changed_words, std::memory_order_relaxed);
    if (state.details_logged[index] >= 12) {
        return;
    }
    ++state.details_logged[index];
    LOG_INFO(HW_GPU,
             "cmdpage diag: {} at {:#x} (page {:#x} bytes {:#x}-{:#x}): {} words changed by the "
             "flush; draw-written bytes of the page {}",
             kind_names[index], address, page, begin, end, changed_words,
             first.draw_first <= first.draw_last
                 ? fmt::format("{:#x}-{:#x}", first.draw_first * 4, first.draw_last * 4 + 4)
                 : std::string{"none"});
}

} // namespace Tegra::Nx2CmdPageDiag
