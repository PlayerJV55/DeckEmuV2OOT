// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

// Graphics pipeline libraries (EDEN_NX2_GPL, VK_EXT_graphics_pipeline_library).
//
// A pipeline is linked from four parts: vertex input, pre-rasterization shaders, fragment shader
// and fragment output. The two shader parts are what the driver spends its time on; they are
// built once and shared by the state variants of a shader set, so a variant whose shader parts
// exist costs its two small interface parts and a fast link instead of a whole compile.
//
// The parts use the pipeline's own descriptor set layout (one set, push descriptors and
// descriptor buffers as without libraries), so a shader library is shared by pipelines whose
// layouts are identically defined: in practice the variants of one shader set.
//
// Libraries are keyed by the SPIR-V their stages were emitted to (128-bit hashes), the layout,
// the state the part bakes in, the dynamic state list, the render pass and the creation flags.
// Keying by the emitted SPIR-V is always correct; a variant whose state changes the SPIR-V
// (vertex attribute types the vertex shader reads, for one) builds that part again.
//
// Adapted from Lichii #579 (PantACRO4life, 2b306c68e1), without its per-stage descriptor sets.

/// EDEN_NX2_GPL_OPTIMIZE=1: after a fast link, the libraries are linked again with link time
/// optimization on the low priority optimize worker, and that pipeline replaces the fast one at a
/// later draw. The libraries then keep link time optimization info (more memory).
bool GplOptimizeEnabled();

enum class GplPart : u32 {
    PreRasterization = 0,
    Fragment = 1,
};

/// A shader library and the layout objects it was created with.
struct GplLibrary {
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
};

/// Bytes of a library key. Only whole scalar values are added, never structs with padding.
class GplKeyWriter {
public:
    template <typename T>
        requires std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>
    void Add(T value) {
        char bytes[sizeof(T)];
        std::memcpy(bytes, &value, sizeof(T));
        data.append(bytes, sizeof(T));
    }

    void Add(const u128& value) {
        Add(value[0]);
        Add(value[1]);
    }

    void AddBytes(const std::string& bytes) {
        Add(static_cast<u64>(bytes.size()));
        data.append(bytes);
    }

    [[nodiscard]] const std::string& Data() const noexcept {
        return data;
    }

private:
    std::string data;
};

/// The shader libraries graphics pipelines are linked from. A pipeline holds the libraries it was
/// linked from, since a library must outlive the pipelines linked from it; the cache keeps at most
/// MaxUnusedLibraries that no pipeline holds, dropping the least recently used first.
class GraphicsPipelineLibraryCache {
public:
    using Builder = std::function<GplLibrary()>;

    /// ~0.2 GB of driver memory at the ~0.45 MB a whole pipeline holds.
    static constexpr size_t MaxUnusedLibraries = 512;

    /// The library for `key`: built with `build` on this thread when nobody has built it yet,
    /// waited for when another thread is building it. Null when its build failed (a failed key is
    /// not retried while it stays cached). `name` (a shader hash) is only for the log.
    std::shared_ptr<const GplLibrary> GetOrCreate(GplPart part, const std::string& key,
                                                  const Builder& build, u64 name);

    /// Drops every library no pipeline holds (memory pressure). Returns how many went.
    size_t DropUnused();

    /// A link and how long it took.
    void RecordLink(bool optimized, std::chrono::nanoseconds time);

    /// A run-time pipeline built, linked from libraries or whole, libraries included.
    void RecordPipeline(bool linked, std::chrono::nanoseconds time);

    /// A pipeline built whole because a library or its link failed.
    void RecordFallback();

    /// Logs the last 10 s (Nx2Perf gpl) when anything happened. GPU thread.
    void Report();

private:
    struct Stat {
        std::atomic<u32> count{};
        std::atomic<u64> total_ns{};
        std::atomic<u64> max_ns{};

        void Add(std::chrono::nanoseconds time);
    };

    struct Entry {
        std::mutex mutex;
        std::condition_variable condition;
        bool done{};
        std::shared_ptr<const GplLibrary> library;
        std::list<const std::string*>::iterator lru; ///< Under the cache's mutex
    };

    /// Under mutex: drops the least recently used libraries no pipeline holds, keeping `keep`.
    size_t Trim(size_t keep);

    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries;
    std::list<const std::string*> lru; ///< Keys of entries, most recently used first

    std::array<Stat, 2> library_builds; ///< Per part
    std::array<std::atomic<u32>, 2> library_hits{};
    std::array<std::atomic<u32>, 2> library_waits{}; ///< Hits that waited for another build
    std::array<std::atomic<u32>, 2> library_failures{};
    std::array<Stat, 2> links; ///< Fast, optimized
    Stat linked_pipelines;
    Stat whole_pipelines;
    std::atomic<u32> fallbacks{};
    std::chrono::steady_clock::time_point report_window = std::chrono::steady_clock::now();
};

} // namespace Vulkan
