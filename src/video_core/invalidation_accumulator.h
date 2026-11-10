// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/nx2_title_defaults.h"

namespace VideoCommon {

class InvalidationAccumulator {
public:
    InvalidationAccumulator() = default;
    ~InvalidationAccumulator() = default;

    void Add(GPUVAddr address, size_t size) noexcept {
        auto const end_address = start_address + accumulated_size;
        if (!(address >= start_address && address + size <= end_address)) {
            const GPUVAddr first_block = address & atomicity_mask;
            size = ((address + size + atomicity_size_mask) & atomicity_mask) -
                   (WholeBlocks() ? first_block : address);
            address = first_block;
            if (IsWaiting(address, size)) {
                ++dropped;
                return;
            }
            if (start_address == 0) {
                start_address = address;
                accumulated_size = size;
                // will now have collected as this point
            } else if (address != end_address) {
                buffer.emplace_back(start_address, accumulated_size);
                start_address = address;
                accumulated_size = size;
            } else {
                accumulated_size += size;
            }
        }
    }

    template <typename F>
    [[nodiscard]] bool InvalidateAll(F&& f) noexcept {
        if (start_address > 0) {
            for (auto [address, size] : buffer)
                f(address, size);
            f(start_address, accumulated_size);
            buffer.clear();
            start_address = 0;
            accumulated_size = 0;
            return true;
        }
        return false;
    }

    /// Adds left out since the last call because a waiting range covered them.
    [[nodiscard]] u64 TakeDropped() noexcept {
        const u64 count = dropped;
        dropped = 0;
        return count;
    }

private:
    // A range runs from the start of the first 32-byte block a write touches to the end of the
    // last one. The size used to be taken before the address was aligned down, so a write past
    // its block's start got a range ending short of it (a lone word at 0x54 gave [0x40, 0x4c)).
    // EDEN_NX2_INVALIDATE_BLOCKS=0 rounds that way again, the default for Ocarina of Time: the
    // rest of a block can hold GPU-written bytes guest memory doesn't have yet, which the
    // invalidation reloads stale (glowing geometry in Hyrule Field, Fritchii v5.1).
    [[nodiscard]] static bool WholeBlocks() noexcept {
        static Common::Nx2::SessionValue<bool> enabled{[] {
            return Common::Nx2::Setting("EDEN_NX2_INVALIDATE_BLOCKS", "1") != "0";
        }};
        return enabled.Get();
    }

    // Writes between two draws often go back to a range already waiting: NVN's draw macros
    // write the base vertex and instance to c[0][0x640] around each draw and reset them after
    // it, with the next draw's uniform data in between. Every waiting range is invalidated in
    // every cache at the next InvalidateAll, so one inside another adds nothing; only the last
    // few are looked at, to keep runs of thousands of inline writes linear.
    // EDEN_NX2_INVALIDATE_DEDUP=0 keeps them all (for comparisons).
    [[nodiscard]] bool IsWaiting(GPUVAddr address, size_t size) const noexcept {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_INVALIDATE_DEDUP");
            return value == nullptr || value[0] != '0';
        }();
        if (!enabled) {
            return false;
        }
        constexpr size_t RecentRanges = 4;
        size_t looked = 0;
        for (auto it = buffer.rbegin(); it != buffer.rend() && looked < RecentRanges;
             ++it, ++looked) {
            if (address >= it->first && address + size <= it->first + it->second) {
                return true;
            }
        }
        return false;
    }

    static constexpr size_t atomicity_bits = 5;
    static constexpr size_t atomicity_size = 1ULL << atomicity_bits;
    static constexpr size_t atomicity_size_mask = atomicity_size - 1;
    static constexpr size_t atomicity_mask = ~atomicity_size_mask;
    std::vector<std::pair<VAddr, size_t>> buffer;
    GPUVAddr start_address = 0;
    size_t accumulated_size = 0;
    u64 dropped = 0;
};

} // namespace VideoCommon
