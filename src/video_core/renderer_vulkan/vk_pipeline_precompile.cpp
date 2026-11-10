// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "shader_recompiler/frontend/ir/opcodes.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/program_header.h"
#include "video_core/nx2.h"
#include "video_core/renderer_vulkan/pipeline_cache_file.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_precompile.h"
#include "video_core/shader_environment.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

namespace fs = std::filesystem;
namespace ShaderWarm = VideoCommon::ShaderWarm;
using Shader::Ampere::NativeComputeDispatch;
using VideoCommon::ScannedEnvironment;

constexpr std::array<char, 8> DoneMagic{'N', 'M', 'P', 'C', 'D', '0', '0', '1'};
constexpr u64 MaxDoneEntries = 1ULL << 26;
constexpr auto DoneSaveInterval = std::chrono::seconds{60};
constexpr auto ProgressLogInterval = std::chrono::seconds{10};

/// A render setup is precompiled once this share (per mille) of the recorded vertex and pixel
/// pipelines has it, the most used first.
constexpr size_t RecordedSetupPermille = 20;
constexpr size_t MaxRenderSetups = 6;
/// Sets of vertex attribute types, the most recorded first: one short session can record few
/// skinned meshes, whose bone indices are integers.
constexpr size_t MaxAttributeTypeSets = 4;

/// What the compute warm-up (warm-compute.bin) answers for a dispatch's reads of the GPU's state.
const VideoCommon::GuessMissing ComputeGuess{};

/// What the original answers for a draw's reads of the GPU's state.
const VideoCommon::GuessMissing GraphicsGuess{
    .cbuf_bank0 = 0,
    .cbuf_other = 0,
    .texture_type = Shader::TextureType::Color2D,
    .pixel_format = Shader::TexturePixelFormat::A8B8G8R8_UNORM,
};

fs::path DoneListPath(const fs::path& title_dir) {
    return title_dir / "native_precompiled.bin";
}

