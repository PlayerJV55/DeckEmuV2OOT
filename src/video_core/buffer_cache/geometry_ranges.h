// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

#include "common/common_types.h"
#include "common/literals.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"

namespace VideoCommon {

/// The device range the index buffer or a vertex stream binds, before its buffer is found: what
/// BufferCache::UpdateIndexBuffer and UpdateVertexBuffer compute from the registers and the GPU
/// page table.
struct GeometryRange {
    DAddr device_addr{};
    u32 size{};
    bool bound{}; ///< False: the null binding

    bool operator==(const GeometryRange&) const = default;
};

/// The index buffer of a draw that doesn't use inline indexes.
inline GeometryRange IndexBufferRange(
    const Tegra::Engines::Maxwell3D::Regs::IndexBuffer& index_buffer,
    const Tegra::MemoryManager& gpu_memory) {
    const GPUVAddr gpu_addr_begin = index_buffer.StartAddress();
    const GPUVAddr gpu_addr_end = index_buffer.EndAddress();
    const std::optional<DAddr> device_addr = gpu_memory.GpuToCpuAddress(gpu_addr_begin);
    const u32 address_size = static_cast<u32>(gpu_addr_end - gpu_addr_begin);
    const u32 draw_size =
        (index_buffer.count + index_buffer.first) * u32(index_buffer.FormatSizeInBytes());
    const u32 size = (std::min)(address_size, draw_size);
    if (size == 0 || !device_addr) {
        return {};
    }
    return {.device_addr = *device_addr, .size = size, .bound = true};
}

/// Where vertex stream `index` starts and ends in GPU memory.
struct VertexStreamBounds {
    GPUVAddr begin{};
    GPUVAddr end{};
    bool clamped{}; ///< An enabled native stream's size past 4 GiB, clamped to 4 GiB
};

inline VertexStreamBounds VertexStreamAddresses(const Tegra::Engines::Maxwell3D::Regs& regs,
                                                bool native, u32 index) {
    const auto& array = regs.vertex_streams[index];
    const GPUVAddr begin = array.Address();
    u64 native_size = native ? regs.NativeVertexStreamSize(index) : 0;
    const bool clamped =
        native && array.enable != 0 && native_size > (std::numeric_limits<u32>::max)();
    if (clamped) {
        // Garbage commands can set this; clamp like Maxwell3D::GetMaxCurrentVertices, and
        // MaxContinuousRange bounds it below.
        native_size = (std::numeric_limits<u32>::max)();
    }
    const GPUVAddr end =
        native ? begin + native_size : regs.vertex_stream_limits[index].Address() + 1;
    return {.begin = begin, .end = end, .clamped = clamped};
}

/// The range vertex stream `index` binds. A disabled stream binds nothing, so its address needs no
/// translation: the stream registers mark streams dirty draw after draw, and in Mario Kart World
/// races these translations were a third of those the split's back makes, most of them for
/// disabled streams.
inline GeometryRange VertexStreamRange(const Tegra::Engines::Maxwell3D::Regs& regs,
                                       const VertexStreamBounds& bounds,
                                       const Tegra::MemoryManager& gpu_memory, u32 index) {
    using namespace Common::Literals;
    if (regs.vertex_streams[index].enable == 0) {
        return {};
    }
    const std::optional<DAddr> device_addr = gpu_memory.GpuToCpuAddress(bounds.begin);
    u32 size = static_cast<u32>(bounds.end - bounds.begin);
    if (size == 0 || !device_addr) {
        return {};
    }
    if (!gpu_memory.IsWithinGPUAddressRange(bounds.end) || size >= 64_MiB) {
        size = static_cast<u32>(gpu_memory.MaxContinuousRange(bounds.begin, size));
    }
    return {.device_addr = *device_addr, .size = size, .bound = true};
}

/// Index and vertex stream ranges a back split worker resolved for the current draw
/// (EDEN_NX2_BACK_SPLIT), which the buffer cache takes instead of resolving them itself. With
/// `verify` it resolves them anyway, takes its own and counts the differences.
struct PreparedGeometry {
    static constexpr u32 IndexSlot =
        static_cast<u32>(Tegra::Engines::Maxwell3D::Regs::NumVertexArrays);

    struct Mismatch {
        u32 slot; ///< A vertex stream, or IndexSlot
        GeometryRange parallel;
        GeometryRange serial;
    };

    std::array<GeometryRange, IndexSlot + 1> ranges{};
    u64 prepared_mask{}; ///< Bit per slot in ranges
    bool verify{};
    u32 mismatches{};
    std::array<Mismatch, 4> first_mismatches{};

    void Clear() noexcept {
        prepared_mask = 0;
        mismatches = 0;
    }

    void Set(u32 slot, const GeometryRange& range) noexcept {
        ranges[slot] = range;
        prepared_mask |= u64{1} << slot;
    }

    [[nodiscard]] bool Has(u32 slot) const noexcept {
        return ((prepared_mask >> slot) & 1) != 0;
    }

    /// The prepared range of `slot` when there is one, else `serial()`.
    template <typename Serial>
    GeometryRange Resolve(u32 slot, Serial&& serial) {
        if (!Has(slot)) {
            return serial();
        }
        if (!verify) {
            return ranges[slot];
        }
        const GeometryRange expected = serial();
        if (expected != ranges[slot]) {
            if (mismatches < first_mismatches.size()) {
                first_mismatches[mismatches] = {slot, ranges[slot], expected};
            }
            ++mismatches;
        }
        return expected;
    }
};

} // namespace VideoCommon
