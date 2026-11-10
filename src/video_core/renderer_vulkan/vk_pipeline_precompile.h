// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/shader_warm/native_program_list.h"

namespace Vulkan {

/// A pipeline Prepare Shaders' precompile built: its key hash and its shaders' hashes (vertex and
/// pixel; compute: the program and 0).
struct PrecompiledPipeline {
    u64 key_hash{};
    u64 first{};
    u64 second{};
};

/// A render setup (render targets, depth and multisampling) of the cache's vertex and pixel
/// pipelines: the first recorded key with it, which graphics precompiles copy, and how many keys
/// had it.
struct RecordedRenderSetup {
    GraphicsPipelineCacheKey key{};
    size_t count{};
};

/// Boots other than Prepare Shaders with a precompile switch on: what the session's new pipelines
/// (not in the disk cache) found of what Prepare Shaders built, logged when the session ends.
struct NativePrecompileHits {
    std::unordered_set<u64> keys;    ///< Key hashes built
    std::unordered_set<u64> pairs;   ///< Vertex and pixel programs built together
    std::unordered_set<u64> shaders; ///< Every program built
    /// Vertex and pixel keys: the key built, its programs built together in other state, both
    /// programs built but apart, not built
    std::array<u32, 4> graphics{};
    u32 other_stages{}; ///< Graphics keys with other stages, which are never precompiled
    /// The key built, the program built with other sizes, not built
    std::array<u32, 3> compute{};
};

/// What a PipelineCache keeps for the precompile of the game's native programs
/// (PipelineCache::PrecompileNativePrograms, vk_pipeline_precompile.cpp).
struct NativePrecompileState {
    VideoCommon::ShaderWarm::NativeProgramScan scan; ///< Filled by the collector
    std::mutex done_mutex;
    /// The pipelines built for this driver, by key hash (native_precompiled.bin), under done_mutex
    std::unordered_map<u64, PrecompiledPipeline> done;
    std::chrono::steady_clock::time_point saved{}; ///< Under done_mutex
    /// EDEN_NX2_PRECOMPILE_GRAPHICS: LoadDiskResources notes the cache's vertex and pixel
    /// pipelines (NoteRecordedPipeline): their render setups by RenderSetupHash, and how many had
    /// each set of vertex attribute types (FixedPipelineState::attribute_types).
    bool record_setups{};
    std::map<u64, RecordedRenderSetup> setups;
    std::map<u64, size_t> attribute_types;
    size_t recorded{};
    std::unique_ptr<NativePrecompileHits> hits;
    /// Last: stopped and joined before the scan it fills is destroyed.
    std::jthread collector;
};

} // namespace Vulkan