template <typename T>
bool Read(std::ifstream& file, T& value) {
    return static_cast<bool>(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename T>
void Write(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

/// The pipelines built by earlier runs. What a driver's cache keeps is its own, so a list written
/// for another driver, GPU or pipeline cache version is not read: everything is built again.
std::unordered_map<u64, PrecompiledPipeline> ReadDoneList(const fs::path& path,
                                                          const Device& device) {
    std::ifstream file(path, std::ios::binary);
    std::array<char, 8> magic{};
    u32 version{};
    u32 driver{};
    u32 device_id{};
    u64 count{};
    if (!file.read(magic.data(), magic.size()) || magic != DoneMagic || !Read(file, version) ||
        version != PipelineCacheFile::CurrentVersion || !Read(file, driver) ||
        driver != device.GetDriverVersion() || !Read(file, device_id) ||
        device_id != device.GetDeviceID() || !Read(file, count) || count > MaxDoneEntries) {
        return {};
    }
    std::vector<PrecompiledPipeline> entries(count);
    if (!file.read(reinterpret_cast<char*>(entries.data()),
                   static_cast<std::streamsize>(count * sizeof(PrecompiledPipeline)))) {
        return {};
    }
    std::unordered_map<u64, PrecompiledPipeline> done;
    for (const PrecompiledPipeline& entry : entries) {
        done.insert_or_assign(entry.key_hash, entry);
    }
    return done;
}

void WriteDoneList(const fs::path& path, const Device& device,
                   const std::unordered_map<u64, PrecompiledPipeline>& done) {
    fs::path temporary{path};
    temporary += ".tmp";
    std::error_code error;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(DoneMagic.data(), DoneMagic.size());
        Write(file, PipelineCacheFile::CurrentVersion);
        Write(file, static_cast<u32>(device.GetDriverVersion()));
        Write(file, static_cast<u32>(device.GetDeviceID()));
        Write(file, static_cast<u64>(done.size()));
        for (const auto& [hash, entry] : done) {
            Write(file, entry);
        }
        if (!file) {
            LOG_ERROR(Render_Vulkan, "Precompile: cannot write {}",
                      Common::FS::PathToUTF8String(path));
            file.close();
            fs::remove(temporary, error);
            return;
        }
    }
    fs::rename(temporary, path, error);
}

/// KeplerCompute::EmulatesNativeTensorPrograms outside a DLSS evaluation: Ocarina of Time's
/// matrix multiplies are rejected unless EDEN_NX2_OOT_NETWORK=1.
bool RejectNativeMatrix() {
    static const bool oot_network = [] {
        const char* value = std::getenv("EDEN_NX2_OOT_NETWORK");
        return value != nullptr && value[0] == '1';
    }();
    return !oot_network && Settings::GetCurrentProgramID() == Tegra::Nx2OcarinaOfTimeProgramId;
}

/// SPIR-V emission aborts on a shared memory access without shared memory, so a program that
/// accesses it is only built with a dispatch that gives it some.
bool UsesSharedMemory(const Shader::IR::Program& program) {
    for (const Shader::IR::Block* const block : program.post_order_blocks) {
        for (const Shader::IR::Inst& inst : block->Instructions()) {
            if (Shader::IR::NameOf(inst.GetOpcode()).find("Shared") != std::string_view::npos) {
                return true;
            }
        }
    }
    return false;
}

/// Only a vertex and a pixel program: the pipelines the graphics precompile builds.
bool VertexAndPixelOnly(const std::array<u64, 6>& hashes) {
    return hashes[0] == 0 && hashes[1] != 0 && hashes[2] == 0 && hashes[3] == 0 &&
           hashes[4] == 0 && hashes[5] != 0;
}

/// What a pixel program's compiled code depends on beyond its SPIR-V: the render targets.
u64 RenderSetupHash(const FixedPipelineState& state) {
    const struct {
        std::array<u8, Maxwell::NumRenderTargets> color_formats;
        u32 depth_enabled;
        u32 depth_format;
        u32 msaa_mode;
    } setup{state.color_formats, state.depth_enabled.Value(), state.depth_format.Value(),
            static_cast<u32>(state.msaa_mode.Value())};
    return Common::CityHash64(reinterpret_cast<const char*>(&setup), sizeof(setup));
}

u64 PairHash(u64 vertex, u64 pixel) {
    return std::rotl(vertex, 32) ^ pixel;
}

u64 AttributeTypeAt(u64 types, size_t location) {
    return (types >> (location * 2)) & 3;
}

/// types with the attribute types of other at the locations of mask that other enables.
u64 WithTypesOf(u64 types, u64 other, u32 mask) {
    for (size_t location = 0; location < Maxwell::NumVertexAttributes; ++location) {
        if (((mask >> location) & 1) != 0 && AttributeTypeAt(other, location) != 0) {
            const u64 bits = u64{3} << (location * 2);
            types = (types & ~bits) | (other & bits);
        }
    }
    return types;
}

/// A recorded pipeline with these programs, these vertex attribute types, and the alpha test off.
GraphicsPipelineCacheKey PrecompileKey(const RecordedRenderSetup& setup, u64 vertex, u64 pixel,
                                       u64 attribute_types) {
    GraphicsPipelineCacheKey key = setup.key;
    key.unique_hashes = {};
    key.unique_hashes[1] = vertex;
    key.unique_hashes[5] = pixel;
    key.state.attribute_types = attribute_types;
    key.state.alpha_test_func.Assign(
        FixedPipelineState::PackComparisonOp(Maxwell::ComparisonOp::Always_GL));
    return key;
}

struct GraphicsJob {
    u32 vertex;
    u32 pixel;
    const RecordedRenderSetup* setup;
    u64 attribute_types;
    /// The vertex program stands in for one the pixel program is never paired with: the pixel
    /// program takes every input it reads as written.
    bool stand_in_vertex;
};

struct GraphicsPlan {
    std::vector<GraphicsJob> jobs;
    size_t setups{};
    size_t type_sets{};
    size_t not_lowered{};  ///< Programs the lowering or translation rejects
    size_t unpaired{};     ///< Programs in no pair, built with a stand-in
    size_t built_before{}; ///< Pipelines an earlier run built
};

/// The game's vertex and pixel pairs for each common render setup of the cache's pipelines, a
/// vertex program with each set of attribute types it can see. hashes: each program's shader
/// hash (0: not lowered); reads: the attribute locations each vertex program reads.
GraphicsPlan PlanGraphics(const NativePrecompileState& state, const std::vector<u64>& hashes,
                          const std::vector<u32>& reads) {
    const auto& programs = state.scan.set.programs;
    GraphicsPlan plan;

    // Vertex programs take the attribute type recorded most at each location. Where recorded
    // pipelines differ (float for unskinned meshes where bone indices are integers), the sets of
    // types they use are built too, for the programs that read those locations.
    std::array<std::array<size_t, 4>, Maxwell::NumVertexAttributes> type_counts{};
    for (const auto& [types, count] : state.attribute_types) {
        for (size_t location = 0; location < type_counts.size(); ++location) {
            type_counts[location][AttributeTypeAt(types, location)] += count;
        }
    }
    u64 majority = 0;
    u32 varying = 0;
    for (size_t location = 0; location < type_counts.size(); ++location) {
        const auto& counts = type_counts[location];
        u64 best_type = 1; // Float
        size_t best = 0;
        size_t kinds = 0;
        for (u64 type = 1; type < counts.size(); ++type) {
            kinds += counts[type] != 0 ? 1 : 0;
            if (counts[type] > best) {
                best = counts[type];
                best_type = type;
            }
        }
        majority |= best_type << (location * 2);
        varying |= kinds > 1 ? 1U << location : 0U;
    }
    std::map<u64, size_t> projected;
    for (const auto& [types, count] : state.attribute_types) {
        projected[WithTypesOf(majority, types, varying)] += count;
    }
    std::vector<std::pair<size_t, u64>> ranked;
    for (const auto& [types, count] : projected) {
        ranked.emplace_back(count, types);
    }
    std::ranges::sort(ranked, std::greater{});
    if (ranked.size() > MaxAttributeTypeSets) {
        ranked.resize(MaxAttributeTypeSets);
    }
    plan.type_sets = ranked.size();
    const auto types_for = [&](size_t vertex) {
        std::vector<u64> sets{majority};
        for (const auto& [count, types] : ranked) {
            const u64 variant = WithTypesOf(majority, types, reads[vertex] & varying);
            if (std::ranges::find(sets, variant) == sets.end()) {
                sets.push_back(variant);
            }
        }
        return sets;
    };

    std::vector<const RecordedRenderSetup*> setups;
    for (const auto& [hash, setup] : state.setups) {
        if (setup.count * 1000 >= state.recorded * RecordedSetupPermille) {
            setups.push_back(&setup);
        }
    }
    std::ranges::stable_sort(setups, std::greater{}, &RecordedRenderSetup::count);
    if (setups.size() > MaxRenderSetups) {
        setups.resize(MaxRenderSetups);
    }
    plan.setups = setups.size();

    // A program of each stage pairs with the programs of the other stage that are in no pair.
    std::optional<u32> reference_vertex;
    std::optional<u32> reference_pixel;
    std::vector<u8> paired(programs.size());
    for (const auto& [vertex, pixel] : state.scan.set.pairs) {
        if (vertex < programs.size() && pixel < programs.size()) {
            paired[vertex] = 1;
            paired[pixel] = 1;
        }
    }
    for (u32 index = 0; index < programs.size(); ++index) {
        if (programs[index].header.empty()) {
            continue;
        }
        if (hashes[index] == 0) {
            ++plan.not_lowered;
            continue;
        }
        plan.unpaired += paired[index] == 0 ? 1 : 0;
        auto& reference = programs[index].IsPixel() ? reference_pixel : reference_vertex;
        if (!reference) {
            reference = index;
        }
    }
    if (setups.empty() || !reference_vertex || !reference_pixel) {
        return plan;
    }

    std::unordered_set<u64> queued;
    const auto add = [&](u32 vertex, u32 pixel, const RecordedRenderSetup* setup, u64 types,
                         bool stand_in_vertex) {
        if (hashes[vertex] == 0 || hashes[pixel] == 0) {
            return;
        }
        const u64 key_hash = PrecompileKey(*setup, hashes[vertex], hashes[pixel], types).Hash();
        if (state.done.contains(key_hash)) {
            ++plan.built_before;
        } else if (queued.insert(key_hash).second) {
            plan.jobs.push_back({vertex, pixel, setup, types, stand_in_vertex});
        }
    };
    for (const RecordedRenderSetup* const setup : setups) {
        for (const auto& [vertex, pixel] : state.scan.set.pairs) {
            if (vertex >= programs.size() || pixel >= programs.size()) {
                continue;
            }
            for (const u64 types : types_for(vertex)) {
                add(vertex, pixel, setup, types, false);
            }
        }
        for (u32 index = 0; index < programs.size(); ++index) {
            if (programs[index].IsPixel() && paired[index] == 0) {
                add(*reference_vertex, index, setup, majority, true);
            }
        }
    }
    for (u32 index = 0; index < programs.size(); ++index) {
        if (!programs[index].header.empty() && !programs[index].IsPixel() && paired[index] == 0) {
            for (const u64 types : types_for(index)) {
                add(index, *reference_pixel, setups.front(), types, false);
            }
        }
    }
    return plan;
}

double SecondsSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
}

} // Anonymous namespace

void PipelineCache::PrecompileNativePrograms(Core::System& system) {
    if (!Settings::values.prepare_shaders || !Settings::CurrentProgramIsSwitch2() ||
        (!ShaderWarm::PrecompileComputeEnabled() && !ShaderWarm::PrecompileGraphicsEnabled())) {
        return;
    }
    native_precompile = std::make_unique<NativePrecompileState>();
    NativePrecompileState& state = *native_precompile;
    state.record_setups = ShaderWarm::PrecompileGraphicsEnabled();
    const u64 title_id = system.GetApplicationProcessProgramID();
    state.collector = std::jthread([&system, &state, title_id](std::stop_token stop) {
        state.scan = ShaderWarm::CollectNativePrograms(
            ShaderWarm::OpenApplicationRomFS(system, title_id),
            ShaderWarm::NativeProgramListPath(title_id), stop);
    });
}

void PipelineCache::NoteRecordedPipeline(const GraphicsPipelineCacheKey& key) {
    if (!native_precompile || !native_precompile->record_setups) {
        return;
    }
    // Vertex attribute types are copied into attribute_types, which only dynamic vertex input
    // reads; a recorded transform feedback setup is no draw's but its own.
    if (key.state.dynamic_vertex_input == 0 || key.state.xfb_enabled != 0 ||
        !VertexAndPixelOnly(key.unique_hashes)) {
        return;
    }
    NativePrecompileState& state = *native_precompile;
    RecordedRenderSetup& setup = state.setups[RenderSetupHash(key.state)];
    if (setup.count++ == 0) {
        setup.key = key;
    }
    ++state.attribute_types[key.state.attribute_types];
    ++state.recorded;
}

void PipelineCache::RunNativePrecompile(const fs::path& title_dir, std::stop_token stop_loading) {
    if (!native_precompile || !native_precompile->collector.joinable()) {
        return;
    }
    NativePrecompileState& state = *native_precompile;
    {
        std::stop_callback stop_scan{stop_loading, [&state] { state.collector.request_stop(); }};
        state.collector.join();
    }
    const auto& programs = state.scan.set.programs;
    if (stop_loading.stop_requested()) {
        return;
    }
    if (programs.empty()) {
        LOG_WARNING(Render_Vulkan, "Precompile: no native programs found in the game's files");
        return;
    }
    const auto begin = std::chrono::steady_clock::now();
    const bool compute = ShaderWarm::PrecompileComputeEnabled();
    // Graphics pipelines copy the render setups and vertex attribute types of recorded ones.
    const bool graphics = state.record_setups && state.recorded != 0;
    if (state.record_setups && !graphics) {
        LOG_INFO(Render_Vulkan,
                 "Precompile: the pipeline cache has no vertex and pixel pipelines with dynamic "
                 "vertex input yet; graphics programs are precompiled by a later Prepare Shaders");
    }
    const bool reject_matrix = RejectNativeMatrix();
    const fs::path done_path = DoneListPath(title_dir);
    {
        std::scoped_lock lock{state.done_mutex};
        state.done = ReadDoneList(done_path, device);
        state.saved = begin;
    }

    // Each program's shader hash, which pipeline keys hold (0: not lowered, or not asked for);
    // whether a compute program accesses shared memory (only looked at when one of its dispatches
    // gives it none); and the attribute locations a vertex program reads.
    std::vector<u64> hashes(programs.size());
    std::vector<u8> uses_shared(programs.size());
    std::vector<u32> reads(programs.size());
    std::vector<const NativeComputeDispatch*> unshared(programs.size());
    if (compute) {
        for (const NativeComputeDispatch& dispatch : state.scan.set.compute) {
            if (dispatch.program < programs.size() && dispatch.shared_memory == 0 &&
                unshared[dispatch.program] == nullptr) {
                unshared[dispatch.program] = &dispatch;
            }
        }
    }
    for (size_t index = 0; index < programs.size(); ++index) {
        const bool is_compute = programs[index].header.empty();
        if (is_compute ? !compute : !graphics) {
            continue;
        }
        workers.QueueWork([this, &programs, &hashes, &uses_shared, &reads, &unshared, index,
                           is_compute, reject_matrix] {
            const auto& image = programs[index];
            const Shader::Stage stage = is_compute       ? Shader::Stage::Compute
                                        : image.IsPixel() ? Shader::Stage::Fragment
                                                          : Shader::Stage::VertexB;
            try {
                const auto env = ScannedEnvironment::Create(
                    image.code, image.header, stage, is_compute ? ComputeGuess : GraphicsGuess,
                    reject_matrix);
                if (!env) {
                    return;
                }
                if (const NativeComputeDispatch* const dispatch =
                        is_compute ? unshared[index] : nullptr) {
                    env->SetComputeDispatch(dispatch->workgroup, dispatch->shared_memory,
                                            dispatch->local_memory);
                    ShaderPools pools;
                    Shader::Maxwell::Flow::CFG cfg{*env, pools.flow_block, env->StartAddress()};
                    const auto program{Shader::Maxwell::TranslateProgram(pools.inst, pools.block,
                                                                         *env, cfg, host_info)};
                    uses_shared[index] = UsesSharedMemory(program) ? 1 : 0;
                } else if (stage == Shader::Stage::VertexB) {
                    ShaderPools pools;
                    Shader::Maxwell::Flow::CFG cfg{
                        *env, pools.flow_block,
                        static_cast<u32>(env->StartAddress() + sizeof(Shader::ProgramHeader))};
                    const auto program{Shader::Maxwell::TranslateProgram(pools.inst, pools.block,
                                                                         *env, cfg, host_info)};
                    for (size_t location = 0; location < Maxwell::NumVertexAttributes;
                         ++location) {
                        reads[index] |= program.info.loads.Generic(location) ? 1U << location : 0U;
                    }
                }
                hashes[index] = env->Hash();
            } catch (const std::exception& e) {
                LOG_DEBUG(Render_Vulkan, "Precompile: native program {} not translated: {}", index,
                          e.what());
            }
        });
    }
    workers.WaitForRequests(stop_loading);
    if (stop_loading.stop_requested()) {
        return;
    }

    struct ComputeJob {
        u32 program;
        ComputePipelineCacheKey key;
        u32 local_memory;
    };
    std::vector<ComputeJob> compute_jobs;
    size_t compute_not_lowered = 0;
    size_t compute_needs_shared = 0;
    size_t compute_built_before = 0;
    std::unordered_set<u64> queued;
    for (const NativeComputeDispatch& dispatch : state.scan.set.compute) {
        if (!compute) {
            break;
        }
        if (dispatch.program >= programs.size() || hashes[dispatch.program] == 0) {
            ++compute_not_lowered;
            continue;
        }
        if (uses_shared[dispatch.program] != 0 && dispatch.shared_memory == 0) {
            ++compute_needs_shared;
            continue;
        }
        const ComputePipelineCacheKey key{
            .unique_hash = hashes[dispatch.program],
            .shared_memory_size = dispatch.shared_memory,
            .workgroup_size = dispatch.workgroup,
        };
        const u64 key_hash = key.Hash();
        if (state.done.contains(key_hash)) {
            ++compute_built_before;
            continue;
        }
        if (queued.insert(key_hash).second) {
            compute_jobs.push_back({dispatch.program, key, dispatch.local_memory});
        }
    }
    const GraphicsPlan plan = graphics ? PlanGraphics(state, hashes, reads) : GraphicsPlan{};

    const size_t total = compute_jobs.size() + plan.jobs.size();
    LOG_INFO(Render_Vulkan,
             "Precompile: {} native programs; building {} compute and {} graphics pipelines on "
             "the pipeline workers ({} and {} built by an earlier run)",
             programs.size(), compute_jobs.size(), plan.jobs.size(), compute_built_before,
             plan.built_before);
    // Compute, graphics
    std::array<std::atomic<size_t>, 2> built{};
    std::array<std::atomic<size_t>, 2> failed{};
    std::chrono::steady_clock::time_point logged = begin;
    const auto finish = [&](size_t kind, bool ok, const PrecompiledPipeline& entry) {
        (ok ? built : failed)[kind].fetch_add(1, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{state.done_mutex};
        if (ok) {
            state.done.insert_or_assign(entry.key_hash, entry);
        }
        // Saved as it goes: a stopped run continues where it was next time.
        if (now - state.saved >= DoneSaveInterval) {
            state.saved = now;
            WriteDoneList(done_path, device, state.done);
        }
        if (now - logged >= ProgressLogInterval) {
            logged = now;
            LOG_INFO(Render_Vulkan, "Precompile: {} of {} built, {} failed, {:.0f} s",
                     built[0].load() + built[1].load(), total,
                     failed[0].load() + failed[1].load(), SecondsSince(begin));
        }
    };
    for (const ComputeJob& job : compute_jobs) {
        workers.QueueWork([this, &programs, &finish, &stop_loading, job, reject_matrix] {
            if (stop_loading.stop_requested()) {
                return;
            }
            bool ok = false;
            try {
                const auto& image = programs[job.program];
                const auto env = ScannedEnvironment::Create(
                    image.code, image.header, Shader::Stage::Compute, ComputeGuess, reject_matrix);
                if (env) {
                    env->SetComputeDispatch(job.key.workgroup_size, job.key.shared_memory_size,
                                            job.local_memory);
                    ShaderPools pools;
                    // Built and destroyed here: only the driver's cache keeps it.
                    ok = CreateComputePipeline(pools, job.key, *env, nullptr, false) != nullptr;
                }
            } catch (const std::exception& e) {
                LOG_DEBUG(Render_Vulkan, "Precompile: compute program {} failed: {}",
                          job.program, e.what());
            }
            finish(0, ok, PrecompiledPipeline{static_cast<u64>(job.key.Hash()),
                                              job.key.unique_hash, 0});
        });
    }
    for (const GraphicsJob& job : plan.jobs) {
        workers.QueueWork([this, &programs, &hashes, &finish, &stop_loading, job, reject_matrix] {
            if (stop_loading.stop_requested()) {
                return;
            }
            const GraphicsPipelineCacheKey key =
                PrecompileKey(*job.setup, hashes[job.vertex], hashes[job.pixel],
                              job.attribute_types);
            bool ok = false;
            try {
                const auto& vertex_image = programs[job.vertex];
                const auto& pixel_image = programs[job.pixel];
                const auto vertex = ScannedEnvironment::Create(
                    vertex_image.code, vertex_image.header, Shader::Stage::VertexB, GraphicsGuess,
                    reject_matrix);
                const auto pixel = ScannedEnvironment::Create(
                    pixel_image.code, pixel_image.header, Shader::Stage::Fragment, GraphicsGuess,
                    reject_matrix);
                if (vertex && pixel) {
                    const std::array<Shader::Environment*, 2> envs{vertex.get(), pixel.get()};
                    ShaderPools pools;
                    // Built and destroyed here: only the driver's cache keeps it.
                    ok = CreateGraphicsPipeline(pools, key, envs, nullptr, false, nullptr, nullptr,
                                                false, false, job.stand_in_vertex) != nullptr;
                }
            } catch (const std::exception& e) {
                LOG_DEBUG(Render_Vulkan, "Precompile: programs {} and {} failed: {}", job.vertex,
                          job.pixel, e.what());
            }
            finish(1, ok,
                   PrecompiledPipeline{static_cast<u64>(key.Hash()), key.unique_hashes[1],
                                       key.unique_hashes[5]});
        });
    }
    workers.WaitForRequests(stop_loading);

    {
        std::scoped_lock lock{state.done_mutex};
        WriteDoneList(done_path, device, state.done);
    }
    const auto& scan = state.scan;
    LOG_INFO(Render_Vulkan,
             "Prepare shaders, precompile{}: {} native programs ({}, {:.1f} s), {} vertex and "
             "pixel pairs, {} compute dispatches. Compute: {} built, {} failed, {} built by an "
             "earlier run, {} not lowered, {} left out (shared memory without a size). Graphics "
             "({} render setups, {} attribute type sets): {} built, {} failed, {} built by an "
             "earlier run, {} programs not lowered, {} programs in no pair. {:.1f} s",
             stop_loading.stop_requested() ? " (stopped)" : "", programs.size(),
             scan.from_cache ? std::string{"from native_programs.bin"}
                             : fmt::format("scanned in {} files", scan.files),
             scan.seconds, scan.set.pairs.size(), scan.set.compute.size(), built[0].load(),
             failed[0].load(), compute_built_before, compute_not_lowered, compute_needs_shared,
             plan.setups, plan.type_sets, built[1].load(), failed[1].load(), plan.built_before,
             plan.not_lowered, plan.unpaired, SecondsSince(begin));
}

void PipelineCache::StartNativePrecompileHits(const fs::path& title_dir) {
    if (!Settings::CurrentProgramIsSwitch2() ||
        (!ShaderWarm::PrecompileComputeEnabled() && !ShaderWarm::PrecompileGraphicsEnabled())) {
        return;
    }
    const auto done = ReadDoneList(DoneListPath(title_dir), device);
    if (done.empty()) {
        LOG_INFO(Render_Vulkan, "Precompile: Prepare Shaders built nothing for this driver yet");
        return;
    }
    auto hits = std::make_unique<NativePrecompileHits>();
    for (const auto& [key_hash, entry] : done) {
        hits->keys.insert(key_hash);
        hits->shaders.insert(entry.first);
        if (entry.second != 0) {
            hits->shaders.insert(entry.second);
            hits->pairs.insert(PairHash(entry.first, entry.second));
        }
    }
    native_precompile = std::make_unique<NativePrecompileState>();
    native_precompile->hits = std::move(hits);
    LOG_INFO(Render_Vulkan,
             "Precompile: Prepare Shaders built {} pipelines for this driver; the session's end "
             "logs how many of its new pipelines they cover",
             done.size());
}

void PipelineCache::NoteNativePrecompileHit(const GraphicsPipelineCacheKey& key) {
    if (!native_precompile || !native_precompile->hits) {
        return;
    }
    const u64 key_hash = key.Hash();
    if (disk_pipelines.contains(key_hash)) {
        return;
    }
    NativePrecompileHits& hits = *native_precompile->hits;
    const auto& programs = key.unique_hashes;
    if (!VertexAndPixelOnly(programs)) {
        ++hits.other_stages;
    } else if (hits.keys.contains(key_hash)) {
        ++hits.graphics[0];
    } else if (hits.pairs.contains(PairHash(programs[1], programs[5]))) {
        ++hits.graphics[1];
    } else if (hits.shaders.contains(programs[1]) && hits.shaders.contains(programs[5])) {
        ++hits.graphics[2];
    } else {
        ++hits.graphics[3];
    }
}

void PipelineCache::NoteNativePrecompileHit(const ComputePipelineCacheKey& key) {
    if (!native_precompile || !native_precompile->hits) {
        return;
    }
    NativePrecompileHits& hits = *native_precompile->hits;
    ++hits.compute[hits.keys.contains(key.Hash())          ? 0
                   : hits.shaders.contains(key.unique_hash) ? 1
                                                            : 2];
}

void PipelineCache::ReportNativePrecompileHits() {
    if (!native_precompile || !native_precompile->hits) {
        return;
    }
    const NativePrecompileHits& hits = *native_precompile->hits;
    const u32 graphics = std::accumulate(hits.graphics.begin(), hits.graphics.end(), 0U);
    const u32 compute = std::accumulate(hits.compute.begin(), hits.compute.end(), 0U);
    if (graphics == 0 && hits.other_stages == 0 && compute == 0) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Precompile, this session's new pipelines (not in the disk cache): {} vertex and "
             "pixel - {} with a key Prepare Shaders built, {} with its programs built together "
             "in other state, {} with both programs built apart, {} not built; {} with other "
             "stages (never precompiled); {} compute - {} with a key built, {} with the program "
             "built for other sizes, {} not built",
             graphics, hits.graphics[0], hits.graphics[1], hits.graphics[2], hits.graphics[3],
             hits.other_stages, compute, hits.compute[0], hits.compute[1], hits.compute[2]);
}

} // namespace Vulkan
