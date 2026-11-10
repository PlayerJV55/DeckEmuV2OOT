// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <bit>
#include <numeric>
#include "common/cityhash.h"
#include "common/scope_exit.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/memory_detect.h"
#include "common/nx2_title_defaults.h"
#include "common/thread.h"
#include "common/thread_worker.h"
#include "core/core.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/ir_opt/oot_visual_workaround.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/program_header.h"
#include "video_core/build_priority.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/native_programs.h"
#include "video_core/nx2_perf.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/pipeline_cache_file.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_optimization.h"
#include "video_core/renderer_vulkan/pipeline_statistics.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_precompile.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_cache_v2.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_notify.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/spirv_inline_control.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"

#ifdef __ANDROID__
#include "../../android/app/src/main/jni/android_settings.h"
#endif

namespace Vulkan {

namespace {
using Shader::Backend::SPIRV::EmitSPIRV;
using Shader::Maxwell::ConvertLegacyToGeneric;
using Shader::Maxwell::GenerateGeometryPassthrough;
using Shader::Maxwell::MergeDualVertexPrograms;
using Shader::Maxwell::TranslateProgram;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GraphicsEnvironment;

constexpr u32 CACHE_VERSION = PipelineCacheFile::CurrentVersion;
constexpr size_t VULKAN_CACHE_FLUSH_PIPELINES = 128;
constexpr size_t VULKAN_CACHE_FLUSH_MIN_SECONDS = 30;
constexpr std::array<char, 8> VULKAN_CACHE_MAGIC_NUMBER{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};

template <typename Container>
auto MakeSpan(Container& container) {
    return std::span(container.data(), container.size());
}

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    UNIMPLEMENTED_MSG("Unimplemented comparison op={}", comparison);
    return {};
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        ASSERT_MSG(false, "Invalid vertex attribute type!");
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

Shader::AttributeType AttributeType(const FixedPipelineState& state, size_t index) {
    switch (state.DynamicAttributeType(index)) {
    case 0:
        return Shader::AttributeType::Disabled;
    case 1:
        return Shader::AttributeType::Float;
    case 2:
        return Shader::AttributeType::SignedInt;
    case 3:
        return Shader::AttributeType::UnsignedInt;
    }
    return Shader::AttributeType::Disabled;
}

Shader::InputTopology MaxwellToInputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::InputTopology::Points;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::InputTopology::Lines;
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleStrip:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::Patches:
        return Shader::InputTopology::Triangles;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        return Shader::InputTopology::LinesAdjacency;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        return Shader::InputTopology::TrianglesAdjacency;
    }
    return {};
}

Shader::TessPrimitive MaxwellToTessPrimitive(u32 raw) {
    switch (static_cast<Maxwell::Tessellation::DomainType>(raw)) {
    case Maxwell::Tessellation::DomainType::Isolines:
        return Shader::TessPrimitive::Isolines;
    case Maxwell::Tessellation::DomainType::Triangles:
        return Shader::TessPrimitive::Triangles;
    case Maxwell::Tessellation::DomainType::Quads:
        return Shader::TessPrimitive::Quads;
    }
    ASSERT(false);
    return Shader::TessPrimitive::Triangles;
}

Shader::TessSpacing MaxwellToTessSpacing(u32 raw) {
    switch (static_cast<Maxwell::Tessellation::Spacing>(raw)) {
    case Maxwell::Tessellation::Spacing::Integer:
        return Shader::TessSpacing::Equal;
    case Maxwell::Tessellation::Spacing::FractionalOdd:
        return Shader::TessSpacing::FractionalOdd;
    case Maxwell::Tessellation::Spacing::FractionalEven:
        return Shader::TessSpacing::FractionalEven;
    }
    ASSERT(false);
    return Shader::TessSpacing::Equal;
}

Shader::AttributeType ColorOutputType(u8 render_target_format) {
    const auto format = static_cast<Tegra::RenderTargetFormat>(render_target_format);
    const auto pixel_format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
    if (!VideoCore::Surface::IsPixelFormatInteger(pixel_format)) {
        return Shader::AttributeType::Float;
    }
    return VideoCore::Surface::IsPixelFormatSignedInteger(pixel_format)
               ? Shader::AttributeType::SignedInt
               : Shader::AttributeType::UnsignedInt;
}

// What translation and SPIR-V emission read of a pipeline key, plus the settings they read
// (vk_shader_reuse.h). MakeRuntimeInfo, the geometry output pruning and the layer passthrough
// only get this key, so nothing they use can be left out of it. A field is only filled when a
// stage of the pipeline reads it: pipelines differing in state their shaders ignore then share
// the shaders.
ShaderTranslationKey MakeShaderTranslationKey(const GraphicsPipelineCacheKey& pipeline_key,
                                              const Device& device) {
    const FixedPipelineState& state{pipeline_key.state};
    const bool has_tessellation_eval{pipeline_key.unique_hashes[3] != 0};
    const bool has_geometry{pipeline_key.unique_hashes[4] != 0};
    const bool has_fragment{pipeline_key.unique_hashes[5] != 0};

    ShaderTranslationKey key{};
    key.unique_hashes = pipeline_key.unique_hashes;
    key.input_topology = MaxwellToInputTopology(state.topology);
    key.passthrough_topology = MaxwellToOutputTopology(state.topology);
    key.convert_depth_mode = state.ndc_minus_one_to_one != 0;
    key.force_early_z = state.early_z != 0;
    key.y_negate = state.y_negate != 0;

    if (state.dynamic_vertex_input) {
        for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
            key.generic_input_types[index] = AttributeType(state, index);
        }
    } else {
        std::ranges::transform(state.attributes, key.generic_input_types.begin(),
                               &CastAttributeType);
    }
    // A fixed point size is written by the last vertex stage when points are drawn, and by a
    // geometry stage that outputs points (a generated passthrough one outputs what is drawn).
    if (key.input_topology == Shader::InputTopology::Points || has_geometry) {
        key.point_size = state.point_size;
    }
    if (state.xfb_enabled != 0) {
        key.xfb_enabled = true;
        key.xfb_state = state.xfb_state;
    }
    if (has_tessellation_eval) {
        key.tess_clockwise = state.tessellation_clockwise != 0;
        key.tess_primitive = MaxwellToTessPrimitive(state.tessellation_primitive.Value());
        key.tess_spacing = MaxwellToTessSpacing(state.tessellation_spacing);
    }
    if (has_fragment) {
        key.alpha_test_func = MaxwellToCompareFunction(
            state.UnpackComparisonOp(state.alpha_test_func.Value()));
        key.alpha_test_reference = state.alpha_test_ref;
        key.dual_source_blend = state.attachment0_dual_source_blend != 0;
        if (device.IsMoltenVK()) {
            std::ranges::transform(state.color_formats, key.color_output_types.begin(),
                                   &ColorOutputType);
        }
    }

    const auto& resolution{Settings::values.resolution_info};
    key.resolution_up_scale = resolution.up_scale;
    key.resolution_down_shift = resolution.down_shift;
    key.rescaling_active = resolution.active;
    key.renderer_debug = Settings::values.renderer_debug.GetValue();
    key.disable_shader_loop_safety_checks =
        Settings::values.disable_shader_loop_safety_checks.GetValue();
    key.fix_bloom_effects = Settings::values.fix_bloom_effects.GetValue();
    return key;
}

void SetTransformFeedback(Shader::RuntimeInfo& info, const ShaderTranslationKey& key,
                          const Device& device) {
    if (!key.xfb_enabled) {
        return;
    }
    if (!device.IsExtTransformFeedbackSupported()) {
        LOG_WARNING(Render_Vulkan, "XFB requested in pipeline key but device lacks VK_EXT_transform_feedback; ignoring XFB decorations");
        return;
    }
    auto [varyings, count] = VideoCommon::MakeTransformFeedbackVaryings(key.xfb_state);
    info.xfb_varyings = varyings;
    info.xfb_count = count;
}

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const ShaderTranslationKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program,
                                    const Vulkan::Device& device) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    if (previous_program && previous_program->stage == Shader::Stage::Mesh) {
        for (const u32 line : previous_program->mesh.primitive_lines) {
            const auto attribute{static_cast<Shader::IR::Attribute>(line)};
            if (Shader::IR::IsGeneric(attribute)) {
                info.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
            }
        }
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].is_geometry_passthrough};
    const float point_size{std::bit_cast<float>(key.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.input_topology == Shader::InputTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            SetTransformFeedback(info, key, device);
            info.convert_depth_mode = key.convert_depth_mode;
        }
        info.generic_input_types = key.generic_input_types;
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = key.tess_clockwise;
        info.tess_primitive = key.tess_primitive;
        info.tess_spacing = key.tess_spacing;
        break;
    case Shader::Stage::Mesh:
        info.convert_depth_mode = key.convert_depth_mode;
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        SetTransformFeedback(info, key, device);
        info.convert_depth_mode = key.convert_depth_mode;
        break;
    case Shader::Stage::Fragment:
        info.alpha_test_func = key.alpha_test_func;
        info.alpha_test_reference = std::bit_cast<float>(key.alpha_test_reference);
        info.dual_source_blend = key.dual_source_blend;
        info.color_output_types = key.color_output_types;
        break;
    default:
        break;
    }
    info.input_topology = key.input_topology;
    info.force_early_z = key.force_early_z;
    info.y_negate = key.y_negate;
    // Transform feedback would capture the scaled values, a passthrough geometry shader doesn't
    // scale the outputs it writes itself, and indexed attribute reads in the fragment stage don't
    // undo the scaling, so all three keep native noperspective interpolation.
    const bool geometry_passthrough{key.unique_hashes[4] != 0 &&
                                    programs[4].is_geometry_passthrough};
    if (Shader::NoPerspectiveAsSmooth(device.GetDriverID() != VK_DRIVER_ID_NVIDIA_PROPRIETARY) &&
        key.unique_hashes[5] != 0 && !geometry_passthrough &&
        !key.xfb_enabled && !programs[5].info.loads_indexed_attributes) {
        const bool has_tess{key.unique_hashes[3] != 0};
        const bool last_pre_raster{
            (stage == Shader::Stage::VertexB && !has_tess && !has_geometry) ||
            (stage == Shader::Stage::TessellationEval && !has_geometry) ||
            (stage == Shader::Stage::Geometry && has_geometry)};
        if (stage == Shader::Stage::Fragment) {
            info.scaled_noperspective_inputs =
                previous_program && (previous_program->stage == Shader::Stage::VertexB ||
                                     previous_program->stage == Shader::Stage::TessellationEval ||
                                     previous_program->stage == Shader::Stage::Geometry);
        }
        if (last_pre_raster) {
            const auto& fragment_info{programs[5].info};
            for (size_t index = 0; index < 32; ++index) {
                if (fragment_info.interpolation[index] == Shader::Interpolation::NoPerspective) {
                    info.noperspective_outputs.set(index);
                }
            }
        }
    }
    return info;
}

// EDEN_NX2_COMPUTE_NOINLINE / EDEN_NX2_GRAPHICS_NOINLINE=1: functions a shader calls from several
// places (a native program's global memory functions, several hundred instructions each, called
// from dozens of places) are built as calls instead of being inlined at every call site
// (video_core/spirv_inline_control.h). On NVIDIA single compute shaders of a test title took 2-13 s
// to build with them inlined, each a freeze at its first dispatch; 92 such shaders went from 246 s
// to 72.5 s of compile (Lichii). Only the module handed to the driver changes: the cache and the
// SPIR-V dumps keep the code as emitted. On by default for Ocarina of Time, off elsewhere.
bool NoInlineSetting(const char* name) {
    return Common::Nx2::Setting(name, "0") == "1";
}

bool ComputeNoInline() {
    static Common::Nx2::SessionValue<bool> enabled{
        [] { return NoInlineSetting("EDEN_NX2_COMPUTE_NOINLINE"); }};
    return enabled.Get();
}

bool GraphicsNoInline() {
    static Common::Nx2::SessionValue<bool> enabled{
        [] { return NoInlineSetting("EDEN_NX2_GRAPHICS_NOINLINE"); }};
    return enabled.Get();
}

// The words a module is built from: `code` with the DontInline pass when `enabled`.
std::vector<u32> ModuleCode(std::span<const u32> code, bool enabled, std::string_view kind,
                            u64 shader_hash) {
    std::vector<u32> words(code.begin(), code.end());
    if (!enabled) {
        return words;
    }
    if (const size_t marked = VideoCommon::MarkSharedFunctionsDontInline(words); marked != 0) {
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
            LOG_INFO(Render_Vulkan,
                     "{} shader {:016x}: {} shared functions built as calls, not inlined", kind,
                     shader_hash, marked);
        }
    }
    return words;
}

// For ReportRuntimePipeline, per thread since builds run on several: time spent emitting SPIR-V
// and making modules, and whether the last graphics build reused another pipeline's shaders.
thread_local std::chrono::steady_clock::duration emit_time_this_thread{};
thread_local bool last_build_reused{};

// Emits a stage's SPIR-V and makes its module. With a reference (EDEN_NX2_PERF_VERIFY=1 and
// async_pipelines), the SPIR-V the GPU thread emitted is used if this one differs from it.
// `emitted` gets a copy of the SPIR-V for shader reuse, `spirv_hash` the hash graphics pipeline
// libraries are keyed by (EDEN_NX2_GPL).
vk::ShaderModule EmitShaderModule(const Device& device, const Shader::Profile& profile,
                                  const Shader::RuntimeInfo& runtime_info,
                                  Shader::IR::Program& program, Shader::Backend::Bindings& binding,
                                  u64 shader_hash, size_t stage_index,
                                  std::span<const u32> reference = {},
                                  std::vector<u32>* emitted = nullptr,
                                  u128* spirv_hash = nullptr) {
    const auto begin = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        emit_time_this_thread += std::chrono::steady_clock::now() - begin;
    };
    std::vector<u32> code{EmitSPIRV(profile, runtime_info, program, binding)};
    if (!reference.empty() && !std::ranges::equal(code, reference)) {
        LOG_ERROR(Render_Vulkan,
                  "Nx2Perf async_pipelines: shader {:016x} SPIR-V differs from the GPU thread's, "
                  "using that",
                  shader_hash);
        code.assign(reference.begin(), reference.end());
    }
    if (emitted != nullptr) {
        *emitted = code;
    }
    if (spirv_hash != nullptr) {
        *spirv_hash = Common::CityHash128(reinterpret_cast<const char*>(code.data()),
                                          code.size() * sizeof(u32));
    }
    device.SaveShader(code);
    vk::ShaderModule module{
        BuildShader(device, ModuleCode(code, GraphicsNoInline(), "Graphics", shader_hash))};

    // Text log + .spv dump. Text log is gated by gpu_log_level != Off; .spv dump
    // is independent and gated only by gpu_log_shader_dumps.
    const bool should_log = GPU::Logging::IsActive();
    const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
    if (should_log || should_dump) {
        static constexpr std::array stage_names{"vertex", "tess_control", "tess_eval", "geometry", "fragment"};
        const std::string shader_name = fmt::format("shader_{:016x}_{}", shader_hash, stage_names[stage_index]);
        if (should_log) {
            const std::string shader_info = fmt::format("SPIR-V size: {} bytes, hash: {:016x}",
                code.size() * sizeof(u32), shader_hash);
            GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
        }
        if (should_dump) {
            GPU::Logging::DumpSpirvShader(shader_hash,
                                          std::span<const u32>(code.data(), code.size()));
        }
    }

    if (device.HasDebuggingToolAttached()) {
        const std::string name{fmt::format("Shader {:016x}", shader_hash)};
        module.SetObjectNameEXT(name.c_str());
    }
    return module;
}

// EDEN_NX2_PARALLEL_STAGES: one program of a new graphics pipeline, translated by a helper thread
// or the GPU thread, whichever claims it first. Each has its own environment and pools: an
// environment memoizes what it reads, and pools are not thread-safe.
struct StageTranslation {
    Shader::Environment* env{};
    std::shared_ptr<ShaderPools> pools;
    u32 cfg_offset{};
    bool exits_to_dispatcher{};
    std::atomic<bool> claimed{};
    std::optional<Shader::IR::Program> program;
    std::exception_ptr exception;
};

// The programs of one pipeline. The helpers' tasks share it: one that runs after the GPU thread
// claimed its program finds it claimed and touches nothing else.
struct StageBatch {
    std::array<StageTranslation, Maxwell::MaxShaderProgram> stages;
    size_t count{};
    std::mutex mutex;
    std::condition_variable finished;
    size_t done{}; ///< Under mutex
};

void TranslateStage(StageBatch& batch, size_t index, const Shader::HostTranslateInfo& host_info) {
    StageTranslation& stage{batch.stages[index]};
    if (stage.claimed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        Shader::Maxwell::Flow::CFG cfg(*stage.env, stage.pools->flow_block, stage.cfg_offset,
                                       stage.exits_to_dispatcher);
        stage.program = TranslateProgram(stage.pools->inst, stage.pools->block, *stage.env, cfg,
                                         host_info);
    } catch (...) {
        stage.exception = std::current_exception();
    }
    std::scoped_lock lock{batch.mutex};
    if (++batch.done == batch.count) {
        batch.finished.notify_all();
    }
}

// Queues every program but the last to the helpers. The GPU thread translates the last one (the
// fragment program when there is one, usually the largest), then each one no helper took yet,
// newest first, and waits for those a helper is translating.
void TranslateStagesAtOnce(Common::ThreadWorker& helpers, const std::shared_ptr<StageBatch>& batch,
                           const Shader::HostTranslateInfo& host_info) {
    for (size_t index = 0; index + 1 < batch->count; ++index) {
        helpers.QueueWork(
            [batch, index, info = &host_info] { TranslateStage(*batch, index, *info); });
    }
    for (size_t index = batch->count; index-- > 0;) {
        TranslateStage(*batch, index, host_info);
    }
    std::unique_lock lock{batch->mutex};
    batch->finished.wait(lock, [&batch] { return batch->done == batch->count; });
}

// A graphics pipeline's translated programs, kept for its build to emit their SPIR-V.
struct DeferredStages {
    explicit DeferredStages(std::atomic<size_t>& pending_) : pending{pending_} {
        pending.fetch_add(1, std::memory_order_relaxed);
    }
    ~DeferredStages() {
        pending.fetch_sub(1, std::memory_order_relaxed);
    }
    DeferredStages(const DeferredStages&) = delete;
    DeferredStages& operator=(const DeferredStages&) = delete;

    std::atomic<size_t>& pending;
    std::shared_ptr<ShaderPools> pools; ///< Holds the programs' blocks and instructions
    /// Those of the programs translated at once (EDEN_NX2_PARALLEL_STAGES)
    std::vector<std::shared_ptr<ShaderPools>> stage_pools;
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    std::array<std::optional<Shader::RuntimeInfo>, Maxwell::MaxShaderStage> runtime_infos;
    std::array<std::vector<u32>, Maxwell::MaxShaderStage> reference_spirv;
};

size_t GetTotalPipelineWorkers() {
    const size_t max_core_threads =
        std::max<size_t>(static_cast<size_t>(std::thread::hardware_concurrency()), 2ULL) - 1ULL;
#ifdef __ANDROID__
    const int configured = AndroidSettings::values.pipeline_worker_count.GetValue();
    const int clamped = std::clamp(configured, 2, 8);
    const size_t desired = static_cast<size_t>(clamped);
    if (desired == 0) {
        return 1ULL;
    }
    return std::min(max_core_threads, desired);
#else
    return max_core_threads;
#endif
}

// A geometry program lowered from a pass-through one on hosts without
// VK_NV_geometry_shader_passthrough copies every generic in its mask: with all 32 next to
// Position it outputs 132 components, over maxGeometryOutputComponents (128 on AMD and NVIDIA).
// AMD's Windows driver closes the process building such a pipeline (Mario Tennis Fever's match
// start, Orchid 38f9867f09). Over the limit, the generics the fragment program never reads are
// dropped; anything within it is left alone. EDEN_NX2_GS_PRUNE_OUTPUTS=0 never drops them.
void PruneUnreadGeometryOutputs(Shader::IR::Program& geometry, const Shader::IR::Program& fragment,
                                bool xfb_enabled, u32 max_components) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_GS_PRUNE_OUTPUTS");
        return value == nullptr || value[0] != '0';
    }();
    Shader::VaryingState& stores{geometry.info.stores};
    const Shader::Info& fs{fragment.info};
    if (!enabled || fragment.stage != Shader::Stage::Fragment || xfb_enabled ||
        geometry.is_geometry_passthrough || fs.loads_indexed_attributes || fs.loads.Legacy() ||
        stores.Legacy()) {
        return;
    }
    u32 components{};
    for (size_t index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
        components += stores.Generic(index) ? 4 : 0;
    }
    components += stores.AnyComponent(Shader::IR::Attribute::PositionX) ? 4 : 0;
    components += stores[Shader::IR::Attribute::PointSize] ? 1 : 0;
    components += stores[Shader::IR::Attribute::Layer] ? 1 : 0;
    components += stores[Shader::IR::Attribute::ViewportIndex] ? 1 : 0;
    components += stores.ClipDistances() ? 8 : 0;
    if (components <= max_components) {
        return;
    }
    for (size_t index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
        if (fs.loads.Generic(index)) {
            continue;
        }
        for (size_t element = 0; element < 4; ++element) {
            stores.Set(Shader::IR::Attribute::Generic0X + index * 4 + element, false);
        }
    }
}

} // Anonymous namespace

size_t ComputePipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof *this);
    return static_cast<size_t>(hash);
}

bool ComputePipelineCacheKey::operator==(const ComputePipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof *this) == 0;
}

size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), Size());
    return static_cast<size_t>(hash);
}

bool GraphicsPipelineCacheKey::operator==(const GraphicsPipelineCacheKey& rhs) const noexcept {
    return unique_hashes == rhs.unique_hashes &&
           std::memcmp(&state, &rhs.state, state.Size()) == 0;
}

namespace {
// NVIDIA's and AMD's drivers keep a disk cache of built pipelines of their own. Eden's copy on top
// only kept every pipeline the title ever built in host memory (title 1: a 245 MB file, ~0.35 GB
// of RAM for the session) and copied it whole at each save, which stalled frames; pipelines built
// no slower without it.
bool DriverCachesPipelines(const Device& device) {
    switch (device.GetDriverID()) {
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:
    case VK_DRIVER_ID_AMD_PROPRIETARY:
        return true;
    default:
        return false;
    }
}

// AMD's Windows driver (and AMDVLK, built from the same code) crashes in
// vkCreateGraphicsPipelines on some fragment shaders with a dynamic-lane SHFL.IDX: an access
// violation in amdvlk64.dll on RDNA1, 2 and 4 (Ocarina of Time, upstream Eglantine #618). Such
// shuffles in graphics stages become a loop of broadcasts there. RADV is not affected.
// EDEN_NX2_AMD_SHUFFLE_LOOP=0 keeps the shuffle, =1 takes the loop on any driver.
bool BrokenDynamicSubgroupShuffle(VkDriverId driver_id) {
    static const char forced = [] {
        const char* value = std::getenv("EDEN_NX2_AMD_SHUFFLE_LOOP");
        return value == nullptr ? '\0' : value[0];
    }();
    if (forced == '0' || forced == '1') {
        return forced == '1';
    }
    return driver_id == VK_DRIVER_ID_AMD_PROPRIETARY || driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE;
}

// AMD's Windows driver for the Radeon RX 5000/6000 before 2.0.388 (2.0.353, Vulkan 1.4.315)
// optimizes Ocarina of Time's largest compute shaders for minutes as the intro movie starts
// (f9ea14a22394f5b1, 1.3 MB of SPIR-V: 18 min; eff3ccbe1438679d, 0.8 MB: 6 min on an RX 6600 XT)
// and the title screen looks frozen. Unoptimized they build in 36 and 17 s; 2.0.388 and later
// build both optimized. On AMD's proprietary driver before 2.0.388, those two and every compute
// shader of 800 KiB of SPIR-V or more are built without driver optimizations.
// EDEN_NX2_AMD_SAFE_COMPILE=0 turns it off, =1 applies it to any AMD proprietary driver version;
// EDEN_NX2_AMD_SAFE_COMPILE_MIN_KIB sets the size (0: every compute shader).
bool DisableDriverOptimization(const Device& device, u64 shader_hash, size_t spirv_bytes) {
    static constexpr std::array<u64, 2> SLOW_COMPUTE{0xf9ea14a22394f5b1ULL, 0xeff3ccbe1438679dULL};
    static const int mode = [] {
        const char* const value = std::getenv("EDEN_NX2_AMD_SAFE_COMPILE");
        if (value == nullptr || value[0] == '\0') {
            return -1;
        }
        return value[0] == '0' ? 0 : 1;
    }();
    static const size_t min_bytes = [] {
        const char* const value = std::getenv("EDEN_NX2_AMD_SAFE_COMPILE_MIN_KIB");
        if (value != nullptr && value[0] >= '0' && value[0] <= '9') {
            return static_cast<size_t>(std::strtoull(value, nullptr, 10)) * 1024;
        }
        return size_t{800} * 1024;
    }();
    if (mode == 0 || device.GetDriverID() != VK_DRIVER_ID_AMD_PROPRIETARY) {
        return false;
    }
    if (spirv_bytes < min_bytes &&
        std::ranges::find(SLOW_COMPUTE, shader_hash) == SLOW_COMPUTE.end()) {
        return false;
    }
    return mode == 1 || device.GetDriverVersion() < VK_MAKE_API_VERSION(0, 2, 0, 388);
}

// A compute shader first met in game holds its dispatch, and the guest, until it is built: AMD's
// driver took 6 and 18 min for Ocarina of Time's largest, 17 and 36 s without optimizations
// (Radeon RX 6600 XT, 2.0.353), and 5.6-8.7 s waits show in an RX 6550M log. Such shaders are
// built without driver optimizations first, then optimized on a background thread and swapped
// in. EDEN_NX2_COMPUTE_FAST_FIRST: unset applies it on AMD's proprietary driver in Switch 2
// titles to shaders of EDEN_NX2_COMPUTE_FAST_FIRST_MIN_KIB (64) KiB of SPIR-V or more, 1 on any
// driver, 0 never.
bool FastFirstCompute(const Device& device, size_t spirv_bytes) {
    static Common::Nx2::SessionValue<u8> mode{[] {
        const std::string_view value = Common::Nx2::Setting("EDEN_NX2_COMPUTE_FAST_FIRST", "");
        return static_cast<u8>(value.empty() ? 2 : value[0] == '0' ? 0 : 1);
    }};
    static const size_t min_bytes = [] {
        const char* const value = std::getenv("EDEN_NX2_COMPUTE_FAST_FIRST_MIN_KIB");
        if (value != nullptr && value[0] >= '0' && value[0] <= '9') {
            return static_cast<size_t>(std::strtoull(value, nullptr, 10)) * 1024;
        }
        return size_t{64} * 1024;
    }();
    const u8 current = mode.Get();
    if (current == 0 || spirv_bytes < min_bytes) {
        return false;
    }
    return current == 1 || (device.GetDriverID() == VK_DRIVER_ID_AMD_PROPRIETARY &&
                            Settings::CurrentProgramIsSwitch2());
}
} // Anonymous namespace

namespace {
// EDEN_NX2_PIPELINE_ASYNC (only with asynchronous shaders): a graphics pipeline missing from the
// cache is translated and built on the pipeline workers instead of the GPU thread, where each took
// 5-25 ms (565 in 10 s when Ocarina of Time enters Kokiri Forest with a cold cache). Its draws
// wait for it within the native budget and are then skipped where BuiltPipeline would skip them.
// 0 off, 1 Switch 1 draws only, 2 native draws too (the default for Switch 2 titles).
u32 AsyncPipelinesSetting() {
    static const std::optional<u32> forced = []() -> std::optional<u32> {
        const char* value = std::getenv("EDEN_NX2_PIPELINE_ASYNC");
        if (value == nullptr || value[0] == '\0') {
            return std::nullopt;
        }
        return value[0] == '0' ? 0U : value[0] == '1' ? 1U : 2U;
    }();
    return forced.value_or(Settings::CurrentProgramIsSwitch2() ? 2U : 0U);
}
} // Anonymous namespace

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             GuestDescriptorQueue& guest_descriptor_queue_,
                             DescriptorBufferRing& descriptor_buffer_ring_,
                             RenderPassCache& render_pass_cache_, BufferCache& buffer_cache_,
                             TextureCache& texture_cache_, VideoCore::ShaderNotify& shader_notify_)
    : VideoCommon::ShaderCache{device_memory_}, device{device_}, scheduler{scheduler_},
      descriptor_pool{descriptor_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      descriptor_buffer_ring{descriptor_buffer_ring_},
      render_pass_cache{render_pass_cache_}, buffer_cache{buffer_cache_},
      texture_cache{texture_cache_}, shader_notify{shader_notify_},
      use_asynchronous_shaders{Settings::UseAsynchronousShaders()},
      use_vulkan_pipeline_cache{Settings::values.use_vulkan_driver_pipeline_cache.GetValue() &&
                                !DriverCachesPipelines(device_)},
      num_pipeline_workers{device.GetPipelineWorkerLimit() != 0
                               ? (std::min)(size_t{device.GetPipelineWorkerLimit()},
                                            GetTotalPipelineWorkers())
                               : GetTotalPipelineWorkers()},
      workers(num_pipeline_workers, "VkPipelineBuilder", {}, Common::ThreadPlacement::Background),
      optimize_workers(1, "VkPipelineOptimize", {}, Common::ThreadPlacement::Background),
      serialization_thread(1, "VkPipelineSerialization", {},
                           Common::ThreadPlacement::Background) {
    const auto& float_control{device.FloatControlProperties()};
    const VkDriverId driver_id{device.GetDriverID()};
    const VkShaderStageFlags subgroup_stages{device.GetSubgroupSupportedStages()};
    const auto subgroup_stage_bit{[subgroup_stages](VkShaderStageFlags flag, Shader::Stage stage) {
        return (subgroup_stages & flag) != 0 ? (1u << static_cast<u32>(stage)) : 0u;
    }};
    u32 supported_subgroup_stages{
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexA) |
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexB) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                           Shader::Stage::TessellationControl) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                           Shader::Stage::TessellationEval) |
        subgroup_stage_bit(VK_SHADER_STAGE_GEOMETRY_BIT, Shader::Stage::Geometry) |
        subgroup_stage_bit(VK_SHADER_STAGE_FRAGMENT_BIT, Shader::Stage::Fragment) |
        subgroup_stage_bit(VK_SHADER_STAGE_COMPUTE_BIT, Shader::Stage::Compute) |
        subgroup_stage_bit(VK_SHADER_STAGE_TASK_BIT_EXT, Shader::Stage::Task) |
        subgroup_stage_bit(VK_SHADER_STAGE_MESH_BIT_EXT, Shader::Stage::Mesh)};
    if (driver_id == VK_DRIVER_ID_MESA_TURNIP) {
        supported_subgroup_stages &= ~(1u << static_cast<u32>(Shader::Stage::Geometry));
    }
    const bool is_amd = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                        driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                        driver_id == VK_DRIVER_ID_MESA_RADV;
#ifdef _WIN32
    // AMD's Windows driver also mishandles the position input (PokeDinner 57ffa55a8c).
    const bool broken_position_input = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_AMD_PROPRIETARY;
#else
    const bool broken_position_input = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
#endif
    profile = Shader::Profile{
        .supported_spirv = device.SupportedSpirvVersion(),
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = device.IsDescriptorAliasingSupported(),
        .support_int8 = device.IsInt8Supported(),
        .support_uniform_and_storage_buffer_8bit =
            device.IsUniformAndStorageBuffer8BitAccessSupported(),
        .support_storage_buffer_8bit = device.IsStorageBuffer8BitAccessSupported(),
        .support_int16 = device.IsShaderInt16Supported(),
        .support_uniform_and_storage_buffer_16bit =
            device.IsUniformAndStorageBuffer16BitAccessSupported(),
        .support_storage_buffer_16bit = device.IsStorageBuffer16BitAccessSupported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .support_vertex_instance_id = false,
        .support_float_controls = device.IsKhrShaderFloatControlsSupported(),
        .support_separate_denorm_behavior =
            float_control.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_separate_rounding_mode =
            float_control.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_fp16_denorm_preserve = float_control.shaderDenormPreserveFloat16 != VK_FALSE,
        .support_fp32_denorm_preserve = float_control.shaderDenormPreserveFloat32 != VK_FALSE,
        .support_fp16_denorm_flush = float_control.shaderDenormFlushToZeroFloat16 != VK_FALSE,
        .support_fp32_denorm_flush = float_control.shaderDenormFlushToZeroFloat32 != VK_FALSE,
        .support_fp16_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE,
        .support_fp32_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE,
        .support_fp64_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE,
        .support_explicit_workgroup_layout = device.IsKhrWorkgroupMemoryExplicitLayoutSupported(),
        .support_workgroup_layout_8bit_access =
            device.IsWorkgroupMemoryExplicitLayout8BitAccessSupported(),
        .support_workgroup_layout_16bit_access =
            device.IsWorkgroupMemoryExplicitLayout16BitAccessSupported(),
        .support_shader_quad_control = device.IsKhrShaderQuadControlSupported(),
        .support_quad_shuffles = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT),
        .support_vote = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_VOTE_BIT),
        .support_shuffle_relative =
            device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT),
        .supported_subgroup_stages = supported_subgroup_stages,
        .support_viewport_index_layer_non_geometry =
            device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
        .support_demote_to_helper_invocation =
            device.IsExtShaderDemoteToHelperInvocationSupported(),
        .support_int64_atomics = device.IsExtShaderAtomicInt64Supported(),
        // Native float atomic adds: the CAS loop they replace hung AMD and Intel Arc GPUs in
        // Ocarina's auto-exposure shaders. EDEN_NX2_NATIVE_FLOAT_ATOMICS=0 keeps the loop.
        .support_float32_atomic_add = device.IsShaderBufferFloat32AtomicAddSupported() && [] {
            const char* value = std::getenv("EDEN_NX2_NATIVE_FLOAT_ATOMICS");
            return value == nullptr || value[0] != '0';
        }(),
        .support_shared_int64_atomics = device.IsSharedInt64AtomicsSupported(),
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_native_ndc = device.IsExtDepthClipControlSupported(),
        .support_scaled_attributes = !device.MustEmulateScaledFormats(),
        .support_multi_viewport = device.SupportsMultiViewport(),
        .support_geometry_streams = device.AreTransformFeedbackGeometryStreamsSupported(),
        .support_sampled_image_array_nonuniform_indexing =
            device.IsSampledImageArrayNonUniformIndexingSupported(),
        .support_storage_image_array_nonuniform_indexing =
            device.IsStorageImageArrayNonUniformIndexingSupported(),
        .support_uniform_texel_buffer_array_nonuniform_indexing =
            device.IsUniformTexelBufferArrayNonUniformIndexingSupported(),
        .support_storage_texel_buffer_array_nonuniform_indexing =
            device.IsStorageTexelBufferArrayNonUniformIndexingSupported(),

        .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),

        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
        .need_gather_subpixel_offset = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                       driver_id == VK_DRIVER_ID_MESA_RADV ||
                                       driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
                                       driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA ||
                                       driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_MESA_TURNIP,

        .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
        // EDEN_NX2_NAN_MINMAX=1 gives every driver the NaN-defined min, max and saturate, to tell
        // NaN artifacts apart from others (on NVIDIA they cost Donkey Kong Bananza 31 -> 49 ms).
        .has_nan_propagating_fminmax = is_amd || [] {
            const char* value = std::getenv("EDEN_NX2_NAN_MINMAX");
            return value != nullptr && value[0] == '1';
        }(),
        .has_broken_spirv_position_input = broken_position_input,
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
        .has_broken_fp32_denorm_flush = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic =
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
            device.GetDriverVersion() < VK_MAKE_VERSION(512, 672, 0),
        .has_broken_spirv_subgroup_shuffle = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_spirv_dynamic_subgroup_shuffle = BrokenDynamicSubgroupShuffle(driver_id),
        .max_subgroup_size = device.GetMaxSubgroupSize(),
        .has_broken_robust =
            device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_user_clip_distances = device.GetMaxUserClipDistances(),
        .cooperative_matrix_hmma_columns = device.GetHmmaCooperativeMatrixColumns(),
        .max_compute_shared_memory_size = device.GetMaxComputeSharedMemorySize(),
    };

    host_info = Shader::HostTranslateInfo{
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_per_stage_descriptor_sampled_images = device.GetMaxPerStageDescriptorSampledImages(),
        .max_per_stage_resources = device.GetMaxPerStageResources(),
        .max_per_stage_descriptor_storage_buffers = device.GetMaxPerStageDescriptorStorageBuffers(),
        .max_descriptor_set_samplers = device.GetMaxDescriptorSetSamplers(),
        .max_descriptor_set_uniform_buffers = device.GetMaxDescriptorSetUniformBuffers(),
        .max_descriptor_set_uniform_buffers_dynamic = device.GetMaxDescriptorSetUniformBuffersDynamic(),
        .max_descriptor_set_storage_buffers = device.GetMaxDescriptorSetStorageBuffers(),
        .max_descriptor_set_storage_buffers_dynamic = device.GetMaxDescriptorSetStorageBuffersDynamic(),
        .max_descriptor_set_sampled_images = device.GetMaxDescriptorSetSampledImages(),
        .max_descriptor_set_storage_images = device.GetMaxDescriptorSetStorageImages(),
        .max_descriptor_set_input_attachements = device.GetMaxDescriptorSetInputAttachments(),
        .support_float64 = device.IsFloat64Supported(),
        .support_float16 = device.IsFloat16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .needs_demote_reorder = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                driver_id == VK_DRIVER_ID_SAMSUNG_PROPRIETARY,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_conditional_barrier = device.SupportsConditionalBarriers(),
        .single_lane_geometry_subgroups = !profile.SupportsSubgroupStage(Shader::Stage::Geometry),
    };
    host_info.ApplyDescriptorLimitPolicy();

    if (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputAttributes is too low: {} < {}",
                    device.GetMaxVertexInputAttributes(), Maxwell::NumVertexAttributes);
    }
    if (device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputBindings is too low: {} < {}",
                    device.GetMaxVertexInputBindings(), Maxwell::NumVertexArrays);
    }

    LOG_INFO(Render_Vulkan, "DynamicState setting value: {}", u32(Settings::values.dyna_state.GetValue()));

    dynamic_features = {};
    dynamic_features.driver_id = device.GetDriverID();
    dynamic_features.driver_version = device.GetDriverVersion();

    // User granularity enforced in vulkan_device.cpp switch statement:
    //   Level 0: Core Dynamic States only
    //   Level 1: Core + EDS1
    //   Level 2: Core + EDS1 + EDS2 (accumulative)
    //   Level 3: Core + EDS1 + EDS2 + EDS3 (accumulative)
    // Here we only verify if extensions were successfully loaded by the device

    dynamic_features.has_extended_dynamic_state =
        device.IsExtExtendedDynamicStateSupported();

    dynamic_features.has_extended_dynamic_state_2 =
        device.IsExtExtendedDynamicState2Supported();
    dynamic_features.has_extended_dynamic_state_2_logic_op =
        device.IsExtExtendedDynamicState2ExtrasSupported();
    dynamic_features.has_extended_dynamic_state_2_patch_control_points = false;

    dynamic_features.has_extended_dynamic_state_3_blend =
        device.IsExtExtendedDynamicState3BlendingSupported();
    dynamic_features.has_extended_dynamic_state_3_enables =
        device.IsExtExtendedDynamicState3EnablesSupported();
    dynamic_features.has_color_write_enable =
        device.IsExtColorWriteEnableSupported();
    dynamic_features.has_dynamic_state3_depth_clamp_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3DepthClampEnable();
    dynamic_features.has_dynamic_state3_logic_op_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LogicOpEnable();
    dynamic_features.has_dynamic_state3_line_stipple_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LineStippleEnable();

    // VIDS: Independent toggle (not affected by dyna_state levels)
    dynamic_features.has_dynamic_vertex_input =
        device.IsExtVertexInputDynamicStateSupported() &&
        Settings::values.vertex_input_dynamic_state.GetValue();

    dynamic_features.has_provoking_vertex = device.IsExtProvokingVertexSupported();
    dynamic_features.has_provoking_vertex_first_mode =
        device.SupportsProvokingVertexFirstMode();
    dynamic_features.has_provoking_vertex_last_mode =
        device.SupportsProvokingVertexLastMode();
    dynamic_features.has_provoking_vertex_tf_preserve =
        device.SupportsTransformFeedbackProvokingVertexPreservation();

    if (use_asynchronous_shaders) {
        async_pipelines = AsyncPipelinesSetting();
        async_stats.window = std::chrono::steady_clock::now();
        if (async_pipelines != 0) {
            LOG_INFO(Render_Vulkan,
                     "EDEN_NX2_PIPELINE_ASYNC={}: graphics pipelines are translated and built on "
                     "the pipeline workers",
                     async_pipelines);
        }
    }
    build_priority = VideoCommon::BuildPriority::Enabled();
    if (build_priority) {
        LOG_INFO(Render_Vulkan, "EDEN_NX2_BUILD_PRIORITY: background pipeline builds wait while "
                                "the pipeline workers have builds to do");
    }
    use_shader_reuse = Common::Nx2::Setting("EDEN_NX2_SHADER_REUSE", "1") != "0";
    if (!use_shader_reuse) {
        LOG_INFO(Render_Vulkan, "EDEN_NX2_SHADER_REUSE=0: every new graphics pipeline translates "
                                "its shaders again");
    }
    const std::string stage_threads{Common::Nx2::Setting("EDEN_NX2_PARALLEL_STAGES", "0")};
    if (const int threads = std::clamp(std::atoi(stage_threads.c_str()), 0, 4); threads > 0) {
        stage_helpers.emplace(static_cast<size_t>(threads), "VkStageTranslate");
        LOG_INFO(Render_Vulkan,
                 "EDEN_NX2_PARALLEL_STAGES={}: the programs of a new graphics pipeline built on "
                 "the GPU thread are translated at once",
                 threads);
    }
}

std::shared_ptr<ShaderPools> PipelineCache::TakeSparePools() {
    {
        std::scoped_lock lock{spare_pools_mutex};
        if (!spare_pools.empty()) {
            std::shared_ptr<ShaderPools> pools = std::move(spare_pools.back());
            spare_pools.pop_back();
            return pools;
        }
    }
    return std::make_shared<ShaderPools>();
}

void PipelineCache::ReturnSparePools(std::shared_ptr<ShaderPools> pools) {
    pools->ReleaseContents();
    std::scoped_lock lock{spare_pools_mutex};
    if (spare_pools.size() < 4) {
        spare_pools.push_back(std::move(pools));
    }
}

namespace {
// <title>/vulkan_used.bin: the graphics pipelines recent sessions used, as key hash + sessions
// since the last use.
constexpr u32 UsedPipelinesMagic = 0x31505545; // "EUP1"
// Pipelines the last EDEN_NX2_USED_SESSIONS sessions used (2, Ocarina of Time 8) are built at
// boot, at most MaxPipelinesWithoutUsedList of them. Each holds ~0.45 MB of driver memory: title 1
// built 4,800 from eight sessions, 1.5 GB more than building them on first use (but that stalled
// frames 4x as often), while one session of its title screen uses ~1,500. With two sessions,
// Ocarina still built pipelines for 1-2 s of every 10 s early in play (Eglantine 88ad4037e4).
u8 UsedPipelinesMaxAge() {
    const std::string value{Common::Nx2::Setting("EDEN_NX2_USED_SESSIONS", "2")};
    const int sessions = std::atoi(value.c_str());
    return static_cast<u8>(std::clamp(sessions > 0 ? sessions : 2, 1, 16));
}

std::istringstream PayloadStream(std::span<const u8> payload) {
    return std::istringstream{
        std::string{reinterpret_cast<const char*>(payload.data()), payload.size()},
        std::ios::binary};
}

std::unordered_map<u64, u8> LoadUsedPipelines(std::istream& file) {
    std::unordered_map<u64, u8> used;
    const u8 max_age = UsedPipelinesMaxAge();
    u32 magic{}, count{};
    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != UsedPipelinesMagic ||
        !file.read(reinterpret_cast<char*>(&count), sizeof(count))) {
        return used;
    }
    used.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 hash{};
        u8 age{};
        if (!file.read(reinterpret_cast<char*>(&hash), sizeof(hash)) ||
            !file.read(reinterpret_cast<char*>(&age), sizeof(age))) {
            break;
        }
        if (age < max_age) { // lists saved with a longer window
            used.emplace(hash, age);
        }
    }
    return used;
}

/// Entries the last saved session used (age 0).
size_t RecentUsedPipelines(const std::unordered_map<u64, u8>& used) {
    return static_cast<size_t>(
        std::ranges::count_if(used, [](const auto& entry) { return entry.second == 0; }));
}

// Without a list of the pipelines recent sessions used (the first boot with this cache, or a session
// that ended before saving it), this many cached graphics pipelines at most are built at boot and the
// rest when first needed. A cache that grew over many builds held 48k pipelines, ~21 GB of driver
// memory built before the game started; the list was only saved at exit or after two minutes, so a
// boot that ran out of memory never saved one and the next boot did the same.
constexpr size_t MaxPipelinesWithoutUsedList = 3000;

// EDEN_NX2_PRELOAD_ALL_PIPELINES=1 builds every cached pipeline at boot whatever the memory left,
// as before.
bool PreloadAllPipelines() {
    static const bool all = [] {
        const char* value = std::getenv("EDEN_NX2_PRELOAD_ALL_PIPELINES");
        return value != nullptr && value[0] == '1';
    }();
    return all;
}

// pipeline_preload=All also builds the cached pipelines the used list leaves out, and keeps them.
bool PreloadAllCachedPipelines() {
    return Settings::values.pipeline_preload.GetValue() == Settings::PipelinePreload::All;
}

// Built pipelines the game may not need (~0.45 MB of driver memory each) are only built or kept
// while Windows has this much commit memory left; elsewhere it is not known and they always are.
constexpr u64 KeepPipelinesAboveAvailable = 8ULL << 30;

bool MemoryAllowsMorePipelines() {
    const auto available = Common::GetAvailableCommitMemory();
    return !available || *available >= KeepPipelinesAboveAvailable;
}

enum class BackgroundMode { Off, Auto, On };

// EDEN_NX2_BACKGROUND_PIPELINES: the cached graphics pipelines boot leaves out are built on
// low-priority threads once the game runs, each while memory allows (as with pipeline_preload=
// All), instead of on the GPU thread when a draw first needs one. Unset: for Switch 2 titles where
// the memory left is known (Windows), unless the driver can't compile pipelines in parallel (the
// prebuilds would run next to the pipeline workers). "1": for every title, and everywhere. "0":
// off.
BackgroundMode BackgroundPipelines() {
    static const BackgroundMode mode = [] {
        const char* value = std::getenv("EDEN_NX2_BACKGROUND_PIPELINES");
        if (value == nullptr) {
            return BackgroundMode::Auto;
        }
        return value[0] == '0' ? BackgroundMode::Off : BackgroundMode::On;
    }();
    return mode;
}

bool BuildSkippedPipelinesInBackground(bool broken_parallel_compiling) {
    switch (BackgroundPipelines()) {
    case BackgroundMode::Off:
        return false;
    case BackgroundMode::Auto:
        return !broken_parallel_compiling && Settings::CurrentProgramIsSwitch2() &&
               Common::GetAvailableCommitMemory().has_value();
    case BackgroundMode::On:
        return true;
    }
    return false;
}

// EDEN_NX2_BACKGROUND_PIPELINE_THREADS: how many threads build them (a quarter of the host's, 1 to
// 6, by default).
size_t BackgroundPipelineThreads() {
    if (const char* value = std::getenv("EDEN_NX2_BACKGROUND_PIPELINE_THREADS")) {
        return std::clamp<size_t>(static_cast<size_t>(std::strtoull(value, nullptr, 10)), 1, 16);
    }
    return std::clamp<size_t>(std::thread::hardware_concurrency() / 4, 1, 6);
}

// EDEN_NX2_USED_LIST_MERGE=0 starts a new two-session window at every save, as before.
bool MergeShortSessions() {
    static const bool merge = [] {
        const char* value = std::getenv("EDEN_NX2_USED_LIST_MERGE");
        return value == nullptr || value[0] != '0';
    }();
    return merge;
}

enum class NativeSyncMode { Off, Budget, Full, SkipScene };

// EDEN_NX2_NATIVE_SYNC_PIPELINES: unset or "budget" waits for a native pipeline within the
// budgets below before skipping the draw, "full" always waits, "0" skips while it builds, as before.
// When it is unset, Selevet's EDEN_NX2_ASYNC_SKIP is taken: "0" always waits, anything else
// skips scene geometry only (SkipScene). Ocarina of Time defaults to "full": its skipped draws
// left objects black or missing.
NativeSyncMode NativeSyncPipelines() {
    static Common::Nx2::SessionValue<NativeSyncMode> mode{[] {
        const char* skip = std::getenv("EDEN_NX2_ASYNC_SKIP");
        if (std::getenv("EDEN_NX2_NATIVE_SYNC_PIPELINES") == nullptr && skip != nullptr) {
            return skip[0] == '0' ? NativeSyncMode::Full : NativeSyncMode::SkipScene;
        }
        const std::string_view text =
            Common::Nx2::Setting("EDEN_NX2_NATIVE_SYNC_PIPELINES", "budget");
        if (text == "0" || text == "off") {
            return NativeSyncMode::Off;
        }
        return text == "full" || text == "1" ? NativeSyncMode::Full : NativeSyncMode::Budget;
    }};
    return mode.Get();
}

u32 NativeSyncMicroseconds(const char* name, std::string_view global_default) {
    const std::string_view text = Common::Nx2::Setting(name, global_default);
    u32 result = 0;
    std::from_chars(text.data(), text.data() + text.size(), result);
    return result;
}

// Per-draw wait in budget mode (EDEN_NX2_NATIVE_SYNC_BUDGET_US), and the most all draws wait in a
// window (EDEN_NX2_NATIVE_SYNC_WINDOW_US within EDEN_NX2_NATIVE_SYNC_WINDOW_LENGTH_US): 3 ms, and
// 8 ms a frame. At those, Ocarina of Time skipped 50-140 draws every 10 s (objects vanishing, black
// surfaces) for pipelines that took 2-25 ms, so it defaults to 50 ms, and 100 ms in 250 ms. Mario
// Kart World stalled up to 64 ms per draw with those mid-race (0.1.7-experimental.3 had 3 and 8).
std::chrono::microseconds NativeSyncDrawBudget() {
    static Common::Nx2::SessionValue<u32> budget{
        [] { return NativeSyncMicroseconds("EDEN_NX2_NATIVE_SYNC_BUDGET_US", "3000"); }};
    return std::chrono::microseconds{budget.Get()};
}

std::chrono::microseconds NativeSyncWindow() {
    static Common::Nx2::SessionValue<u32> length{
        [] { return NativeSyncMicroseconds("EDEN_NX2_NATIVE_SYNC_WINDOW_LENGTH_US", "16667"); }};
    return std::chrono::microseconds{length.Get()};
}

std::chrono::microseconds NativeSyncWindowBudget() {
    static Common::Nx2::SessionValue<u32> budget{
        [] { return NativeSyncMicroseconds("EDEN_NX2_NATIVE_SYNC_WINDOW_US", "8000"); }};
    return std::chrono::microseconds{budget.Get()};
}

// TEMPORARY Ocarina of Time haze and exposure workaround (oot_visual_workaround.h), on by
// default. EDEN_NX2_OOT_VISUAL_WORKAROUND=0 turns it off.
bool OotVisualWorkaround() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_OOT_VISUAL_WORKAROUND");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// Selevet's tuning knobs for it, with the same names and range: EDEN_NX2_OOT_FOG_SCALE,
// EDEN_NX2_OOT_EXPOSURE_SCALE and EDEN_NX2_OOT_BLOOM_EXP_SCALE (higher is weaker bloom). A value
// outside (0, 16) keeps the default.
const Shader::Optimization::OotVisualScales& OotVisualScales() {
    static const Shader::Optimization::OotVisualScales scales = [] {
        const auto read = [](const char* name, float fallback) {
            const char* value = std::getenv(name);
            if (value == nullptr) {
                return fallback;
            }
            const float scale = std::strtof(value, nullptr);
            return scale > 0.0f && scale < 16.0f ? scale : fallback;
        };
        Shader::Optimization::OotVisualScales result;
        result.fog = read("EDEN_NX2_OOT_FOG_SCALE", result.fog);
        result.exposure = read("EDEN_NX2_OOT_EXPOSURE_SCALE", result.exposure);
        result.bloom_exposure = read("EDEN_NX2_OOT_BLOOM_EXP_SCALE", result.bloom_exposure);
        return result;
    }();
    return scales;
}
} // Anonymous namespace

PipelineCache::~PipelineCache() {
    ReportNativePrecompileHits();
    // The warm-up builds use the members below.
    StopComputeWarm();
    {
        // Queued builds return without starting; running ones finish before anything they use
        // is destroyed.
        std::unique_lock lock{async_mutex};
        async_stopping = true;
        async_idle.wait(lock, [this] { return async_running == 0; });
    }
    StopBackgroundPipelines();
    serialization_thread.WaitForRequests();
    SaveUsedPipelines(false);
    VideoCommon::NativePrograms::End();
    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }
    if (pipeline_binaries_enabled) {
        device.GetLogical().DisablePipelineBinaryCache();
    }
}

void PipelineCache::SaveUsedPipelines(bool in_background) {
    used_pipelines_saved = std::chrono::steady_clock::now();
    if (used_pipelines_filename.empty()) {
        return;
    }
    // A session too short to say what the game uses (a boot to the menu) keeps the old list.
    constexpr size_t MinimumUsed = 64;
    const size_t session_used = used_this_session.size() + used_and_evicted.size();
    if (session_used < MinimumUsed) {
        LOG_INFO(Render_Vulkan,
                 "Used pipelines saved: {} this session, {} from earlier sessions, {} total "
                 "(skipped)",
                 session_used, used_pipelines.size(), used_pipelines.size());
        return;
    }
    std::unordered_map<u64, u8> next;
    next.reserve(used_pipelines.size() + used_this_session.size());
    for (const auto& [key, pipeline] : graphics_cache) {
        if (used_this_session.contains(pipeline.get())) {
            next.emplace(key.Hash(), u8{0});
        }
    }
    for (const u64 hash : used_and_evicted) {
        next.emplace(hash, u8{0});
    }
    const size_t from_session = next.size();
    // A session that used under half of what the last one did (closed on the loading screen,
    // say) is added to the list as it is: shifting the window would drop the older session's
    // pipelines, and the next boot would build them while loading. Bounded like a boot without
    // a list.
    bool merged = false;
    const u8 max_age = UsedPipelinesMaxAge();
    if (MergeShortSessions() && session_used * 2 < RecentUsedPipelines(used_pipelines)) {
        size_t merged_size = from_session;
        for (const auto& entry : used_pipelines) {
            merged_size += next.contains(entry.first) ? 0 : 1;
        }
        if (merged_size <= MaxPipelinesWithoutUsedList) {
            merged = true;
            for (const auto& [hash, age] : used_pipelines) {
                next.try_emplace(hash, age);
            }
        }
    }
    // After merges the last session's entries can be several sessions': a shift keeps them, after
    // this session's, within the same bound.
    size_t left_out = 0;
    if (!merged) {
        for (const auto& [hash, age] : used_pipelines) {
            if (age + 1 >= max_age || next.contains(hash)) {
                continue;
            }
            if (MergeShortSessions() && next.size() >= MaxPipelinesWithoutUsedList) {
                ++left_out;
                continue;
            }
            next.emplace(hash, static_cast<u8>(age + 1));
        }
    }
    LOG_INFO(Render_Vulkan,
             "Used pipelines saved: {} this session, {} from earlier sessions, {} total ({}), {} "
             "earlier ones over {} left out",
             from_session, next.size() - from_session, next.size(),
             merged ? "merged" : "shifted", left_out, MaxPipelinesWithoutUsedList);
    const u32 count = static_cast<u32>(next.size());
    std::vector<char> bytes;
    bytes.reserve(sizeof(UsedPipelinesMagic) + sizeof(count) + next.size() * (sizeof(u64) + 1));
    const auto append = [&bytes](const auto& value) {
        const auto* const data = reinterpret_cast<const char*>(&value);
        bytes.insert(bytes.end(), data, data + sizeof(value));
    };
    append(UsedPipelinesMagic);
    append(count);
    for (const auto& [hash, age] : next) {
        append(hash);
        append(age);
    }
    if (cache_v2) {
        cache_v2->Append(VideoCommon::ShaderCacheV2::RecordKind::Used, 0,
                         std::span{reinterpret_cast<const u8*>(bytes.data()), bytes.size()});
        return;
    }
    // The periodic saves run on the GPU thread: the file is written on the serialization thread.
    const auto write = [filename = used_pipelines_filename, bytes = std::move(bytes)] {
        std::ofstream file{filename, std::ios::binary | std::ios::trunc};
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    if (in_background) {
        serialization_thread.QueueWork(std::move(write));
    } else {
        write();
    }
}

namespace {
// EDEN_NX2_PIPELINE_CLEAN=1 (default off): see CurrentGraphicsPipelineClean.
bool PipelineCleanEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PIPELINE_CLEAN");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}
} // Anonymous namespace

void PipelineCache::BuildCleanWatch() {
    using Range = CleanLookup::Range;
    const Maxwell& regs = maxwell3d->regs;
    const DynamicFeatures& features = dynamic_features;
    const auto watch_into = [&regs](std::vector<Range>& ranges, const auto& field) {
        const auto offset = static_cast<size_t>(reinterpret_cast<const char*>(&field) -
                                                reinterpret_cast<const char*>(&regs));
        const size_t first = offset / sizeof(u32);
        const size_t last = (offset + sizeof(field) + sizeof(u32) - 1) / sizeof(u32);
        ranges.push_back({static_cast<u32>(first), static_cast<u32>(last - first)});
    };
    std::vector<Range> ranges;
    const auto watch = [&](const auto& field) { watch_into(ranges, field); };

    // Every register FixedPipelineState::Refresh (and what it calls) reads with these features.
    // Refresh itself, always:
    watch(regs.transform_feedback_enabled);
    watch(regs.depth_mode);
    watch(regs.polygon_mode_front);
    watch(regs.tessellation);
    watch(regs.patch_vertices);
    watch(regs.anti_alias_samples_mode);
    watch(regs.blend);                    // ComputeAttachment0DualSourceBlend
    watch(regs.blend_per_target_enabled); // ComputeAttachment0DualSourceBlend
    watch(regs.blend_per_target[0]);      // ComputeAttachment0DualSourceBlend
    watch(regs.alpha_test_enabled);
    watch(regs.alpha_test_func);
    watch(regs.alpha_test_ref);
    watch(regs.mandated_early_z);
    watch(regs.zeta_enable);
    watch(regs.zeta.format);
    watch(regs.window_origin);
    watch(regs.provoking_vertex);
    watch(regs.conservative_raster_enable);
    watch(regs.line_anti_alias_enable);
    watch(regs.anti_alias_alpha_control);
    watch(regs.depth_bounds);
    watch(regs.line_stipple_params);
    for (const auto& rt : regs.rt) {
        watch(rt.format);
    }
    watch(regs.point_size);
    // Dirty::VertexInput (both branches):
    watch(regs.vertex_attrib_format);
    if (!features.has_dynamic_vertex_input) {
        watch(regs.vertex_stream_instances);
        for (const auto& stream : regs.vertex_streams) {
            watch(stream.frequency);
        }
    }
    if (!features.has_extended_dynamic_state) {
        // Strides (vertex_strides): the first word of each stream, VertexStream's leading union
        // of stride and enable.
        for (const auto& stream : regs.vertex_streams) {
            const auto offset = static_cast<size_t>(reinterpret_cast<const char*>(&stream) -
                                                    reinterpret_cast<const char*>(&regs));
            ranges.push_back({static_cast<u32>(offset / sizeof(u32)), 1});
        }
    }
    // Dirty::ViewportSwizzles:
    for (const auto& viewport : regs.viewport_transform) {
        watch(viewport.swizzle);
    }
    // DynamicState::Refresh (no extended dynamic state):
    if (!features.has_extended_dynamic_state) {
        watch(regs.gl_front_face);
        watch(regs.stencil_front_op);
        watch(regs.stencil_two_side_enable);
        watch(regs.stencil_back_op);
        watch(regs.stencil_enable);
        watch(regs.depth_write_enabled);
        watch(regs.depth_bounds_enable);
        watch(regs.depth_test_enable);
        watch(regs.depth_test_func);
        watch(regs.gl_cull_face);
        watch(regs.gl_cull_test_enabled);
    }
    // DynamicState::Refresh2:
    if (!features.has_extended_dynamic_state_2_logic_op) {
        watch(regs.logic_op);
        if (!features.has_extended_dynamic_state_2) {
            watch(regs.polygon_offset_point_enable);
            watch(regs.polygon_offset_line_enable);
            watch(regs.polygon_offset_fill_enable);
            watch(regs.rasterize_enable);
            watch(regs.primitive_restart);
        }
    }
    // Dirty::Blending, BlendingAttachment::Refresh (no extended dynamic state 3 blending). Native
    // programs refresh these at every draw whatever the flag says: equal values give the same key.
    if (!features.has_extended_dynamic_state_3_blend) {
        watch(regs.color_mask_common);
        watch(regs.color_mask);
        watch(regs.blend_per_target);
        watch(regs.iterated_blend);
    }
    // DynamicState::Refresh3:
    if (!features.has_dynamic_state3_logic_op_enable) {
        watch(regs.logic_op);
    }
    if (!features.has_dynamic_state3_depth_clamp_enable) {
        watch(regs.viewport_clip_control);
    }
    if (!features.has_dynamic_state3_line_stipple_enable) {
        watch(regs.line_stipple_enable);
    }

    // Sorted and merged: contiguous words compare as one block.
    const auto merge = [](std::vector<Range> input) {
        std::ranges::sort(input, {}, &Range::first);
        std::vector<Range> merged;
        for (const Range& range : input) {
            if (!merged.empty() && range.first <= merged.back().first + merged.back().count) {
                const u32 end = (std::max)(merged.back().first + merged.back().count,
                                           range.first + range.count);
                merged.back().count = end - merged.back().first;
            } else {
                merged.push_back(range);
            }
        }
        return merged;
    };
    clean.ranges = merge(std::move(ranges));
    // RefreshXfbState, while transform_feedback_enabled (watched above) is set:
    std::vector<Range> xfb_ranges;
    watch_into(xfb_ranges, regs.transform_feedback.controls);
    watch_into(xfb_ranges, regs.stream_out_layout);
    clean.xfb_ranges = merge(std::move(xfb_ranges));

    size_t words = 0;
    for (const Range& range : clean.ranges) {
        words += range.count;
    }
    clean.snapshot.assign(words, 0);
    size_t xfb_words = 0;
    for (const Range& range : clean.xfb_ranges) {
        xfb_words += range.count;
    }
    clean.xfb_snapshot.assign(xfb_words, 0);
    clean.built = true;
    LOG_INFO(Render_Vulkan, "EDEN_NX2_PIPELINE_CLEAN: watching {} register words in {} ranges",
             words, clean.ranges.size());
}

bool PipelineCache::CleanInputsUnchanged() const {
    const u32* const words = maxwell3d->regs.reg_array.data();
    const u32* saved = clean.snapshot.data();
    for (const CleanLookup::Range& range : clean.ranges) {
        if (std::memcmp(words + range.first, saved, range.count * sizeof(u32)) != 0) {
            return false;
        }
        saved += range.count;
    }
    if (maxwell3d->regs.transform_feedback_enabled != 0) {
        saved = clean.xfb_snapshot.data();
        for (const CleanLookup::Range& range : clean.xfb_ranges) {
            if (std::memcmp(words + range.first, saved, range.count * sizeof(u32)) != 0) {
                return false;
            }
            saved += range.count;
        }
    }
    return true;
}

void PipelineCache::TakeCleanSnapshot() {
    const u32* const words = maxwell3d->regs.reg_array.data();
    u32* saved = clean.snapshot.data();
    for (const CleanLookup::Range& range : clean.ranges) {
        std::memcpy(saved, words + range.first, range.count * sizeof(u32));
        saved += range.count;
    }
    if (maxwell3d->regs.transform_feedback_enabled != 0) {
        saved = clean.xfb_snapshot.data();
        for (const CleanLookup::Range& range : clean.xfb_ranges) {
            std::memcpy(saved, words + range.first, range.count * sizeof(u32));
            saved += range.count;
        }
    }
    clean.hashes = graphics_key.unique_hashes;
    clean.maxwell3d = maxwell3d;
    clean.topology = maxwell3d->draw_manager.draw_state.topology;
    clean.engine_state = maxwell3d->engine_state;
}

// The key is (unique_hashes, FixedPipelineState). Refresh's output depends only on: the
// registers in clean.ranges (and clean.xfb_ranges while transform feedback is on), the dirty
// flags VertexInput, ViewportSwizzles and Blending (it recomputes those groups only while set,
// and clears some), dynamic_features (set once at construction), the draw's topology and the
// engine hint. When all of these equal their values at the last Refresh and the flags are
// clear, Refresh would rewrite the same key and Next(graphics_key) would return
// current_pipeline (whose key it is): the lookup returns it as that fast path does.
GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineClean() {
    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        clean.pipeline = nullptr;
        return nullptr;
    }
    if (!clean.built) {
        BuildCleanWatch();
    }
    const auto& flags = maxwell3d->dirty.flags;
    const bool blending_dirty =
        !dynamic_features.has_extended_dynamic_state_3_blend && flags[Dirty::Blending];
    if (clean.pipeline != nullptr && clean.pipeline == current_pipeline &&
        clean.maxwell3d == maxwell3d && !flags[Dirty::VertexInput] &&
        !flags[Dirty::ViewportSwizzles] && !blending_dirty &&
        clean.hashes == graphics_key.unique_hashes &&
        clean.topology == maxwell3d->draw_manager.draw_state.topology &&
        clean.engine_state == maxwell3d->engine_state && CleanInputsUnchanged()) {
        ++clean.hits;
    } else {
        ++clean.misses;
        clean.pipeline = nullptr;
    }
    if (((clean.hits + clean.misses) & 0xfff) == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (clean.last_log == std::chrono::steady_clock::time_point{}) {
            clean.last_log = now;
        } else if (now - clean.last_log >= std::chrono::seconds{10}) {
            const double seconds = std::chrono::duration<double>(now - clean.last_log).count();
            LOG_INFO(Render_Vulkan,
                     "EDEN_NX2_PIPELINE_CLEAN per second: lookups without Refresh {}, "
                     "with Refresh {}",
                     static_cast<u64>(static_cast<double>(clean.hits) / seconds),
                     static_cast<u64>(static_cast<double>(clean.misses) / seconds));
            clean.last_log = now;
            clean.hits = 0;
            clean.misses = 0;
        }
    }
    if (clean.pipeline != nullptr) {
        current_pipeline->MarkUsed(frame_count);
        return BuiltPipeline(current_pipeline);
    }

    TakeCleanSnapshot();
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);
    if (current_pipeline) {
        GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
        if (next) {
            current_pipeline = next;
            clean.pipeline = next;
            current_pipeline->MarkUsed(frame_count);
            return BuiltPipeline(current_pipeline);
        }
    }
    // The slow path makes graphics_cache[graphics_key] current, unless it has no pipeline.
    GraphicsPipeline* const previous = current_pipeline;
    GraphicsPipeline* const result = CurrentGraphicsPipelineSlowPath();
    if (current_pipeline != previous) {
        clean.pipeline = current_pipeline;
    }
    return result;
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    last_pipeline_building = false;
    if (async_pipelines != 0 && async_completed_count.load(std::memory_order_acquire) != 0) {
        const auto begin = std::chrono::steady_clock::now();
        DrainAsyncPipelines();
        async_stats.gpu_time += std::chrono::steady_clock::now() - begin;
    }
    if (PipelineCleanEnabled()) {
        return CurrentGraphicsPipelineClean();
    }
    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        return nullptr;
    }
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);

    if (current_pipeline) {
        GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
        if (next) {
            current_pipeline = next;
            current_pipeline->MarkUsed(frame_count);
            return BuiltPipeline(current_pipeline);
        }
    }
    return CurrentGraphicsPipelineSlowPath();
}

constexpr u32 MaxComputeRebuilds = 4;

namespace {
/// The constant buffer slots each program of key binds as storage buffer pointers. envs holds an
/// environment per program, or (compact) one per program the key uses.
template <typename Envs>
VideoCommon::EvictedPointerSlots::Slots MappedPointerSlots(const GraphicsPipelineCacheKey& key,
                                                           const Envs& envs, bool compact) {
    static_assert(VideoCommon::EvictedPointerSlots::NumPrograms == Maxwell::MaxShaderProgram);
    VideoCommon::EvictedPointerSlots::Slots slots;
    size_t env_index = 0;
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] != 0) {
            slots[index] = envs[compact ? env_index++ : index].MappedStoragePointerKeys();
        }
    }
    return slots;
}

/// Which pointer slots a native program's arguments fill depends on the area (Ocarina of Time's
/// foliage culling fills different list slots in Hyrule Field and Castle Town), so a translation
/// rebuilt in one area may lack pointers the first cached one binds. Try first the translations
/// that bind every pointer the first one binds, those binding the most first, then the others in
/// cache order.
void OrderComputeTranslations(std::vector<FileEnvironment>& candidates) {
    if (candidates.size() < 2) {
        return;
    }
    std::vector<std::vector<u64>> keys;
    keys.reserve(candidates.size());
    for (const FileEnvironment& env : candidates) {
        keys.push_back(env.MappedStoragePointerKeys());
    }
    std::vector<size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const auto keeps_first = [&](size_t index) {
        return std::ranges::includes(keys[index], keys.front());
    };
    std::ranges::stable_sort(order, [&](size_t lhs, size_t rhs) {
        const bool lhs_keeps{keeps_first(lhs)};
        if (lhs_keeps != keeps_first(rhs)) {
            return lhs_keeps;
        }
        return lhs_keeps && keys[lhs].size() > keys[rhs].size();
    });
    std::vector<FileEnvironment> ordered;
    ordered.reserve(candidates.size());
    for (const size_t index : order) {
        ordered.push_back(std::move(candidates[index]));
    }
    candidates = std::move(ordered);
}
} // Anonymous namespace

ComputePipeline* PipelineCache::CurrentComputePipeline(u64& native_key) {

    const ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        return nullptr;
    }
    native_key = shader->lowered_native ? shader->lowered_native->native_program_key : 0;
    const auto& qmd{kepler_compute->launch_description};
    const ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    const auto [pair, is_new]{compute_cache.try_emplace(key)};
    auto& pipeline{pair->second};
    if (is_new) {
        NoteNativePrecompileHit(key);
        ComputeRebuildInfo rebuild_info;
        pipeline = CreateComputePipeline(key, shader, rebuild_info);
        if (pipeline && !rebuild_info.null_pointer_slots.empty()) {
            compute_rebuild_info.insert_or_assign(key, std::move(rebuild_info));
        }
        return pipeline.get();
    }
    const auto stale{compute_rebuild_info.find(key)};
    if (stale == compute_rebuild_info.end() || stale->second.rebuilds >= MaxComputeRebuilds ||
        !StaleNullPointerSlot(stale->second)) {
        return pipeline.get();
    }
    // The translation dropped accesses through a pointer that was null back then. Which slots
    // the game fills depends on the area, so the new one keeps binding every pointer this one
    // binds, null now or not: a null pointer binds nothing.
    auto& info{stale->second};
    ComputeRebuildInfo rebuilt_info;
    auto rebuilt{CreateComputePipeline(key, shader, rebuilt_info, info.mapped_pointers)};
    if (!rebuilt) {
        // Keep the working translation when the new one cannot be built (for example a narrow
        // access that now stays global across two buffers), and stop retrying.
        LOG_WARNING(Render_Vulkan, "Keeping compute shader {:016x}: its rebuild failed",
                    key.unique_hash);
        info.rebuilds = MaxComputeRebuilds;
        return pipeline.get();
    }
    if (pipeline) {
        retired_compute_pipelines.emplace_back(scheduler.CurrentTick(), std::move(pipeline));
    }
    pipeline = std::move(rebuilt);
    rebuilt_info.rebuilds = info.rebuilds + 1;
    info = std::move(rebuilt_info);
    return pipeline.get();
}

bool PipelineCache::StaleNullPointerSlot(const ComputeRebuildInfo& info) const {
    const auto& qmd{kepler_compute->launch_description};
    for (const auto& [index, offset] : info.null_pointer_slots) {
        if (index >= qmd.const_buffer_config.size() ||
            ((qmd.const_buffer_enable_mask >> index) & 1) == 0) {
            continue;
        }
        const auto& cbuf{qmd.const_buffer_config[index]};
        if (offset + sizeof(u64) > cbuf.size) {
            continue; // The translator reads past the bound size as zero, too
        }
        const GPUVAddr slot{cbuf.Address() + offset};
        if (!gpu_memory->IsFullyMappedRange(slot, sizeof(u64))) {
            continue;
        }
        const GPUVAddr address{gpu_memory->Read<u64>(slot)};
        if (address != 0 && gpu_memory->IsWithinGPUAddressRange(address) &&
            gpu_memory->IsFullyMappedRange(address, 1)) {
            LOG_INFO(Render_Vulkan,
                     "Rebuilding compute program {:#x}: c{}[{:#x}] was null at translation and "
                     "now points to {:#x}",
                     kepler_compute->regs.code_loc.Address() + qmd.program_start, index, offset,
                     address);
            return true;
        }
    }
    return false;
}

namespace {
u64 ShaderSetHash(const GraphicsPipelineCacheKey& key) {
    return Common::CityHash64(reinterpret_cast<const char*>(key.unique_hashes.data()),
                              sizeof(key.unique_hashes));
}

constexpr std::array<const char*, 24> KeyFieldNames{
    "topology",        "msaa",           "xfb",          "polygon_mode",   "tessellation",
    "dual_source",     "features",       "alpha_func",   "depth",          "raster",
    "alpha_coverage",  "app_stage",      "raw_other",    "color_formats",  "driver",
    "alpha_test_ref",  "point_size",     "viewport_swizzles", "attribute_types",
    "dynamic_state",   "blending",       "vertex_attributes", "vertex_bindings", "tail",
};

/// Which parts of the fixed state differ (bits of KeyFieldNames), over the bytes the key uses.
u32 KeyDifferences(const FixedPipelineState& now, const FixedPipelineState& before) {
    u32 fields = 0;
    const auto bits = [&fields](u32 diff, u32 mask, u32 field) {
        if ((diff & mask) != 0) {
            fields |= 1U << field;
        }
    };
    const u32 x1 = now.raw1 ^ before.raw1;
    const u32 x2 = now.raw2 ^ before.raw2;
    bits(x1, 0x0F000000, 0);
    bits(x1, 0xF0000000, 1);
    bits(x1, 0x00000040, 2);
    bits(x1, 0x00000300, 3);
    bits(x1, 0x000FFC00, 4);
    bits(x1, 0x00200000, 5);
    bits(x1, 0x001000BF, 6);
    bits(x1, 0x00C00000, 12);
    bits(x2, 0x0000000E, 7);
    bits(x2, 0x000007F0, 8);
    bits(x2, 0x00007800, 9);
    bits(x2, 0x00018000, 10);
    bits(x2, 0x000E0000, 11);
    bits(x2, 0xFFF00001, 12);
    const size_t size = (std::max)(now.Size(), before.Size());
    const auto bytes = [&](size_t begin, size_t end, u32 field) {
        end = (std::min)(end, size);
        if (begin < end && std::memcmp(reinterpret_cast<const u8*>(&now) + begin,
                                       reinterpret_cast<const u8*>(&before) + begin,
                                       end - begin) != 0) {
            fields |= 1U << field;
        }
    };
    using S = FixedPipelineState;
    bytes(offsetof(S, color_formats), offsetof(S, driver_id), 13);
    bytes(offsetof(S, driver_id), offsetof(S, alpha_test_ref), 14);
    bytes(offsetof(S, alpha_test_ref), offsetof(S, point_size), 15);
    bytes(offsetof(S, point_size), offsetof(S, viewport_swizzles), 16);
    bytes(offsetof(S, viewport_swizzles), offsetof(S, attribute_types), 17);
    bytes(offsetof(S, attribute_types), offsetof(S, dynamic_state), 18);
    bytes(offsetof(S, dynamic_state), offsetof(S, attachments), 19);
    bytes(offsetof(S, attachments), offsetof(S, attributes), 20);
    bytes(offsetof(S, attributes), offsetof(S, binding_divisors), 21);
    bytes(offsetof(S, binding_divisors), offsetof(S, xfb_state), 22);
    bytes(offsetof(S, xfb_state), sizeof(S), 23);
    return fields;
}

std::string DescribeKeyFields(u32 fields, size_t count) {
    std::string text;
    for (size_t field = 0; field < count; ++field) {
        if ((fields >> field) & 1) {
            text += text.empty() ? "" : ", ";
            text += KeyFieldNames[field];
        }
    }
    return text.empty() ? std::string{"nothing in the fixed state"} : text;
}

/// The fields with the most variants first, at most 6.
template <size_t N>
std::string DescribeFieldCounts(const std::array<u32, N>& counts) {
    std::array<size_t, N> order;
    for (size_t i = 0; i < N; ++i) {
        order[i] = i;
    }
    std::ranges::stable_sort(order, [&](size_t a, size_t b) { return counts[a] > counts[b]; });
    std::string text;
    for (size_t i = 0; i < (std::min)(N, size_t{6}) && counts[order[i]] != 0; ++i) {
        text += fmt::format("{}{} {}", text.empty() ? "" : ", ", KeyFieldNames[order[i]],
                            counts[order[i]]);
    }
    return text.empty() ? std::string{"-"} : text;
}
} // Anonymous namespace

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    if (!pipeline_cache_filename.empty()) {
        // A later load (another title or program) stops the earlier warm-up first.
        StopComputeWarm();
        serialization_thread.WaitForRequests();
        if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
            SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                         CACHE_VERSION);
        }
    }

    const auto shader_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir)};
    const auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    {
        std::scoped_lock lock{background_mutex};
        incomplete_records.clear();
    }
    pipeline_cache_filename = base_dir / "vulkan.bin";
    mesh_pipeline_cache_filename = base_dir / "vulkan_mesh.bin";
    used_pipelines_filename = base_dir / "vulkan_used.bin";
    namespace CacheV2 = VideoCommon::ShaderCacheV2;
    VideoCommon::NativePrograms::End();
    cache_v2.reset();
    std::optional<CacheV2::LogFile> log_v2;
    if (CacheV2::Enabled()) {
        const auto log_path = base_dir / CacheV2::FileName;
        log_v2.emplace(CacheV2::ReadLogFile(log_path));
        if (log_v2->header == CacheV2::HeaderStatus::OtherVersion) {
            LOG_WARNING(Render_Vulkan, "Shader cache v2: {} is another format version's; the "
                                       "older cache files are used",
                        Common::FS::PathToUTF8String(log_path));
            log_v2.reset();
        }
    }
    if (log_v2) {
        const auto log_path = base_dir / CacheV2::FileName;
        const PipelineCacheFile::LogImport imported =
            PipelineCacheFile::ImportIntoLog(base_dir, log_path, *log_v2);
        if (!imported.sources.empty() || imported.used) {
            std::string sources;
            for (const std::string& source : imported.sources) {
                sources += (sources.empty() ? "" : ", ") + source;
            }
            LOG_INFO(Render_Vulkan,
                     "Shader cache v2 took {} graphics and {} compute pipelines from {}{}",
                     imported.graphics, imported.compute, sources.empty() ? "no file" : sources,
                     imported.used ? ", and the used list" : "");
            log_v2.emplace(CacheV2::ReadLogFile(log_path));
        }
        if (imported.failed) {
            LOG_WARNING(Render_Vulkan, "Shader cache v2 couldn't write all it took to {}",
                        Common::FS::PathToUTF8String(log_path));
        }
        const CacheV2::LogStats& stats = log_v2->stats;
        LOG_INFO(Render_Vulkan,
                 "Shader cache v2: {} bytes, {} live records, {} superseded, {} tombstones, {} "
                 "damaged, {} bytes skipped, {} bytes cut off",
                 stats.file_size, stats.live, stats.superseded, stats.tombstones, stats.damaged,
                 stats.skipped_bytes, stats.torn_tail_bytes);
        cache_v2.emplace(log_path);
    }
    // AMD's Windows driver keeps no compiled pipelines between sessions; NVIDIA's keeps them all
    // in its own disk cache, so saving binaries there would only duplicate it.
    if (Common::Nx2::Setting("EDEN_NX2_PIPELINE_BINARIES", "1") != "0" &&
        device.IsKhrPipelineBinarySupported() &&
        device.GetDriverID() != VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
        device.GetLogical().EnablePipelineBinaryCache(base_dir / "vulkan_binaries.bin");
        pipeline_binaries_enabled = true;
    }
    if (log_v2) {
        used_pipelines.clear();
        for (const CacheV2::LogRecord& record : log_v2->live) {
            if (record.kind == CacheV2::RecordKind::Used) {
                std::istringstream stream = PayloadStream(record.payload);
                used_pipelines = LoadUsedPipelines(stream);
            }
        }
    } else {
        std::ifstream file{used_pipelines_filename, std::ios::binary};
        used_pipelines = LoadUsedPipelines(file);
    }
    const size_t recent_used = RecentUsedPipelines(used_pipelines);
    LOG_INFO(Render_Vulkan, "Used pipeline list: {} hashes ({} from the last session, {} older)",
             used_pipelines.size(), recent_used, used_pipelines.size() - recent_used);
    used_pipelines_saved = std::chrono::steady_clock::now();
    const bool preload_all_cached = PreloadAllCachedPipelines();
    const bool prepare = Settings::values.prepare_shaders;
    const bool repair = VideoCommon::PipelineCacheRepair();
    cache_repair = repair;
    const auto load_start = std::chrono::steady_clock::now();
    const bool build_skipped_in_background =
        BuildSkippedPipelinesInBackground(device.GetPipelineWorkerLimit() == 1);
    size_t skipped_pipelines = 0;
    size_t queued_graphics = 0;
    std::vector<std::pair<GraphicsPipelineCacheKey, std::vector<FileEnvironment>>> skipped_graphics;
    std::unordered_map<u64, size_t> skipped_index; ///< Key hash -> index in skipped_graphics
    /// Later records of pipelines queued at boot, by key hash (see load_graphics)
    std::unordered_map<u64, std::pair<GraphicsPipelineCacheKey, std::vector<FileEnvironment>>>
        later_graphics;
    size_t migrated_pipelines = 0;
    size_t feature_mismatched_pipelines = 0;

    if (use_vulkan_pipeline_cache) {
        vulkan_pipeline_cache_filename = base_dir / "vulkan_pipelines.bin";
        vulkan_pipeline_cache =
            LoadVulkanPipelineCache(vulkan_pipeline_cache_filename, CACHE_VERSION);
    }

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        size_t unlisted_built{};
        size_t unlisted_deferred{};
        size_t released{};
        std::atomic_bool low_memory{};
        bool has_loaded{};
        std::unique_ptr<PipelineStatistics> statistics;
        /// Key hashes of graphics builds that read values their records lack
        std::vector<u64> incomplete;
    } state;

    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        state.statistics = std::make_unique<PipelineStatistics>(device);
    }
    // A rebuilt translation is cached beside the stale one. Choose between them before building,
    // so loading only ever adds entries (see OrderComputeTranslations), and take the next when a
    // translation fails.
    ::Common::unordered_map<ComputePipelineCacheKey, std::vector<FileEnvironment>> compute_envs;
    const auto load_compute{[&](std::istream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        NoteCachedProgram(key.unique_hash);
        auto& candidates{compute_envs[key]};
        candidates.push_back(std::move(env));
        if (candidates.size() == 1) {
            ++state.total;
        }
    }};
    const auto queue_compute{[&](const ComputePipelineCacheKey& key,
                                 std::vector<FileEnvironment> candidates) {
        workers.QueueWork([this, key, envs_ = std::move(candidates), &state,
                           &callback]() mutable {
            ShaderPools pools;
            std::unique_ptr<ComputePipeline> pipeline;
            ComputeRebuildInfo rebuild_info;
            for (auto& env : envs_) {
                pools.ReleaseContents();
                pipeline = CreateComputePipeline(pools, key, env, state.statistics.get(), false);
                if (pipeline) {
                    if (env.IsNativeProgram()) {
                        rebuild_info.null_pointer_slots = env.NullStoragePointerSlots();
                        rebuild_info.mapped_pointers = env.MappedStoragePointerKeys();
                    }
                    break;
                }
            }
            std::scoped_lock lock{state.mutex};
            if (pipeline && compute_cache.emplace(key, std::move(pipeline)).second &&
                !rebuild_info.null_pointer_slots.empty()) {
                compute_rebuild_info.insert_or_assign(key, std::move(rebuild_info));
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
    }};
    const auto queue_graphics{[&](const GraphicsPipelineCacheKey& key,
                                  std::vector<FileEnvironment> envs, bool listed) {
        workers.QueueWork([this, key, envs_ = std::move(envs), listed, prepare, repair, &state,
                           &callback]() mutable {
            // Checked as they build, as memory goes down; once low, the rest wait until needed.
            const bool deferred =
                !listed && !prepare &&
                (state.low_memory.load(std::memory_order_relaxed) || !MemoryAllowsMorePipelines());
            std::unique_ptr<GraphicsPipeline> pipeline;
            if (deferred) {
                state.low_memory.store(true, std::memory_order_relaxed);
            } else {
                ShaderPools pools;
                boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram>
                    env_ptrs;
                for (auto& env : envs_) {
                    env_ptrs.push_back(&env);
                }
                pipeline = CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs),
                                                  state.statistics.get(), false);
                if (pipeline) {
                    pipeline->SetMappedPointers(MappedPointerSlots(key, envs_, true));
                }
            }
            const bool incomplete =
                repair && !deferred && !pipeline &&
                std::ranges::any_of(envs_, &FileEnvironment::LacksRecordedValue);
            // A prepare run builds what memory can't hold too, then drops it: the build alone
            // fills the driver's cache, so the game's later builds of it are quick.
            const bool released = pipeline && !listed && prepare && !MemoryAllowsMorePipelines();
            if (released) {
                pipeline.reset();
            }

            std::scoped_lock lock{state.mutex};
            if (released) {
                ++state.released;
            }
            if (incomplete) {
                state.incomplete.push_back(key.Hash());
            }
            if (pipeline) {
                graphics_cache.emplace(key, std::move(pipeline));
                if (!listed) {
                    ++state.unlisted_built;
                }
            }
            if (deferred) {
                ++state.unlisted_deferred;
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        });
        ++state.total;
    }};
    bool reading_mesh_file = false;
    const bool reading_log = log_v2.has_value();
    std::optional<CacheV2::LogStats> log_stats;
    const auto load_graphics{[&](std::istream& file, std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        for (const u64 unique_hash : key.unique_hashes) {
            if (unique_hash != 0) {
                NoteCachedProgram(unique_hash);
            }
        }

        // Task/mesh pipelines are kept in their own file, whose records carry the mesh registers
        // they are translated with; vulkan.bin never held any.
        const bool mesh = std::ranges::any_of(envs, [](const FileEnvironment& env) {
            return env.ShaderStage() == Shader::Stage::Task ||
                   env.ShaderStage() == Shader::Stage::Mesh;
        });
        if ((!reading_log && mesh != reading_mesh_file) ||
            (mesh && !device.IsExtMeshShaderSupported())) {
            return;
        }

        if ((key.state.extended_dynamic_state != 0) !=
                dynamic_features.has_extended_dynamic_state ||
            (key.state.extended_dynamic_state_2 != 0) !=
                dynamic_features.has_extended_dynamic_state_2 ||
            (key.state.extended_dynamic_state_2_logic_op != 0) !=
                dynamic_features.has_extended_dynamic_state_2_logic_op ||
            (key.state.extended_dynamic_state_3_blend != 0) !=
                dynamic_features.has_extended_dynamic_state_3_blend ||
            (key.state.extended_dynamic_state_3_enables != 0) !=
                dynamic_features.has_extended_dynamic_state_3_enables ||
            (key.state.color_write_enable_dynamic != 0) !=
                dynamic_features.has_color_write_enable ||
            (key.state.dynamic_vertex_input != 0) !=
                dynamic_features.has_dynamic_vertex_input) {
            {
                std::scoped_lock lock{known_shader_keys_mutex};
                known_shader_keys.try_emplace(ShaderSetHash(key), key);
            }
            ++feature_mismatched_pipelines;
            return;
        }

        const bool key_requests_provoking_last = key.state.provoking_vertex_last != 0;
        if (key_requests_provoking_last && !dynamic_features.has_provoking_vertex_last_mode) {
            return;
        }

        const bool key_uses_transform_feedback = key.state.xfb_enabled != 0;
        if (key_uses_transform_feedback && key_requests_provoking_last &&
            !dynamic_features.has_provoking_vertex_tf_preserve) {
            return;
        }

        // The key holds the driver, so after a driver update no cached pipeline matched a draw
        // again: each was built at boot for nothing, then built again in game and stored a second
        // time. The cache holds guest shaders that are translated again for this driver either
        // way, so an entry from another driver or version (a shader pack made on another GPU) is
        // taken as this one's.
        const u64 recorded_hash = key.Hash();
        if ((key.state.driver_id != dynamic_features.driver_id ||
             key.state.driver_version != dynamic_features.driver_version) &&
            std::getenv("EDEN_NX2_KEEP_CACHE_DRIVER_VERSION") == nullptr) {
            key.state.driver_id = dynamic_features.driver_id;
            key.state.driver_version = dynamic_features.driver_version;
            ++migrated_pipelines;
        }
        const u64 hash = key.Hash();
        if (!disk_pipelines.insert(hash).second) {
            // A later record of the pipeline, stored by a session whose build of the earlier one
            // failed for a value its record lacks (incomplete_records): it takes the earlier
            // one's place in the background builds, and is built when that one fails at boot.
            if (repair) {
                if (const auto it = skipped_index.find(hash); it != skipped_index.end()) {
                    skipped_graphics[it->second].second = std::move(envs);
                } else {
                    later_graphics.insert_or_assign(hash, std::make_pair(key, std::move(envs)));
                }
            }
            return;
        }
        {
            std::scoped_lock lock{known_shader_keys_mutex};
            known_shader_keys.try_emplace(ShaderSetHash(key), key);
        }
        NoteRecordedPipeline(key);
        const bool listed = PreloadAllPipelines() ||
                            (used_pipelines.empty() ? queued_graphics < MaxPipelinesWithoutUsedList
                                                    : used_pipelines.contains(hash) ||
                                                      used_pipelines.contains(recorded_hash));
        if (!listed && !preload_all_cached && !prepare) {
            ++skipped_pipelines;
            if (build_skipped_in_background) {
                skipped_index.emplace(hash, skipped_graphics.size());
                skipped_graphics.emplace_back(key, std::move(envs));
            }
            return;
        }
        ++queued_graphics;
        queue_graphics(key, std::move(envs), listed);
    }};
    if (reading_log) {
        const VideoCommon::LoadComputeFn read_compute{load_compute};
        const VideoCommon::LoadGraphicsFn read_graphics{load_graphics};
        size_t other_layouts = 0;
        for (const CacheV2::LogRecord& record : log_v2->live) {
            if (stop_loading.stop_requested()) {
                break;
            }
            if (record.kind != CacheV2::RecordKind::Graphics &&
                record.kind != CacheV2::RecordKind::Compute) {
                continue;
            }
            // Another build's layout: kept for it, not read
            if (!PipelineCacheFile::IsLogRecordLayout(record.layout)) {
                ++other_layouts;
                continue;
            }
            try {
                std::istringstream stream = PayloadStream(record.payload);
                stream.exceptions(std::ios::failbit | std::ios::badbit);
                VideoCommon::ReadPipelineRecord(stream, read_compute, read_graphics);
            } catch (const std::exception& e) {
                LOG_ERROR(Render_Vulkan, "Shader cache v2 record {:016x} can't be read: {}",
                          record.key, e.what());
                cache_v2->Append(CacheV2::RecordKind::Tombstone, record.key,
                                 CacheV2::TombstonePayload(record.kind));
            }
        }
        if (other_layouts != 0) {
            LOG_INFO(Render_Vulkan,
                     "{} shader cache v2 pipelines are laid out for another build and not loaded",
                     other_layouts);
        }
        // EDEN_NX2_NATIVE_PROGRAMS: the recorded native programs are lowered on the workers with
        // the pipelines, so that their first binds take them instead of lowering on the GPU thread.
        if (VideoCommon::NativePrograms::Enabled()) {
            namespace NativePrograms = VideoCommon::NativePrograms;
            std::unordered_set<u64> recorded;
            std::vector<std::pair<u64, VideoCommon::NativeProgramInputs>> programs;
            for (const CacheV2::LogRecord& record : log_v2->live) {
                if (record.kind != CacheV2::RecordKind::Program) {
                    continue;
                }
                std::optional<VideoCommon::NativeProgramInputs> inputs =
                    NativePrograms::Decode(record.layout, record.payload);
                if (!inputs) {
                    continue;
                }
                const u64 key = NativePrograms::Key(*inputs);
                if (recorded.insert(key).second) {
                    programs.emplace_back(key, std::move(*inputs));
                }
            }
            NativePrograms::Begin(*cache_v2, std::move(recorded));
            for (auto& program : programs) {
                workers.QueueWork([key = program.first, inputs = std::move(program.second)] {
                    const auto start = std::chrono::steady_clock::now();
                    std::shared_ptr<const VideoCommon::LoweredNativeCode> code;
                    try {
                        code = VideoCommon::LowerNativeProgram(inputs);
                    } catch (const std::exception& e) {
                        LOG_ERROR(Render_Vulkan,
                                  "Native program record {:016x} can't be lowered: {}", key,
                                  e.what());
                    }
                    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start);
                    NativePrograms::Add(key, std::move(code), static_cast<u64>(ns.count()));
                });
            }
        }
        // The environments hold copies of what they read: the log's bytes can go.
        log_stats = log_v2->stats;
        log_v2.reset();
    } else {
        VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, CACHE_VERSION,
                                   load_compute, load_graphics);
        reading_mesh_file = true;
        VideoCommon::LoadPipelines(stop_loading, mesh_pipeline_cache_filename, CACHE_VERSION,
                                   load_compute, load_graphics);
    }
    for (auto& [key, candidates] : compute_envs) {
        OrderComputeTranslations(candidates);
        queue_compute(key, std::move(candidates));
    }
    compute_envs.clear();

    LOG_INFO(Render_Vulkan, "Total Pipeline Count: {}", state.total);
    if (feature_mismatched_pipelines != 0) {
        LOG_WARNING(Render_Vulkan,
                    "{} cached graphics pipelines were saved with other dynamic state features "
                    "(another GPU or the extended/vertex input dynamic state settings) and are "
                    "not loaded",
                    feature_mismatched_pipelines);
    }
    if (migrated_pipelines != 0) {
        LOG_INFO(Render_Vulkan,
                 "{} cached graphics pipelines were saved by another driver or driver version and "
                 "are used for this one",
                 migrated_pipelines);
    }
    if (skipped_pipelines != 0) {
        LOG_INFO(Render_Vulkan, "{} cached graphics pipelines are built {}", skipped_pipelines,
                 build_skipped_in_background ? "in the background" : "when first needed");
    }

    std::unique_lock lock{state.mutex};
    callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
    state.has_loaded = true;
    lock.unlock();

    workers.WaitForRequests(stop_loading);
    VideoCommon::NativePrograms::ReportBoot();

    // Records from a build that read less than this one: a later record of the same pipeline is
    // built instead, and the others are stored again when the game builds them.
    if (!state.incomplete.empty() && !stop_loading.stop_requested()) {
        const size_t incomplete = state.incomplete.size();
        std::vector<u64> failed = std::exchange(state.incomplete, {});
        size_t retried = 0;
        {
            std::scoped_lock retry_lock{state.mutex};
            std::erase_if(failed, [&](u64 hash) {
                const auto it = later_graphics.find(hash);
                if (it == later_graphics.end()) {
                    return false;
                }
                queue_graphics(it->second.first, std::move(it->second.second), true);
                ++retried;
                return true;
            });
        }
        workers.WaitForRequests(stop_loading);
        failed.insert(failed.end(), state.incomplete.begin(), state.incomplete.end());
        LOG_INFO(Render_Vulkan,
                 "{} cached graphics pipelines lack values this build's translation reads: {} "
                 "built from a later record, {} stored again when the game builds them",
                 incomplete, retried - state.incomplete.size(), failed.size());
        std::scoped_lock incomplete_lock{background_mutex};
        incomplete_records.insert(failed.begin(), failed.end());
    }
    later_graphics.clear();

    // The log is compacted once a quarter of it is records no load reads.
    if (log_stats && !stop_loading.stop_requested() && log_stats->dead_bytes != 0 &&
        log_stats->dead_bytes >= log_stats->file_size / 4) {
        cache_v2->Compact();
    }

    if (prepare) {
        lock.lock();
        LOG_INFO(Render_Vulkan,
                 "Prepare shaders: {} cached pipelines built in {:.1f} s, {} of them released "
                 "after building (under {} GiB of memory free)",
                 state.built, std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                            load_start)
                                  .count(),
                 state.released, KeepPipelinesAboveAvailable >> 30);
        lock.unlock();
        RunNativePrecompile(base_dir, stop_loading);
    } else {
        StartNativePrecompileHits(base_dir);
    }

    if (preload_all_cached) {
        lock.lock();
        LOG_INFO(Render_Vulkan,
                 "All cached pipelines: {} past the used list built at boot, {} built when first "
                 "needed (under {} GiB of memory free)",
                 state.unlisted_built, state.unlisted_deferred, KeepPipelinesAboveAvailable >> 30);
        lock.unlock();
    }

    if (!skipped_graphics.empty() && !stop_loading.stop_requested()) {
        StartBackgroundPipelines(std::move(skipped_graphics));
    }

    if (use_vulkan_pipeline_cache) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
        size_t size = 0;
        vulkan_pipeline_cache.Read(&size, nullptr);
        last_cache_size.store(size, std::memory_order_relaxed);
        last_flush = std::chrono::steady_clock::now();
    }

    if (state.statistics) {
        state.statistics->Report();
    }

    if (!stop_loading.stop_requested()) {
        warm_cancel = false;
        warm_stop_callback = std::make_unique<std::stop_callback<std::function<void()>>>(
            stop_loading, std::function<void()>{[this] { warm_cancel = true; }});
        StartComputeWarm(title_id);
    }
}

namespace {
/// The warm file to load: EDEN_NX2_COMPUTE_WARM when set, else the title's own file, empty when
/// neither exists.
std::filesystem::path ComputeWarmFile(const std::filesystem::path& title_file) {
    if (const std::string_view path = Common::Nx2::Setting("EDEN_NX2_COMPUTE_WARM", "");
        !path.empty()) {
        return std::filesystem::path{std::u8string{path.begin(), path.end()}};
    }
    std::error_code ec;
    if (std::filesystem::exists(title_file, ec)) {
        return title_file;
    }
    return {};
}

/// Warm-up threads, EDEN_NX2_WARM_THREADS (1 to 32, 4 by default). They run at low priority.
size_t WarmThreads() {
    const std::string value{Common::Nx2::Setting("EDEN_NX2_WARM_THREADS", "4")};
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 1 || parsed > 32) {
        LOG_WARNING(Render_Vulkan,
                    "Nx2Perf: EDEN_NX2_WARM_THREADS={} is not a number from 1 to 32, using 4",
                    value);
        return 4;
    }
    return static_cast<size_t>(parsed);
}

/// The "already warmed" marker: warm file size and last-write time, entry count, GPU driver
/// version and device ID, pipeline cache version, and whether shared shader functions were built
/// as calls (the SPIR-V the driver compiled differs between the two). Next to the warm file, or
/// in the title's shader folder when that folder is not writable.
struct WarmStamp {
    u64 size{};
    s64 write_time{};
    u64 entries{};
    u32 driver{};
    u32 device_id{};
    u32 cache_version{};
    u32 no_inline{};
};

std::optional<WarmStamp> CurrentStamp(const std::filesystem::path& file, const Device& device,
                                      u64 entries) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto time = std::filesystem::last_write_time(file, ec);
    if (ec) {
        return std::nullopt;
    }
    return WarmStamp{size,
                     static_cast<s64>(time.time_since_epoch().count()),
                     entries,
                     device.GetDriverVersion(),
                     device.GetDeviceID(),
                     CACHE_VERSION,
                     ComputeNoInline() ? 1U : 0U};
}

std::array<std::filesystem::path, 2> MarkerPaths(const std::filesystem::path& file,
                                                 const std::filesystem::path& title_file) {
    const auto marker = VideoCommon::ShaderWarm::WarmMarkerPath(file);
    return {marker, title_file.parent_path() / marker.filename()};
}

bool WarmAlreadyDone(const std::filesystem::path& file, const std::filesystem::path& title_file,
                     const Device& device) {
    const auto stamp = CurrentStamp(file, device, 0);
    if (!stamp) {
        return false;
    }
    for (const auto& marker : MarkerPaths(file, title_file)) {
        std::ifstream in(marker);
        WarmStamp saved;
        if (in >> saved.size >> saved.write_time >> saved.entries >> saved.driver >>
            saved.device_id >> saved.cache_version >> saved.no_inline) {
            if (saved.size == stamp->size && saved.write_time == stamp->write_time &&
                saved.driver == stamp->driver && saved.device_id == stamp->device_id &&
                saved.cache_version == stamp->cache_version &&
                saved.no_inline == stamp->no_inline) {
                return true;
            }
        }
    }
    return false;
}

void WriteWarmMarker(const std::filesystem::path& file, const std::filesystem::path& title_file,
                     const Device& device, u64 entries) {
    const auto stamp = CurrentStamp(file, device, entries);
    if (!stamp) {
        return;
    }
    for (const auto& marker : MarkerPaths(file, title_file)) {
        std::error_code ec;
        std::filesystem::create_directories(marker.parent_path(), ec);
        std::ofstream out(marker, std::ios::trunc);
        out << stamp->size << ' ' << stamp->write_time << ' ' << stamp->entries << ' '
            << stamp->driver << ' ' << stamp->device_id << ' ' << stamp->cache_version << ' '
            << stamp->no_inline << '\n';
        out.close();
        if (out) {
            return;
        }
    }
}

/// Lets one caller through every 250 ms (status bar updates from the warm-up threads).
struct WarmProgressThrottle {
    std::atomic<s64> last{};

    bool Due() {
        const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        s64 previous = last.load(std::memory_order_relaxed);
        return now - previous >= 250 &&
               last.compare_exchange_strong(previous, now, std::memory_order_relaxed);
    }
};
} // Anonymous namespace

void PipelineCache::StopComputeWarm() {
    warm_cancel = true;
    compute_warm_threads.clear();
    warm_stop_callback.reset();
}

void PipelineCache::StartComputeWarm(u64 title_id) {
    if (!compute_warm_threads.empty()) {
        return;
    }
    const std::filesystem::path title_file = VideoCommon::ShaderWarm::ComputeWarmPath(title_id);
    const std::filesystem::path warm_file = ComputeWarmFile(title_file);
    if (warm_file.empty()) {
        return;
    }
    const std::string path = Common::FS::PathToUTF8String(warm_file);
    if (WarmAlreadyDone(warm_file, title_file, device)) {
        LOG_INFO(Render_Vulkan, "Nx2Perf compute_warm: already done for this file and driver");
        return;
    }
    size_t file_entries = 0;
    bool read_ok = false;
    // Its own reader: the disk cache loader deletes files it rejects.
    struct Entry {
        ComputePipelineCacheKey key;
        FileEnvironment env;
    };
    auto entries = std::make_shared<std::vector<Entry>>();
    try {
        std::ifstream file(warm_file, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            LOG_WARNING(Render_Vulkan, "Nx2Perf compute_warm: cannot open {}", path);
            return;
        }
        file.exceptions(std::ifstream::failbit);
        const auto end{file.tellg()};
        file.seekg(0, std::ios::beg);
        std::array<char, 8> magic{};
        u32 version{};
        file.read(magic.data(), magic.size())
            .read(reinterpret_cast<char*>(&version), sizeof(version));
        constexpr std::array<char, 8> expected_magic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
        if (magic != expected_magic || version != CACHE_VERSION) {
            LOG_WARNING(Render_Vulkan, "Nx2Perf compute_warm: {} is not a version {} file", path,
                        CACHE_VERSION);
            return;
        }
        while (file.tellg() != end) {
            u32 num_envs{};
            file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
            if (num_envs != 1) {
                LOG_WARNING(Render_Vulkan, "Nx2Perf compute_warm: unexpected entry in {}", path);
                break;
            }
            Entry entry;
            entry.env.Deserialize(file);
            if (entry.env.ShaderStage() != Shader::Stage::Compute) {
                LOG_WARNING(Render_Vulkan, "Nx2Perf compute_warm: non-compute entry in {}", path);
                break;
            }
            file.read(reinterpret_cast<char*>(&entry.key), sizeof(entry.key));
            ++file_entries;
            // Programs already in the disk cache were built at boot.
            if (!compute_cache.contains(entry.key)) {
                entries->push_back(std::move(entry));
            }
        }
        read_ok = file.tellg() == end;
    } catch (const std::exception& e) {
        LOG_WARNING(Render_Vulkan, "Nx2Perf compute_warm: reading {} failed: {}", path, e.what());
    }
    if (entries->empty()) {
        if (read_ok && file_entries != 0) {
            WriteWarmMarker(warm_file, title_file, device, file_entries);
        }
        return;
    }
    struct Progress {
        std::atomic<size_t> next{};
        std::atomic<size_t> built{};
        std::atomic<size_t> failed{};
        std::atomic<size_t> running{};
        WarmProgressThrottle throttle;
        std::chrono::steady_clock::time_point begin{std::chrono::steady_clock::now()};
    };
    auto progress = std::make_shared<Progress>();
    static const VideoCommon::GuessMissing guess{};
    const size_t num_threads = WarmThreads();
    progress->running = num_threads;
    shader_notify.SetWarmProgress(1, 0, entries->size());
    LOG_INFO(Render_Vulkan, "Nx2Perf compute_warm: {} programs to build on {} low priority threads",
             entries->size(), num_threads);
    for (size_t thread = 0; thread < num_threads; ++thread) {
        compute_warm_threads.emplace_back([this, entries, progress, warm_file, title_file,
                                           file_entries, read_ok](std::stop_token stop) {
            Common::SetCurrentThreadName("VkComputeWarm");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            while (!stop.stop_requested() && !warm_cancel) {
                YieldBackgroundBuild(
                    [this, &stop] { return stop.stop_requested() || warm_cancel.load(); });
                if (stop.stop_requested() || warm_cancel) {
                    break;
                }
                const size_t index = progress->next.fetch_add(1);
                if (index >= entries->size()) {
                    break;
                }
                Entry& entry = (*entries)[index];
                entry.env.SetGuessMissing(&guess);
                bool ok = false;
                background_building.fetch_add(1, std::memory_order_relaxed);
                try {
                    ShaderPools pools;
                    // Built and destroyed here: only the driver's cache keeps it.
                    ok = CreateComputePipeline(pools, entry.key, entry.env, nullptr, false) !=
                         nullptr;
                } catch (const std::exception& e) {
                    LOG_DEBUG(Render_Vulkan, "compute_warm {:016x}: {}", entry.key.unique_hash,
                              e.what());
                }
                background_building.fetch_sub(1, std::memory_order_relaxed);
                (ok ? progress->built : progress->failed).fetch_add(1);
                entry.env = FileEnvironment{};
                const size_t done = progress->built + progress->failed;
                if (progress->throttle.Due()) {
                    shader_notify.SetWarmProgress(1, done, entries->size());
                }
                if (done % 500 == 0) {
                    LOG_INFO(Render_Vulkan, "Nx2Perf compute_warm: {} of {} done", done,
                             entries->size());
                }
            }
            if (progress->running.fetch_sub(1) != 1) {
                return;
            }
            shader_notify.SetWarmProgress(0, 0, 0);
            const size_t done = progress->built + progress->failed;
            if (read_ok && !stop.stop_requested() && !warm_cancel && done >= entries->size()) {
                WriteWarmMarker(warm_file, title_file, device, file_entries);
            }
            LOG_INFO(Render_Vulkan,
                     "Nx2Perf compute_warm: finished, {} built, {} failed, {} not reached, {:.0f} s",
                     progress->built.load(), progress->failed.load(),
                     entries->size() - std::min(entries->size(), done),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                   progress->begin)
                         .count());
        });
    }
}

void PipelineCache::QueueVulkanPipelineCacheFlush() {
    if (!use_vulkan_pipeline_cache || vulkan_pipeline_cache_filename.empty() ||
        !device.CanReadPipelineCacheDuringBuilds()) {
        return;
    }
    if (++pipelines_since_flush < VULKAN_CACHE_FLUSH_PIPELINES) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto megabytes = last_cache_size.load(std::memory_order_relaxed) / (1024 * 1024);
    const std::chrono::seconds interval{
        std::max<size_t>(VULKAN_CACHE_FLUSH_MIN_SECONDS, megabytes)};
    if (last_flush.time_since_epoch().count() != 0 && now - last_flush < interval) {
        return;
    }
    if (flush_in_flight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    pipelines_since_flush = 0;
    last_flush = now;
    serialization_thread.QueueWork([this] {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
        size_t size = 0;
        vulkan_pipeline_cache.Read(&size, nullptr);
        last_cache_size.store(size, std::memory_order_relaxed);
        flush_in_flight.store(false, std::memory_order_release);
    });
}

void PipelineCache::ReportNewPipelineKey(const GraphicsPipelineCacheKey& key) {
    NoteNativePrecompileHit(key);
    // EDEN_NX2_PIPELINE_DIFF=1 also logs each state variant (up to 400).
    static const bool verbose = std::getenv("EDEN_NX2_PIPELINE_DIFF") != nullptr;
    auto& stats = new_key_stats;
    const auto now = std::chrono::steady_clock::now();
    if (stats.window_start == std::chrono::steady_clock::time_point{}) {
        stats.window_start = now;
    }
    size_t kind = 0;
    u32 fields = 0;
    u32 raw1_diff = 0;
    u32 raw2_diff = 0;
    if (disk_pipelines.contains(key.Hash())) {
        kind = 2;
    } else {
        std::scoped_lock lock{known_shader_keys_mutex};
        const auto [it, first] = known_shader_keys.try_emplace(ShaderSetHash(key), key);
        if (!first) {
            kind = 1;
            fields = KeyDifferences(key.state, it->second.state);
            raw1_diff = key.state.raw1 ^ it->second.state.raw1;
            raw2_diff = key.state.raw2 ^ it->second.state.raw2;
            // Whether the attributes that differ are ones the shaders read: if not, the key
            // could drop them and the variant would not exist.
            constexpr u32 AttributesField = 21;
            const auto earlier = graphics_cache.find(it->second);
            if ((fields >> AttributesField) & 1 && earlier != graphics_cache.end() &&
                earlier->second && !key.state.dynamic_vertex_input) {
                u32 differing = 0;
                u32 differing_types = 0;
                for (size_t index = 0; index < key.state.attributes.size(); ++index) {
                    const auto& attribute = key.state.attributes[index];
                    const auto& earlier_attribute = it->second.state.attributes[index];
                    differing |= attribute.raw != earlier_attribute.raw ? 1U << index : 0U;
                    differing_types |=
                        CastAttributeType(attribute) != CastAttributeType(earlier_attribute)
                            ? 1U << index
                            : 0U;
                }
                ++stats.attribute_variants;
                const u32 read = earlier->second->GenericAttributesRead();
                if ((differing & read) == 0) {
                    ++stats.unread_attribute_variants;
                } else if ((differing_types & read) == 0) {
                    ++stats.format_only_attribute_variants;
                }
            }
            it->second = key;
        }
    }
    ++stats.window[kind];
    ++stats.session[kind];
    for (size_t field = 0; field < NewKeyStats::NumFields; ++field) {
        if ((fields >> field) & 1) {
            ++stats.window_fields[field];
            ++stats.session_fields[field];
        }
    }
    if (verbose && kind == 1 && stats.diff_lines++ < 400) {
        LOG_INFO(Render_Vulkan,
                 "New pipeline for known shaders {:016x} differs in: {} (raw1^{:#x} raw2^{:#x})",
                 ShaderSetHash(key), DescribeKeyFields(fields, NewKeyStats::NumFields), raw1_diff,
                 raw2_diff);
    }
    if (now - stats.window_start < std::chrono::seconds{10}) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "New pipeline keys in the last {:.1f} s: {} new shaders, {} state variants of known "
             "shaders, {} cached but not built at boot; variants differ in: {}. Session: {} new "
             "shaders, {} variants ({}), {} cached; {} of {} vertex attribute variants differ "
             "only in attributes the shaders don't read; {} more only in formats, not in the "
             "types the vertex shader reads",
             std::chrono::duration<double>(now - stats.window_start).count(), stats.window[0],
             stats.window[1], stats.window[2], DescribeFieldCounts(stats.window_fields),
             stats.session[0], stats.session[1], DescribeFieldCounts(stats.session_fields),
             stats.session[2], stats.unread_attribute_variants, stats.attribute_variants,
             stats.format_only_attribute_variants);
    stats.window = {};
    stats.window_fields = {};
    stats.window_start = now;
}

// EDEN_NX2_ATTRIBUTE_ALIAS (default on, 0 off): without dynamic vertex input the key holds all
// 32 vertex attribute formats, but a pipeline is built with only those its vertex stage reads.
// A key that differs from a cached one only in attributes nobody reads gets that pipeline
// instead of a new build (Ocarina of Time: all 61 state variants of known shaders in a session
// differed in vertex attributes, each a build, and with asynchronous shaders skipped draws).
bool AttributeAliasEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_ATTRIBUTE_ALIAS");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

GraphicsPipeline* PipelineCache::FindAttributeAlias() {
    if (graphics_key.state.dynamic_vertex_input || !AttributeAliasEnabled() ||
        graphics_cache.contains(graphics_key)) {
        return nullptr;
    }
    if (const auto it = attribute_aliases.find(graphics_key); it != attribute_aliases.end()) {
        return it->second;
    }
    const auto trimmed = [](GraphicsPipelineCacheKey key, u32 read) {
        for (size_t index = 0; index < key.state.attributes.size(); ++index) {
            if (((read >> index) & 1) == 0) {
                key.state.attributes[index].raw = 0;
            }
        }
        return key;
    };
    for (const auto& [key, pipeline] : graphics_cache) {
        if (!pipeline || key.unique_hashes != graphics_key.unique_hashes) {
            continue;
        }
        const u32 read = pipeline->GenericAttributesRead();
        if (trimmed(key, read) != trimmed(graphics_key, read)) {
            continue;
        }
        attribute_aliases.emplace(graphics_key, pipeline.get());
        ++attribute_alias_count;
        const auto now = std::chrono::steady_clock::now();
        if (now - attribute_alias_logged >= std::chrono::seconds{10}) {
            attribute_alias_logged = now;
            LOG_INFO(Render_Vulkan,
                     "Pipeline keys that differ only in unread vertex attributes, given a cached "
                     "pipeline instead of a new build: {} this session",
                     attribute_alias_count);
        }
        return pipeline.get();
    }
    return nullptr;
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {
    if (GraphicsPipeline* const alias = FindAttributeAlias()) {
        return MakeCurrent(alias);
    }
    if (async_pipelines != 0) {
        return CurrentGraphicsPipelineAsync();
    }
    const auto [pair, is_new]{graphics_cache.try_emplace(graphics_key)};
    auto& pipeline{pair->second};
    if (is_new) {
        ReportNewPipelineKey(graphics_key);
        pipeline = TakeBackgroundPipeline(graphics_key);
        if (!pipeline) {
            pipeline = CreateGraphicsPipeline();
        }
    }
    if (!pipeline) {
        return nullptr;
    }
    return MakeCurrent(pipeline.get());
}

GraphicsPipeline* PipelineCache::MakeCurrent(GraphicsPipeline* pipeline) {
    if (used_this_session.insert(pipeline).second &&
        std::chrono::steady_clock::now() - used_pipelines_saved > std::chrono::seconds{15}) {
        SaveUsedPipelines(true);
    }
    if (current_pipeline) {
        // Under the key looked up: an attribute alias has a key of its own, and kept under that
        // one, Next missed it on every draw and the list grew by one each time.
        current_pipeline->AddTransition(graphics_key, pipeline);
    }
    current_pipeline = pipeline;
    current_pipeline->MarkUsed(frame_count);
    return BuiltPipeline(current_pipeline);
}

void PipelineCache::TickFrame() {
    ++frame_count;
    if (device.IsExtGraphicsPipelineLibrarySupported()) {
        graphics_libraries.Report();
    }
    while (!evicted_pipelines.empty() && scheduler.IsFree(evicted_pipelines.front().first)) {
        evicted_pipelines.pop_front();
    }
    while (!retired_compute_pipelines.empty() &&
           scheduler.IsFree(retired_compute_pipelines.front().first) &&
           retired_compute_pipelines.front().second->IsBuildTaskDone()) {
        retired_compute_pipelines.pop_front();
    }
    constexpr u64 EvictCheckFrames = 600;
    if (frame_count % EvictCheckFrames == 0) {
        EvictUnusedPipelines();
        std::erase_if(cleared_frames, [this](const auto& entry) {
            return entry.second.last + EvictCheckFrames < frame_count;
        });
    }
}

namespace {
// EDEN_NX2_SKIP_GUARD: "1" skips only draws whose targets are all cleared every frame, "depth"
// also depth-tested draws whose depth target is (scene geometry over a color target the sky
// covers instead of a clear), "0" any draw. Mario Kart World defaults to "depth": its color
// targets are never cleared whole, so every new pipeline mid-race held its draw for the whole
// compile on the scheduler thread (up to 834 ms, a present held 998 ms), against none in
// 0.1.7-experimental.3, which had no guard.
char SkipGuardMode() {
    static Common::Nx2::SessionValue<char> mode{[] {
        const std::string_view value = Common::Nx2::Setting("EDEN_NX2_SKIP_GUARD", "1");
        return value == "0" ? '0' : value == "depth" ? 'd' : '1';
    }};
    return mode.Get();
}

bool SkipGuardEnabled() {
    return SkipGuardMode() != '0';
}
} // Anonymous namespace

void PipelineCache::NoteClear(u64 color_address, u64 depth_address) {
    if (!SkipGuardEnabled()) {
        return;
    }
    for (const u64 address : {color_address, depth_address}) {
        if (address == 0) {
            continue;
        }
        ClearFrames& frames = cleared_frames[address];
        if (frames.last != frame_count) {
            frames.previous = frames.last;
            frames.last = frame_count;
        }
    }
}

bool PipelineCache::MaySkipNativeDraw(const GraphicsPipeline* pipeline) const {
    if (!SkipGuardEnabled()) {
        return true;
    }
    const auto redrawn = [this] {
        const auto& regs = maxwell3d->regs;
        const u64 color = regs.rt_control.count > 0 &&
                                  regs.rt[0].format != Tegra::RenderTargetFormat::NONE
                              ? regs.rt[0].Address()
                              : 0;
        const u64 depth = regs.zeta_enable != 0 ? regs.zeta.Address() : 0;
        if (color == 0 && depth == 0) {
            return false;
        }
        // Every attachment the draw has: a depth-only clear says nothing about the color target.
        const auto every_frame = [this](u64 address) {
            if (address == 0) {
                return true;
            }
            const auto it = cleared_frames.find(address);
            return it != cleared_frames.end() && it->second.last == frame_count &&
                   it->second.previous != 0 && it->second.previous + 2 >= frame_count;
        };
        if (depth != 0 && regs.depth_test_enable != 0 && SkipGuardMode() == 'd' &&
            every_frame(depth)) {
            return true;
        }
        return every_frame(color) && every_frame(depth);
    };
    if ((pipeline == nullptr || (!pipeline->WritesStorage() && !pipeline->UsesTransformFeedback())) &&
        redrawn()) {
        return true;
    }
    ++skip_guard_held;
    const auto now = std::chrono::steady_clock::now();
    if (now - skip_guard_logged >= std::chrono::seconds{10}) {
        skip_guard_logged = now;
        LOG_INFO(Render_Vulkan,
                 "Nx2Perf skip_guard: {} draws waited for their pipeline instead of being skipped "
                 "(target not redrawn every frame, or storage writes)",
                 skip_guard_held);
        skip_guard_held = 0;
    }
    return false;
}

void PipelineCache::EvictUnusedPipelines() {
    // Each built pipeline holds ~0.45 MB of driver memory and none was ever destroyed, so it grew
    // with every place a session went (title 1: 1,500 on its title screen alone). One unused for
    // this long is destroyed; it takes 2-3 ms to build again from the driver's pipeline cache.
    constexpr u64 EvictAfterFrames = 9000;
    if (frame_count <= EvictAfterFrames) {
        return;
    }
    // A Switch 2 pipeline built again also translates its shader again on the GPU thread, a
    // hitch when the player comes back to a place. Keep them while Windows has room for more
    // memory (SmileyTatsu's fork keeps them always), and every title's with pipeline_preload=All,
    // which built them at boot to avoid that. EDEN_NX2_EVICT_PIPELINES=1 always evicts.
    static const bool always_evict = [] {
        const char* value = std::getenv("EDEN_NX2_EVICT_PIPELINES");
        return value != nullptr && value[0] == '1';
    }();
    const bool memory_allows = MemoryAllowsMorePipelines();
    if (!always_evict && (Settings::CurrentProgramIsSwitch2() || PreloadAllCachedPipelines()) &&
        memory_allows) {
        return;
    }
    if (const size_t dropped = DropBackgroundPipelines(); dropped != 0) {
        LOG_INFO(Render_Vulkan, "Dropped {} graphics pipelines built in the background, never used",
                 dropped);
    }
    if (!memory_allows) {
        if (const size_t dropped = graphics_libraries.DropUnused(); dropped != 0) {
            LOG_INFO(Render_Vulkan, "Dropped {} graphics pipeline libraries no pipeline uses",
                     dropped);
        }
    }
    // While memory allows (Switch 1 titles; the Switch 2 ones returned above), only a cache over
    // EDEN_NX2_PIPELINE_EVICT_ABOVE (8,000, ~3.6 GB of driver memory) is trimmed, and only down
    // to it, least recently used first: each one evicted is translated again on the GPU thread
    // when its place is revisited (Fritchii's Ocarina of Time evicted 3,400 in 25 minutes and
    // rebuilt 1,850, 14 s of stalls). Under memory pressure, or with EDEN_NX2_EVICT_PIPELINES=1,
    // every stale one goes, as before.
    static const size_t evict_above = [] {
        const char* value = std::getenv("EDEN_NX2_PIPELINE_EVICT_ABOVE");
        return value != nullptr ? static_cast<size_t>(std::strtoull(value, nullptr, 10))
                                : size_t{8000};
    }();
    const size_t limit = memory_allows && !always_evict ? evict_above : 0;
    if (graphics_cache.size() <= limit) {
        return;
    }
    const u64 cutoff = frame_count - EvictAfterFrames;
    // ~0.9 GB of driver memory at ~0.45 MB each.
    constexpr size_t MaxUnusedPrebuiltPipelines = 2000;
    size_t kept_prebuilt = 0;
    std::vector<std::pair<u64, GraphicsPipelineCacheKey>> candidates;
    for (const auto& [key, pipeline] : graphics_cache) {
        if (pipeline == nullptr || pipeline.get() == current_pipeline || !pipeline->IsBuilt() ||
            pipeline->LastUse() >= cutoff) {
            continue;
        }
        // A pipeline built from the disk cache at boot that no draw has used yet stays, up to a
        // budget: evicting it throws the prebuild away, and the game translates it again on the
        // GPU thread when it reaches that content (OoT: 1174 of 1346 evicted on the title screen,
        // 684 rebuilt in 10 s on entering the field). Past the budget they go like the rest, so a
        // large cache can't hold their driver memory for the whole session.
        if (!pipeline->WasUsed() && kept_prebuilt < MaxUnusedPrebuiltPipelines) {
            ++kept_prebuilt;
            continue;
        }
        candidates.emplace_back(pipeline->LastUse(), key);
    }
    const size_t evicted = (std::min)(candidates.size(), graphics_cache.size() - limit);
    if (evicted == 0) {
        return;
    }
    if (evicted < candidates.size()) {
        std::nth_element(candidates.begin(),
                         candidates.begin() + static_cast<std::ptrdiff_t>(evicted),
                         candidates.end(),
                         [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    }
    const u64 tick = scheduler.CurrentTick();
    for (size_t i = 0; i < evicted; ++i) {
        const auto it = graphics_cache.find(candidates[i].second);
        if (used_this_session.erase(it->second.get()) != 0) {
            used_and_evicted.insert(it->first.Hash());
        }
        evicted_pointer_slots.Remember(it->first.Hash(), it->second->MappedPointers());
        evicted_pipelines.emplace_back(tick, std::move(it->second));
        graphics_cache.erase(it);
    }
    attribute_aliases.clear();
    for (auto& [key, pipeline] : graphics_cache) {
        if (pipeline) {
            pipeline->ClearTransitions();
        }
    }
    LOG_INFO(Render_Vulkan, "Evicted {} graphics pipelines unused for {} frames, {} left", evicted,
             EvictAfterFrames, graphics_cache.size());
}

void PipelineCache::StartBackgroundPipelines(
    std::vector<std::pair<GraphicsPipelineCacheKey, std::vector<FileEnvironment>>> pipelines) {
    StopBackgroundPipelines();
    {
        std::scoped_lock lock{background_mutex};
        background_ready.clear();
        background_claimed.clear();
        background_built = 0;
        background_taken = 0;
        background_left_out = 0;
    }
    const size_t threads = BackgroundPipelineThreads();
    LOG_INFO(Render_Vulkan, "Building {} cached graphics pipelines in the background ({} threads)",
             pipelines.size(), threads);
    background_stop.store(false, std::memory_order_relaxed);
    background_low_memory.store(false, std::memory_order_relaxed);
    background_left.store(pipelines.size(), std::memory_order_relaxed);
    background_start = std::chrono::steady_clock::now();
    background_active.store(true, std::memory_order_release);
    background_workers.reset(new Common::ThreadWorker(threads, "VkPipelinePrebuild", {},
                                                      Common::ThreadPlacement::Background));
    // Newest first: the cache file appends, so its end holds what the latest sessions found.
    for (auto& pending : std::views::reverse(pipelines)) {
        background_workers->QueueWork([this, key = pending.first,
                                       envs = std::move(pending.second)]() mutable {
            // After the builds the game waits for, which may claim this one meanwhile
            YieldBackgroundBuild(
                [this] { return background_stop.load(std::memory_order_relaxed); });
            bool build = !background_stop.load(std::memory_order_relaxed);
            if (build) {
                std::scoped_lock lock{background_mutex};
                build = !background_claimed.contains(key);
            }
            // As pipeline_preload=All: once memory is low, the rest wait until a draw needs them.
            bool left_out = false;
            if (build && (background_low_memory.load(std::memory_order_relaxed) ||
                          !MemoryAllowsMorePipelines())) {
                background_low_memory.store(true, std::memory_order_relaxed);
                build = false;
                left_out = true;
            }
            std::unique_ptr<GraphicsPipeline> pipeline;
            if (build) {
                background_building.fetch_add(1, std::memory_order_relaxed);
                SCOPE_EXIT {
                    background_building.fetch_sub(1, std::memory_order_relaxed);
                };
                ShaderPools pools;
                boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram>
                    env_ptrs;
                for (auto& env : envs) {
                    env_ptrs.push_back(&env);
                }
                pipeline = CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), nullptr, false);
                if (pipeline) {
                    pipeline->SetMappedPointers(MappedPointerSlots(key, envs, true));
                }
            }
            const bool incomplete = build && !pipeline && cache_repair &&
                                    std::ranges::any_of(envs, &FileEnvironment::LacksRecordedValue);
            std::unique_lock lock{background_mutex};
            if (incomplete) {
                incomplete_records.insert(key.Hash());
            }
            if (pipeline && !background_claimed.contains(key)) {
                background_ready.emplace(key, std::move(pipeline));
                ++background_built;
            }
            if (left_out) {
                ++background_left_out;
            }
            if (background_left.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                LOG_INFO(Render_Vulkan,
                         "Background pipelines done in {:.1f} s: {} built, {} of them taken by "
                         "draws so far; {} built by the GPU thread first, {} left out (under {} "
                         "GiB of memory free)",
                         std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                       background_start)
                             .count(),
                         background_built, background_taken, background_claimed.size(),
                         background_left_out, KeepPipelinesAboveAvailable >> 30);
                background_claimed.clear();
                background_active.store(false, std::memory_order_release);
            }
            lock.unlock();
            // One dropped here was never bound, so no GPU work can still use it.
            pipeline.reset();
        });
    }
}

std::unique_ptr<GraphicsPipeline> PipelineCache::TakeBackgroundPipeline(
    const GraphicsPipelineCacheKey& key) {
    std::scoped_lock lock{background_mutex};
    if (const auto it = background_ready.find(key); it != background_ready.end()) {
        auto pipeline = std::move(it->second);
        background_ready.erase(it);
        ++background_taken;
        return pipeline;
    }
    // Built here instead: a background build of it is skipped, or dropped when it finishes.
    if (background_active.load(std::memory_order_acquire)) {
        background_claimed.insert(key);
    }
    return nullptr;
}

size_t PipelineCache::DropBackgroundPipelines() {
    decltype(background_ready) dropped;
    {
        std::scoped_lock lock{background_mutex};
        dropped.swap(background_ready);
    }
    return dropped.size();
}

void PipelineCache::StopBackgroundPipelines() {
    background_stop.store(true, std::memory_order_relaxed);
    // Joins the threads: a build in progress finishes, the queued ones are dropped.
    background_workers.reset();
}

void PipelineCache::YieldBackgroundBuild(const std::function<bool()>& stop) {
    if (!build_priority) {
        return;
    }
    const auto waited = VideoCommon::BuildPriority::YieldToForeground(workers, stop);
    if (waited == std::chrono::steady_clock::duration::zero()) {
        return;
    }
    async_stats.background_yields.fetch_add(1, std::memory_order_relaxed);
    async_stats.background_yield_ns.fetch_add(
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count()),
        std::memory_order_relaxed);
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) const noexcept {
    if (pipeline->IsBuilt()) {
        return pipeline;
    }
    if (!use_asynchronous_shaders) {
        return pipeline;
    }
    // Native draws can fill targets that persist across frames, so a skipped one leaves missing
    // or stale results. Wait for the build (fully, or within a budget so first-seen bursts do
    // not stall), and only then fall back to skipping.
    if (maxwell3d->GetNativeMacroEngine().native_turing) {
        const NativeSyncMode mode = NativeSyncPipelines();
        if (mode == NativeSyncMode::Full) {
            return pipeline;
        }
        if (mode == NativeSyncMode::SkipScene) {
            // Selevet's rule: depth-tested draws, or ones larger than a full screen quad, are
            // scene geometry the next frames draw again. Small ones without depth often build a
            // texture once, so they wait. The count is the draw's own; an indirect one waits.
            if ((maxwell3d->regs.zeta_enable ||
                 maxwell3d->draw_manager.draw_state.draw_element_count > 6) &&
                MaySkipNativeDraw(pipeline)) {
                last_pipeline_building = true;
                return nullptr;
            }
            return pipeline;
        }
        if (mode == NativeSyncMode::Budget) {
            // A draw the skip guard holds is never skipped: the scheduler thread waits for its
            // build in any case. Waiting here as well (by default up to 3 ms a draw, 8 ms a
            // frame) only held up the draws after it, whose new pipelines could have started
            // compiling meanwhile.
            if (!MaySkipNativeDraw(pipeline)) {
                return pipeline;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - native_wait_window > NativeSyncWindow()) {
                native_wait_window = now;
                native_wait_spent = {};
            }
            const auto wait =
                std::min(NativeSyncDrawBudget(), NativeSyncWindowBudget() - native_wait_spent);
            if (wait.count() > 0) {
                const bool built = pipeline->WaitForBuild(wait);
                native_wait_spent += std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - now);
                if (built) {
                    return pipeline;
                }
            }
        }
    }
    if (maxwell3d->GetNativeMacroEngine().native_turing && !MaySkipNativeDraw(pipeline)) {
        return pipeline;
    }
    // If something is using depth, we can assume that games are not rendering anything which
    // will be used one time.
    if (maxwell3d->regs.zeta_enable) {
        last_pipeline_building = true;
        return nullptr;
    }
    // If games are using a small index count, we can assume these are full screen quads.
    // Usually these shaders are only used once for building textures so we can assume they
    // can't be built async
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    if (draw_state.index_buffer.count <= 6 || draw_state.vertex_buffer.count <= 6) {
        return pipeline;
    }
    last_pipeline_building = true;
    return nullptr;
}

// EDEN_NX2_PIPELINE_ASYNC: a build off the GPU thread reads its environments through the snapshot,
// and they say whether a read missed it (the result is then thrown away).
struct PipelineCache::AsyncBuildContext {
    const GraphicsEnvironments* environments{};

    [[nodiscard]] bool Missed() const {
        return std::ranges::any_of(environments->envs,
                                   [](const VideoCommon::GraphicsEnvironment& env) {
                                       return env.AsyncSnapshotMissed();
                                   });
    }
};

struct PipelineCache::AsyncGraphicsJob {
    enum : u8 { Queued, Running, Done };

    GraphicsPipelineCacheKey key{};
    GraphicsEnvironments environments;
    AsyncBuildContext context;
    std::atomic<u8> state{Queued}; ///< Queued -> Running by whoever builds it (worker or GPU thread)
    std::mutex mutex;
    std::condition_variable done;
    std::unique_ptr<GraphicsPipeline> pipeline; ///< Set when Done; null if the build failed
    bool fallback{}; ///< Set when Done: build it again on the GPU thread
    /// Pointer slots kept from an evicted pipeline of this key, kept again for a fallback build
    VideoCommon::EvictedPointerSlots::Slots kept_pointers;
};

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
    bool build_in_parallel, std::shared_ptr<ShaderPools> emit_pools,
    const AsyncBuildContext* async, bool reuse_shaders, bool translate_at_once,
    bool stand_in_vertex) try {
    auto hash = key.Hash();
    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);
    // EDEN_NX2_GPL: run-time builds are linked from shader libraries. Boot and background builds
    // stay whole: the driver's cache serves those in milliseconds, with less memory.
    GraphicsPipelineLibraryParams library_params{
        .library_cache = reuse_shaders && device.IsExtGraphicsPipelineLibrarySupported()
                             ? &graphics_libraries
                             : nullptr,
        .spirv_hashes = {},
        .optimize_worker = &optimize_workers,
    };
    const bool hash_spirv{library_params.library_cache != nullptr};
    last_build_reused = false;
    const ShaderTranslationKey translation_key{MakeShaderTranslationKey(key, device)};
    const bool reuse{use_shader_reuse && reuse_shaders};
    // A build off the GPU thread reads a snapshot, which may lack what an earlier translation
    // read: only builds from live environments look for shaders to reuse.
    if (reuse && async == nullptr) {
        if (std::shared_ptr<const ReusableShaders> shaders{
                shader_reuse.Find(translation_key, envs)}) {
            last_build_reused = true;
            if (Settings::values.dump_guest_shaders) {
                size_t dump_index{0};
                for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
                    if (key.unique_hashes[index] != 0) {
                        envs[dump_index++]->Dump(hash, key.unique_hashes[index]);
                    }
                }
            }
            if (emit_pools) {
                std::scoped_lock lock{spare_pools_mutex};
                if (spare_pools.size() < 4) {
                    spare_pools.push_back(std::move(emit_pools));
                }
            }
            // Only the modules are left to make, which the build does as for deferred SPIR-V.
            auto make_modules = [this, shaders, hashes = key.unique_hashes, hash_spirv](
                                    std::array<u128, Maxwell::MaxShaderStage>& spirv_hashes) {
                std::array<vk::ShaderModule, Maxwell::MaxShaderStage> stage_modules;
                for (size_t stage = 0; stage < stage_modules.size(); ++stage) {
                    if (!shaders->spirv[stage].empty()) {
                        stage_modules[stage] = BuildShader(
                            device, ModuleCode(shaders->spirv[stage], GraphicsNoInline(),
                                               "Graphics", hashes[stage + 1]));
                        if (hash_spirv) {
                            spirv_hashes[stage] = Common::CityHash128(
                                reinterpret_cast<const char*>(shaders->spirv[stage].data()),
                                shaders->spirv[stage].size() * sizeof(u32));
                        }
                    }
                }
                return stage_modules;
            };
            Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
            return std::make_unique<GraphicsPipeline>(
                scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify,
                device, descriptor_pool, guest_descriptor_queue, descriptor_buffer_ring,
                thread_worker, statistics, render_pass_cache, key,
                std::array<vk::ShaderModule, Maxwell::MaxShaderStage>{}, shaders->InfoPointers(),
                std::move(make_modules), library_params);
        }
    }
    // Declared before the programs, whose instructions they hold
    std::vector<std::shared_ptr<ShaderPools>> stage_pools;
    SCOPE_EXIT {
        for (std::shared_ptr<ShaderPools>& stage : stage_pools) {
            ReturnSparePools(std::move(stage));
        }
    };
    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    // Layer passthrough generation for devices without VK_EXT_shader_viewport_index_layer
    Shader::IR::Program* layer_source_program{};

    // With reuse on, translation asks the environments through recorders: their answers tell
    // which later pipelines these shaders fit.
    boost::container::static_vector<RecordingEnvironment, Maxwell::MaxShaderProgram> recorders;
    boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> translate_envs;
    for (Shader::Environment* const env : envs) {
        translate_envs.push_back(reuse ? &recorders.emplace_back(*env) : env);
    }

    // EDEN_NX2_PARALLEL_STAGES: the programs translate at once on the helpers while this thread
    // translates one of them and waits. Their environments read guest memory without flushing it,
    // as on this thread, and nothing else uses them meanwhile.
    std::shared_ptr<StageBatch> batch;
    std::array<StageTranslation*, Maxwell::MaxShaderProgram> translated{};
    if (translate_at_once && stage_helpers && async == nullptr &&
        std::ranges::count_if(key.unique_hashes, [](u64 stage_hash) { return stage_hash != 0; }) >=
            2) {
        batch = std::make_shared<StageBatch>();
        size_t stage_env{0};
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            Shader::Environment& env{*translate_envs[stage_env++]};
            StageTranslation& stage{batch->stages[batch->count++]};
            stage.env = &env;
            stage.pools = TakeSparePools();
            stage.cfg_offset = static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader));
            stage.exits_to_dispatcher = index == 0;
            translated[index] = &stage;
        }
        TranslateStagesAtOnce(*stage_helpers, batch, host_info);
        for (size_t stage = 0; stage < batch->count; ++stage) {
            stage_pools.push_back(std::move(batch->stages[stage].pools));
        }
    }

    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            programs[index] = GenerateGeometryPassthrough(pools.inst, pools.block, host_info,
                                                          *layer_source_program,
                                                          translation_key.passthrough_topology);
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*translate_envs[env_index]};
        ++env_index;

        const auto translate = [&] {
            if (StageTranslation* const stage = translated[index]) {
                if (stage->exception) {
                    std::rethrow_exception(stage->exception);
                }
                return std::move(*stage->program);
            }
            const u32 cfg_offset{
                static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
            return TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        };
        if (!uses_vertex_a || index != 1) {
            // Normal path
            programs[index] = translate();
        } else {
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{translate()};
            programs[index] = MergeDualVertexPrograms(program_va, program_vb, env, host_info);
        }
        if (async != nullptr && async->Missed()) {
            return nullptr; // Translated from a guess: the GPU thread builds it again
        }

        if (const auto edits = Shader::Optimization::ApplyOotVisualWorkaround(
                programs[index], Settings::GetCurrentProgramID(), key.unique_hashes[index],
                OotVisualWorkaround(), OotVisualScales())) {
            LOG_WARNING(Render_Vulkan, "TEMPORARY OoT visual workaround: shader {:016x}, {} scalar edits",
                        key.unique_hashes[index], *edits);
        }

        if (device.GetDriverID() != VK_DRIVER_ID_NVIDIA_PROPRIETARY && Shader::Optimization::HasBrokenPattern(programs[index])) {
            LOG_WARNING(Render_Vulkan, "0x{:016x}: Skipping shader with known issues", key.unique_hashes[index]);
            return nullptr;
        }

        if (Settings::values.dump_guest_shaders) {
            env.Dump(hash, key.unique_hashes[index]);
        }

        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }
    // A tessellation pipeline the driver would refuse is skipped like a missing mesh pipeline:
    // the draw is dropped instead of failing the build with VK_ERROR_INITIALIZATION_FAILED.
    {
        const Shader::IR::Program* control{};
        const Shader::IR::Program* evaluation{};
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            if (programs[index].stage == Shader::Stage::TessellationControl) {
                control = &programs[index];
            } else if (programs[index].stage == Shader::Stage::TessellationEval) {
                evaluation = &programs[index];
            }
        }
        if (control != nullptr || evaluation != nullptr) {
            const u32 max_patch_size = device.GetMaxTessellationPatchSize();
            const char* reason = nullptr;
            if (!device.IsTessellationSupported()) {
                reason = "the device has no tessellation shaders";
            } else if (control == nullptr || evaluation == nullptr) {
                reason = "it has only one of the control and evaluation stages";
            } else if (control->invocations == 0 || control->invocations > max_patch_size) {
                reason = "the control shader's output vertex count is out of range";
            }
            if (reason != nullptr) {
                LOG_WARNING(Render_Vulkan,
                            "Skipping tessellation pipeline: {} (tcs={} tes={}, tcs output "
                            "vertices {}, patch control points {}, device max patch size {})",
                            reason, control != nullptr, evaluation != nullptr,
                            control != nullptr ? control->invocations : 0,
                            key.state.patch_control_points_minus_one.Value() + 1, max_patch_size);
                return nullptr;
            }
        }
    }
    std::shared_ptr<ReusableShaders> reusable;
    if (reuse) {
        reusable = std::make_shared<ReusableShaders>();
        for (RecordingEnvironment& recorder : recorders) {
            reusable->reads.push_back(recorder.TakeReads());
        }
    }
    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
    std::array<vk::ShaderModule, Maxwell::MaxShaderStage> modules;
    std::array<std::optional<Shader::RuntimeInfo>, Maxwell::MaxShaderStage> deferred_runtime_infos;
    std::array<std::vector<u32>, Maxwell::MaxShaderStage> reference_spirv;

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
            continue;
        }
        UNIMPLEMENTED_IF(index == 0);

        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        const bool stand_in{stand_in_vertex && program.stage == Shader::Stage::Fragment};
        const auto runtime_info{MakeRuntimeInfo(programs, translation_key, program,
                                                stand_in ? nullptr : previous_stage, device)};
        ConvertLegacyToGeneric(program, runtime_info);
        if (index == static_cast<size_t>(Maxwell::ShaderType::Geometry)) {
            PruneUnreadGeometryOutputs(program,
                                       programs[static_cast<size_t>(Maxwell::ShaderType::Pixel)],
                                       translation_key.xfb_enabled,
                                       device.GetMaxGeometryOutputComponents());
        }
        if (emit_pools) {
            deferred_runtime_infos[stage_index] = runtime_info;
            if (VideoCommon::Nx2Perf::Verify()) {
                reference_spirv[stage_index] = EmitSPIRV(profile, runtime_info, program, binding);
            }
        } else {
            modules[stage_index] = EmitShaderModule(
                device, profile, runtime_info, program, binding, key.unique_hashes[index],
                stage_index, {}, reusable ? &reusable->spirv[stage_index] : nullptr,
                hash_spirv ? &library_params.spirv_hashes[stage_index] : nullptr);
        }
        previous_stage = &program;
    }
    // The stage infos as the pipeline takes them below.
    if (reusable) {
        for (size_t stage = 0; stage < infos.size(); ++stage) {
            if (infos[stage]) {
                reusable->infos[stage] = *infos[stage];
            }
        }
    }
    GraphicsPipeline::EmitStagesFunc emit_stages;
    if (emit_pools) {
        // The build emits the SPIR-V from the programs, which move to it with their pools. The
        // pipeline takes its stage infos before that.
        auto job = std::make_shared<DeferredStages>(pending_emissions);
        job->pools = emit_pools;
        job->stage_pools = std::move(stage_pools);
        stage_pools.clear();
        job->programs = std::move(programs);
        job->runtime_infos = std::move(deferred_runtime_infos);
        job->reference_spirv = std::move(reference_spirv);
        for (size_t stage = 0; stage < infos.size(); ++stage) {
            if (infos[stage]) {
                infos[stage] = &job->programs[stage + 1].info;
            }
        }
        emit_stages = [this, job = std::move(job), hashes = key.unique_hashes,
                       reusable = std::move(reusable), translation_key, hash_spirv](
                          std::array<u128, Maxwell::MaxShaderStage>& spirv_hashes) mutable {
            std::array<vk::ShaderModule, Maxwell::MaxShaderStage> stage_modules;
            Shader::Backend::Bindings stage_binding;
            for (size_t stage = 0; stage < stage_modules.size(); ++stage) {
                if (job->runtime_infos[stage]) {
                    stage_modules[stage] =
                        EmitShaderModule(device, profile, *job->runtime_infos[stage],
                                         job->programs[stage + 1], stage_binding,
                                         hashes[stage + 1], stage, job->reference_spirv[stage],
                                         reusable ? &reusable->spirv[stage] : nullptr,
                                         hash_spirv ? &spirv_hashes[stage] : nullptr);
                }
            }
            if (reusable) {
                shader_reuse.Add(translation_key, std::move(reusable));
            }
            std::shared_ptr<ShaderPools> pools = std::move(job->pools);
            std::vector<std::shared_ptr<ShaderPools>> stages = std::move(job->stage_pools);
            job.reset();
            for (std::shared_ptr<ShaderPools>& stage : stages) {
                ReturnSparePools(std::move(stage));
            }
            pools->ReleaseContents();
            std::scoped_lock lock{spare_pools_mutex};
            if (spare_pools.size() < 4) {
                spare_pools.push_back(std::move(pools));
            }
            return stage_modules;
        };
    } else if (reusable) {
        shader_reuse.Add(translation_key, std::move(reusable));
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<GraphicsPipeline>(
        scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify, device,
        descriptor_pool, guest_descriptor_queue, descriptor_buffer_ring, thread_worker, statistics,
        render_pass_cache, key, std::move(modules), infos, std::move(emit_stages), library_params);

} catch (const Shader::Exception& exception) {
    if (async != nullptr && async->Missed()) {
        return nullptr; // A read the snapshot lacked; the GPU thread's build reports real errors
    }
    auto hash = key.Hash();
    size_t env_index{0};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        try {
            // Walks the program for the dump; the one that failed may fail here again, and an
            // exception leaving this handler would end the process.
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        } catch (const Shader::Exception&) {
        }
        env.Dump(hash, key.unique_hashes[index]);
    }
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
} catch (const std::exception& exception) {
    if (async != nullptr && async->Missed()) {
        return nullptr;
    }
    // Anything else (a driver error creating a module, out of memory, a damaged cached program)
    // ended the process when it left here, often in a cutscene that brings new shaders. The
    // pipeline's draws are skipped instead.
    LOG_CRITICAL(Render_Vulkan, "Graphics pipeline {:#016x} failed, its draws are skipped: {}",
                 key.Hash(), exception.what());
    return nullptr;
}

namespace {
enum class RuntimeBuildOrigin {
    New,
    DiskCache, ///< In the disk cache, not built at boot
    Mesh,      ///< Task/mesh, not in the disk cache yet
};

// Pipelines built on the GPU thread at run time (shader translation; the driver compile is on
// workers): every one over 2 ms is logged, and the total every 10 s, to tell how much of the
// stutter on new content is translation.
void ReportRuntimePipeline(const char* kind, std::chrono::steady_clock::time_point begin,
                           RuntimeBuildOrigin origin, u32& draws_skipped, bool reused,
                           std::chrono::steady_clock::duration emit_time) {
    static std::chrono::steady_clock::duration total{}, disk_total{}, emit_total{};
    static u32 count{}, logged{}, disk_count{}, mesh_count{}, reused_count{};
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - begin;
    total += elapsed;
    ++count;
    emit_total += emit_time;
    if (reused) {
        ++reused_count;
    }
    if (origin == RuntimeBuildOrigin::DiskCache) {
        disk_total += elapsed;
        ++disk_count;
    } else if (origin == RuntimeBuildOrigin::Mesh) {
        ++mesh_count;
    }
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    if (elapsed > std::chrono::milliseconds{2} && logged < 400) {
        ++logged;
        LOG_INFO(Render_Vulkan, "{} pipeline built on the GPU thread in {:.1f} ms", kind, ms(elapsed));
    }
    if (now - window > std::chrono::seconds{10}) {
        // The first build after 10 s logs it, so it can cover more: the end says how long, the
        // start stays as the tools read it.
        LOG_INFO(Render_Vulkan,
                 "Runtime pipelines in the last 10 s: {} built, {:.1f} ms on the GPU thread; {} "
                 "from the disk cache ({:.1f} ms), {} task/mesh, {} draws skipped while building; "
                 "{} with reused shaders, {:.1f} ms of it emitting SPIR-V and modules; over {:.1f} s",
                 count, ms(total), disk_count, ms(disk_total), mesh_count, draws_skipped,
                 reused_count, ms(emit_total),
                 std::chrono::duration<double>(now - window).count());
        total = {};
        disk_total = {};
        emit_total = {};
        reused_count = 0;
        count = 0;
        disk_count = 0;
        mesh_count = 0;
        draws_skipped = 0;
        window = now;
    }
}
} // Anonymous namespace

void PipelineCache::KeepEvictedPointers(GraphicsEnvironments& environments,
                                        const VideoCommon::EvictedPointerSlots::Slots& slots) {
    static std::atomic<u64> kept_count{};
    for (size_t program = 0; program < Maxwell::MaxShaderProgram; ++program) {
        if (graphics_key.unique_hashes[program] == 0) {
            continue;
        }
        environments.envs[program].KeepStoragePointers(slots[program]);
        const size_t stage = program == 0 ? 0 : program - 1;
        const auto& cbufs = maxwell3d->state.shader_stages[stage].const_buffers;
        for (const u64 slot : slots[program]) {
            // Pointer-table entries (the high bit) aren't checked.
            if ((slot >> 63) != 0) {
                continue;
            }
            const u32 index = static_cast<u32>(slot >> 32);
            const u32 offset = static_cast<u32>(slot);
            GPUVAddr value = 0;
            const char* reason = nullptr;
            if (index >= cbufs.size() || !cbufs[index].enabled ||
                offset + sizeof(u64) > cbufs[index].size ||
                !gpu_memory->IsFullyMappedRange(cbufs[index].address + offset, sizeof(u64))) {
                reason = "constant buffer not bound";
            } else {
                value = gpu_memory->Read<u64>(cbufs[index].address + offset);
                if (value == 0) {
                    reason = "null";
                } else if (!gpu_memory->IsWithinGPUAddressRange(value)) {
                    reason = "outside the GPU address range";
                } else if (!gpu_memory->IsFullyMappedRange(value, 1)) {
                    reason = "unmapped";
                }
            }
            if (reason == nullptr) {
                continue;
            }
            const u64 count = kept_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 32 || count % 1000 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "Graphics pipeline {:016x} translated again after eviction: "
                            "s{}:c{}[{:#x}] holds {:#x} ({}), kept as the storage buffer pointer "
                            "it was ({} so far)",
                            graphics_key.Hash(), program, index, offset, value, reason, count);
            }
        }
    }
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {
    const auto begin = std::chrono::steady_clock::now();
    const auto emit_time_before = emit_time_this_thread;
    RuntimeBuildOrigin build_origin{RuntimeBuildOrigin::New};
    SCOPE_EXIT {
        ReportRuntimePipeline("Graphics", begin, build_origin, draws_skipped_while_building,
                              last_build_reused, emit_time_this_thread - emit_time_before);
    };
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);
    if (const auto kept = evicted_pointer_slots.Take(graphics_key.Hash())) {
        KeepEvictedPointers(environments, *kept);
    }

    // EDEN_NX2_PERF_ON=async_pipelines: a native pipeline's SPIR-V is emitted by its build, on a
    // builder worker or the scheduler thread, instead of here. The GPU thread still translates
    // the programs, which the bindings need. At most one per worker waits to be emitted.
    std::shared_ptr<ShaderPools> emit_pools;
    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::AsyncPipelines) &&
        maxwell3d->GetNativeMacroEngine().native_turing &&
        pending_emissions.load(std::memory_order_relaxed) < num_pipeline_workers) {
        {
            std::scoped_lock lock{spare_pools_mutex};
            if (!spare_pools.empty()) {
                emit_pools = std::move(spare_pools.back());
                spare_pools.pop_back();
            }
        }
        if (!emit_pools) {
            emit_pools = std::make_shared<ShaderPools>();
        }
    } else {
        main_pools.ReleaseContents();
    }
    ShaderPools& pools = emit_pools ? *emit_pools : main_pools;
    auto pipeline{CreateGraphicsPipeline(pools, graphics_key, environments.Span(), nullptr, true,
                                         std::move(emit_pools), nullptr, true, true)};
    if (pipeline) {
        pipeline->SetMappedPointers(MappedPointerSlots(graphics_key, environments.envs, false));
    }
    const bool is_mesh{pipeline && pipeline->IsMeshPipeline()};
    const bool on_disk{disk_pipelines.contains(graphics_key.Hash())};
    if (on_disk) {
        build_origin = RuntimeBuildOrigin::DiskCache;
    } else if (is_mesh) {
        build_origin = RuntimeBuildOrigin::Mesh;
    }
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    StoreRuntimePipeline(graphics_key, *pipeline, environments);
    return pipeline;
}

void PipelineCache::StoreRuntimePipeline(const GraphicsPipelineCacheKey& key,
                                         const GraphicsPipeline& pipeline,
                                         GraphicsEnvironments& environments) {
    if (pipeline_cache_filename.empty()) {
        return;
    }
    if (disk_pipelines.contains(key.Hash())) {
        bool incomplete = false;
        {
            std::scoped_lock lock{background_mutex};
            incomplete = incomplete_records.erase(key.Hash()) != 0;
        }
        if (!incomplete) {
            // In the disk cache already, only not built at boot.
            QueueVulkanPipelineCacheFlush();
            return;
        }
    }
    disk_pipelines.insert(key.Hash());
    if (cache_v2) {
        serialization_thread.QueueWork([this, key, envs = std::move(environments.envs)] {
            boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
                env_ptrs;
            for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
                if (key.unique_hashes[index] != 0) {
                    env_ptrs.push_back(&envs[index]);
                }
            }
            const std::vector<u8> payload = VideoCommon::PipelineRecordBytes(key, env_ptrs);
            if (!payload.empty()) {
                cache_v2->Append(VideoCommon::ShaderCacheV2::RecordKind::Graphics,
                                 PipelineCacheFile::GraphicsLogKey(key), payload,
                                 static_cast<u16>(CACHE_VERSION));
            }
        });
        QueueVulkanPipelineCacheFlush();
        return;
    }
    // Task/mesh records carry fields older builds can't read: they go to a file those don't open.
    const std::filesystem::path& filename =
        pipeline.IsMeshPipeline() ? mesh_pipeline_cache_filename : pipeline_cache_filename;
    serialization_thread.QueueWork([key, envs = std::move(environments.envs), &filename] {
        boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
            env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        SerializePipeline(key, env_ptrs, filename, CACHE_VERSION);
    });
    QueueVulkanPipelineCacheFlush();
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineAsync() {
    const auto begin = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        const auto end = std::chrono::steady_clock::now();
        async_stats.gpu_time += end - begin;
        ReportAsyncPipelines(end);
    };
    DrainAsyncPipelines();
    std::shared_ptr<AsyncGraphicsJob> job;
    auto it{graphics_cache.find(graphics_key)};
    if (it == graphics_cache.end()) {
        const bool on_gpu_thread_only = async_sync_keys.erase(graphics_key.Hash()) != 0;
        std::unique_ptr<GraphicsPipeline> pipeline;
        if (!on_gpu_thread_only) {
            ReportNewPipelineKey(graphics_key);
            pipeline = TakeBackgroundPipeline(graphics_key);
            if (!pipeline && CanDiscardAsyncDraw()) {
                const auto queue_begin = std::chrono::steady_clock::now();
                job = QueueAsyncGraphicsPipeline();
                async_stats.queue_time += std::chrono::steady_clock::now() - queue_begin;
            }
        }
        if (!job) {
            if (!pipeline) {
                if (!on_gpu_thread_only) {
                    ++async_stats.sync_built;
                }
                const auto build_begin = std::chrono::steady_clock::now();
                pipeline = CreateGraphicsPipeline();
                async_stats.build_time += std::chrono::steady_clock::now() - build_begin;
            }
            it = graphics_cache.try_emplace(graphics_key).first;
            it->second = std::move(pipeline);
        }
    } else if (!it->second) {
        const auto pending{async_pending.find(graphics_key)};
        if (pending == async_pending.end()) {
            return nullptr; // Its build failed: skipped, as on the synchronous path
        }
        job = pending->second;
    }
    if (job) {
        // A draw that may be skipped waits for the build only within the budget; one that may
        // not (a full-screen pass, say) gets it now, built here if no worker took it yet.
        if (CanDiscardAsyncDraw() && !WaitForAsyncBuild(*job)) {
            ++async_stats.skipped;
            last_pipeline_building = true;
            return nullptr;
        }
        FinishAsyncGraphicsPipeline(job);
        DrainAsyncPipelines();
        it = graphics_cache.find(graphics_key);
        if (it == graphics_cache.end()) {
            // The build missed its snapshot: built here, with the state of this draw.
            async_sync_keys.erase(graphics_key.Hash());
            const auto build_begin = std::chrono::steady_clock::now();
            auto pipeline{CreateGraphicsPipeline()};
            async_stats.build_time += std::chrono::steady_clock::now() - build_begin;
            it = graphics_cache.try_emplace(graphics_key).first;
            it->second = std::move(pipeline);
        }
    }
    if (!it->second) {
        return nullptr;
    }
    return MakeCurrent(it->second.get());
}

bool PipelineCache::CanDiscardAsyncDraw() const {
    // BuiltPipeline's rule for a pipeline whose driver compile is pending: depth-tested draws and
    // ones larger than a full-screen quad are scene geometry the next frames draw again.
    if (maxwell3d->GetNativeMacroEngine().native_turing &&
        (async_pipelines < 2 || NativeSyncPipelines() == NativeSyncMode::Full ||
         !MaySkipNativeDraw(nullptr))) {
        return false;
    }
    if (maxwell3d->regs.zeta_enable) {
        return true;
    }
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    return draw_state.index_buffer.count > 6 && draw_state.vertex_buffer.count > 6;
}

std::shared_ptr<PipelineCache::AsyncGraphicsJob> PipelineCache::QueueAsyncGraphicsPipeline() {
    auto job{std::make_shared<AsyncGraphicsJob>()};
    job->key = graphics_key;
    GetGraphicsEnvironments(job->environments, graphics_key.unique_hashes);
    boost::container::static_vector<VideoCommon::GraphicsEnvironment*, Maxwell::MaxShaderProgram>
        envs;
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (graphics_key.unique_hashes[index] != 0) {
            envs.push_back(&job->environments.envs[index]);
        }
    }
    const auto snapshot{VideoCommon::GraphicsEnvironment::SnapshotForAsync(
        std::span<VideoCommon::GraphicsEnvironment* const>(envs.data(), envs.size()))};
    if (!snapshot) {
        return nullptr;
    }
    async_stats.snapshot_bytes += snapshot->bytes;
    if (auto kept = evicted_pointer_slots.Take(graphics_key.Hash())) {
        KeepEvictedPointers(job->environments, *kept);
        job->kept_pointers = std::move(*kept);
    }
    job->context.environments = &job->environments;
    graphics_cache.try_emplace(graphics_key);
    async_pending.emplace(graphics_key, job);
    ++async_stats.queued;
    workers.QueueWork([this, job] {
        {
            std::scoped_lock lock{async_mutex};
            if (async_stopping) {
                return;
            }
            ++async_running;
        }
        u8 expected = AsyncGraphicsJob::Queued;
        if (job->state.compare_exchange_strong(expected, AsyncGraphicsJob::Running)) {
            BuildAsyncGraphicsPipeline(job, false);
        }
        std::scoped_lock lock{async_mutex};
        --async_running;
        async_idle.notify_all();
    });
    return job;
}

bool PipelineCache::WaitForAsyncBuild(AsyncGraphicsJob& job) {
    if (job.state.load(std::memory_order_acquire) == AsyncGraphicsJob::Done) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - native_wait_window > NativeSyncWindow()) {
        native_wait_window = now;
        native_wait_spent = {};
    }
    const auto wait =
        std::min(NativeSyncDrawBudget(), NativeSyncWindowBudget() - native_wait_spent);
    if (wait.count() <= 0) {
        return false;
    }
    const bool background = background_building.load(std::memory_order_relaxed) != 0;
    std::unique_lock lock{job.mutex};
    const bool done = job.done.wait_for(lock, wait, [&job] {
        return job.state.load(std::memory_order_acquire) == AsyncGraphicsJob::Done;
    });
    const auto waited = std::chrono::steady_clock::now() - now;
    native_wait_spent += std::chrono::duration_cast<std::chrono::microseconds>(waited);
    async_stats.wait_time += waited;
    if (background || background_building.load(std::memory_order_relaxed) != 0) {
        async_stats.wait_background_time += waited;
    }
    return done;
}

void PipelineCache::BuildAsyncGraphicsPipeline(const std::shared_ptr<AsyncGraphicsJob>& job,
                                               bool on_gpu_thread) {
    // On a worker the driver compile runs right here, so the pipeline arrives built. On the GPU
    // thread (a draw that may not be skipped) it goes to the workers, as on the synchronous path.
    std::optional<ShaderPools> worker_pools;
    if (on_gpu_thread) {
        main_pools.ReleaseContents();
    } else {
        worker_pools.emplace();
    }
    ShaderPools& pools{on_gpu_thread ? main_pools : *worker_pools};
    std::unique_ptr<GraphicsPipeline> pipeline;
    bool fallback = false;
    try {
        pipeline = CreateGraphicsPipeline(pools, job->key, job->environments.Span(), nullptr,
                                          on_gpu_thread, nullptr, &job->context, true);
        fallback = job->context.Missed();
    } catch (const std::exception& exception) {
        // Anything CreateGraphicsPipeline does not handle itself: the GPU thread builds it again
        // and meets the error where the synchronous path would.
        LOG_WARNING(Render_Vulkan, "Pipeline {:016x} build off the GPU thread failed: {}",
                    job->key.Hash(), exception.what());
        fallback = true;
    }
    if (fallback) {
        pipeline.reset();
    } else if (!on_gpu_thread) {
        async_stats.worker_built.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::scoped_lock lock{job->mutex};
        job->pipeline = std::move(pipeline);
        job->fallback = fallback;
        job->state.store(AsyncGraphicsJob::Done, std::memory_order_release);
    }
    job->done.notify_all();
    std::scoped_lock lock{async_mutex};
    async_completed.push_back(job);
    async_completed_count.fetch_add(1, std::memory_order_release);
}

void PipelineCache::FinishAsyncGraphicsPipeline(const std::shared_ptr<AsyncGraphicsJob>& job) {
    u8 expected = AsyncGraphicsJob::Queued;
    const auto begin = std::chrono::steady_clock::now();
    if (job->state.compare_exchange_strong(expected, AsyncGraphicsJob::Running)) {
        ++async_stats.inline_built;
        BuildAsyncGraphicsPipeline(job, true);
        async_stats.build_time += std::chrono::steady_clock::now() - begin;
        return;
    }
    if (job->state.load(std::memory_order_acquire) == AsyncGraphicsJob::Done) {
        return;
    }
    ++async_stats.waited;
    const bool background = background_building.load(std::memory_order_relaxed) != 0;
    std::unique_lock lock{job->mutex};
    job->done.wait(lock, [&job] {
        return job->state.load(std::memory_order_acquire) == AsyncGraphicsJob::Done;
    });
    const auto waited = std::chrono::steady_clock::now() - begin;
    async_stats.wait_time += waited;
    if (background || background_building.load(std::memory_order_relaxed) != 0) {
        async_stats.wait_background_time += waited;
    }
}

void PipelineCache::DrainAsyncPipelines() {
    if (async_completed_count.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::vector<std::shared_ptr<AsyncGraphicsJob>> done;
    {
        std::scoped_lock lock{async_mutex};
        done.swap(async_completed);
        async_completed_count.store(0, std::memory_order_relaxed);
    }
    static std::unordered_set<u64> logged_fallbacks; // GPU thread only
    for (const std::shared_ptr<AsyncGraphicsJob>& job : done) {
        async_pending.erase(job->key);
        auto it{graphics_cache.find(job->key)};
        if (job->fallback) {
            ++async_stats.fallback;
            if (it != graphics_cache.end() && !it->second) {
                graphics_cache.erase(it);
            }
            const u64 hash = job->key.Hash();
            async_sync_keys.insert(hash);
            evicted_pointer_slots.Remember(hash, job->kept_pointers);
            if (logged_fallbacks.insert(hash).second) {
                LOG_INFO(Render_Vulkan,
                         "Pipeline {:016x} needs a read its snapshot lacks: built on the GPU "
                         "thread at its next draw",
                         hash);
            }
            continue;
        }
        if (it == graphics_cache.end()) {
            it = graphics_cache.try_emplace(job->key).first;
        }
        if (it->second) {
            // Not expected (the entry stays null while pending): keep the cached one, and
            // destroy this one once the GPU is done with whatever it recorded.
            if (job->pipeline) {
                evicted_pipelines.emplace_back(scheduler.CurrentTick(), std::move(job->pipeline));
            }
            continue;
        }
        GraphicsPipeline* const pipeline{job->pipeline.get()};
        it->second = std::move(job->pipeline);
        if (pipeline != nullptr) {
            // The environments hold what the translation read, as the GPU thread's do.
            pipeline->SetMappedPointers(
                MappedPointerSlots(job->key, job->environments.envs, false));
            StoreRuntimePipeline(job->key, *pipeline, job->environments);
        }
    }
}

void PipelineCache::ReportAsyncPipelines(std::chrono::steady_clock::time_point now) {
    auto& stats{async_stats};
    if (now - stats.window < std::chrono::seconds{10}) {
        return;
    }
    const u32 worker_built = stats.worker_built.exchange(0, std::memory_order_relaxed);
    const u32 background_yields = stats.background_yields.exchange(0, std::memory_order_relaxed);
    const u64 background_yield_ns =
        stats.background_yield_ns.exchange(0, std::memory_order_relaxed);
    if (stats.queued != 0 || worker_built != 0 || stats.inline_built != 0 || stats.waited != 0 ||
        stats.sync_built != 0 || stats.fallback != 0 || stats.skipped != 0 ||
        background_yields != 0) {
        LOG_INFO(Render_Vulkan,
                 "Async pipelines in the last 10 s: {} queued, {} completed on workers, {} inline, "
                 "{} waited for, {} built on the GPU thread (draw not skippable), {} "
                 "failed/fallback, draws skipped {}, snapshot {} KiB per job, GPU-thread ms {:.1f} "
                 "(builds {:.1f}, waits for worker builds {:.1f}, queueing {:.1f}); waits while "
                 "background builds ran {:.1f} ms, background builds waited for the workers {} "
                 "times, {:.1f} ms",
                 stats.queued, worker_built, stats.inline_built, stats.waited, stats.sync_built,
                 stats.fallback, stats.skipped,
                 stats.queued != 0 ? stats.snapshot_bytes / stats.queued / 1024 : 0,
                 std::chrono::duration<double, std::milli>(stats.gpu_time).count(),
                 std::chrono::duration<double, std::milli>(stats.build_time).count(),
                 std::chrono::duration<double, std::milli>(stats.wait_time).count(),
                 std::chrono::duration<double, std::milli>(stats.queue_time).count(),
                 std::chrono::duration<double, std::milli>(stats.wait_background_time).count(),
                 background_yields, static_cast<double>(background_yield_ns) / 1e6);
    }
    stats.queued = 0;
    stats.inline_built = 0;
    stats.waited = 0;
    stats.sync_built = 0;
    stats.fallback = 0;
    stats.skipped = 0;
    stats.snapshot_bytes = 0;
    stats.gpu_time = {};
    stats.build_time = {};
    stats.wait_time = {};
    stats.queue_time = {};
    stats.wait_background_time = {};
    stats.window = now;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    const ComputePipelineCacheKey& key, const ShaderInfo* shader, ComputeRebuildInfo& rebuild_info,
    std::span<const u64> keep_pointers) {
    const auto begin = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        ReportRuntimePipeline("Compute", begin, RuntimeBuildOrigin::New,
                              draws_skipped_while_building, false, {});
    };
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start,
                           shader->lowered_native};
    env.SetCachedSize(shader->size_bytes);
    env.KeepStoragePointers(keep_pointers);

    main_pools.ReleaseContents();
    auto pipeline{CreateComputePipeline(main_pools, key, env, nullptr, true)};
    if (pipeline && env.IsNativeProgram()) {
        rebuild_info.null_pointer_slots = env.NullStoragePointerSlots();
        rebuild_info.mapped_pointers = env.MappedStoragePointerKeys();
    }
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    if (cache_v2) {
        serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
            const std::vector<u8> payload = VideoCommon::PipelineRecordBytes(
                key, std::array<const GenericEnvironment*, 1>{&env_});
            if (!payload.empty()) {
                cache_v2->Append(VideoCommon::ShaderCacheV2::RecordKind::Compute,
                                 VideoCommon::ShaderCacheV2::Hash64(payload), payload,
                                 static_cast<u16>(CACHE_VERSION));
            }
        });
    } else {
        serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
            SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&env_},
                              pipeline_cache_filename, CACHE_VERSION);
        });
    }
    QueueVulkanPipelineCacheFlush();
    return pipeline;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    PipelineStatistics* statistics, bool build_in_parallel) try {
    auto hash = key.Hash();
    if (device.HasBrokenCompute()) {
        LOG_ERROR(Render_Vulkan, "Skipping {:#016x}", hash);
        return nullptr;
    }

    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);

    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};

    // Dump it before error.
    if (Settings::values.dump_guest_shaders) {
        env.Dump(hash, key.unique_hash);
    }

    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    if (const auto edits = Shader::Optimization::ApplyOotVisualWorkaround(
            program, Settings::GetCurrentProgramID(), key.unique_hash, OotVisualWorkaround(),
            OotVisualScales())) {
        LOG_WARNING(Render_Vulkan, "TEMPORARY OoT visual workaround: shader {:016x}, {} scalar edits",
                    key.unique_hash, *edits);
    }
    const VkDriverIdKHR driver_id = device.GetDriverID();
    const bool needs_shared_mem_clamp =
        driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
        driver_id == VK_DRIVER_ID_ARM_PROPRIETARY || driver_id == VK_DRIVER_ID_MOLTENVK;
    const u32 max_shared_memory = device.GetMaxComputeSharedMemorySize();
    if (needs_shared_mem_clamp && program.shared_memory_size > max_shared_memory) {
        LOG_WARNING(Render_Vulkan,
                    "Compute shader {:#016x} requests {}KB shared memory but device max is {}KB - clamping",
                    key.unique_hash,
                    program.shared_memory_size / 1024,
                    max_shared_memory / 1024);
        program.shared_memory_size = max_shared_memory;
    }
    // A compute shader that uses warp operations is built with requiredSubgroupSize = 32 (see
    // ComputePipeline), so on a GPU whose subgroups can be wider (AMD wave64) its subgroup is
    // still exactly one guest warp. Emitting the wide-warp emulation there only costs time: every
    // shuffle, vote and ballot turns into extra index math, and some into chains of broadcasts.
    Shader::Profile compute_profile{profile};
    if (profile.warp_size_potentially_larger_than_guest &&
        device.IsExtSubgroupSizeControlSupported() &&
        device.IsGuestWarpSizeSupported(VK_SHADER_STAGE_COMPUTE_BIT) &&
        device.GetMinSubgroupSize() <= GuestWarpSize) {
        compute_profile.warp_size_potentially_larger_than_guest = false;
    }
    std::vector<u32> code{EmitSPIRV(compute_profile, program)};
    device.SaveShader(code);
    vk::ShaderModule spv_module{
        BuildShader(device, ModuleCode(code, ComputeNoInline(), "Compute", key.unique_hash))};

    // Text log + .spv dump. Same split as the graphics path.
    const bool should_log = GPU::Logging::IsActive();
    const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
    if (should_log || should_dump) {
        const std::string shader_name = fmt::format("shader_{:016x}_compute", key.unique_hash);
        if (should_log) {
            const std::string shader_info = fmt::format("SPIR-V size: {} bytes, hash: {:016x}",
                code.size() * sizeof(u32), key.unique_hash);
            GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
        }
        if (should_dump) {
            GPU::Logging::DumpSpirvShader(key.unique_hash,
                                          std::span<const u32>(code.data(), code.size()));
        }
    }

    if (device.HasDebuggingToolAttached()) {
        const auto name{fmt::format("Shader {:016x}", key.unique_hash)};
        spv_module.SetObjectNameEXT(name.c_str());
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    const bool unoptimized =
        DisableDriverOptimization(device, key.unique_hash, code.size() * sizeof(u32));
    if (unoptimized) {
        LOG_WARNING(Render_Vulkan,
                    "Compute shader {:016x} ({} KiB of SPIR-V) is built without driver "
                    "optimizations on this AMD driver (EDEN_NX2_AMD_SAFE_COMPILE)",
                    key.unique_hash, code.size() * sizeof(u32) / 1024);
    }
    // Only for builds a dispatch waits for in game; boot builds stay optimized.
    const bool fast_first = build_in_parallel && !unoptimized &&
                            !BuildsUnoptimized(device.GetDriverID(), key.unique_hash) &&
                            FastFirstCompute(device, code.size() * sizeof(u32));
    const Shader::Info info{std::move(program.info)};
    if (!thread_worker) {
        // The driver compiles on this thread. Precache and warm-up workers would otherwise hold
        // their IR through the compile, up to hundreds of MiB for one large shader.
        program = {};
        code = std::vector<u32>{};
        pools.block.Free();
        pools.inst.Free();
    }
    return std::make_unique<ComputePipeline>(device, scheduler, vulkan_pipeline_cache, descriptor_pool,
                                             guest_descriptor_queue, descriptor_buffer_ring,
                                             thread_worker, statistics,
                                             &shader_notify, info, std::move(spv_module),
                                             key.unique_hash, unoptimized,
                                             fast_first ? &optimize_workers : nullptr);

} catch (const Shader::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
} catch (const std::exception& exception) {
    LOG_CRITICAL(Render_Vulkan, "Compute pipeline {:#016x} failed, its dispatches are skipped: {}",
                 key.Hash(), exception.what());
    return nullptr;
}

void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) try {
    // Written beside the cache and renamed over it: the emulator stopped mid-write left the cache
    // empty, and every later boot failed to read it.
    auto temporary = filename;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
        .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));

    size_t cache_size = 0;
    std::vector<char> cache_data;
    if (pipeline_cache) {
        pipeline_cache.Read(&cache_size, nullptr);
        cache_data.resize(cache_size);
        pipeline_cache.Read(&cache_size, cache_data.data());
    }
    file.write(cache_data.data(), cache_size);
    file.close();
    std::error_code error;
    std::filesystem::rename(temporary, filename, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to replace Vulkan driver pipeline cache file {}: {}",
                  Common::FS::PathToUTF8String(filename), error.message());
        std::filesystem::remove(temporary, error);
        return;
    }

    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}

vk::PipelineCache PipelineCache::LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                                         u32 expected_cache_version) {
    const auto create_pipeline_cache = [this](size_t data_size, const void* data) {
        VkPipelineCacheCreateInfo pipeline_cache_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = data_size,
            .pInitialData = data};
        return device.GetLogical().CreatePipelineCache(pipeline_cache_ci);
    };
    try {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return create_pipeline_cache(0, nullptr);
        }
        file.exceptions(std::ifstream::failbit);
        const auto end{file.tellg()};
        file.seekg(0, std::ios::beg);

        std::array<char, 8> magic_number;
        u32 cache_version;
        // A file cut short (the emulator stopped while writing it) would throw from the reads
        // below, and macOS's libc++ throws an ios_base::failure the handler doesn't match.
        if (static_cast<size_t>(end) < magic_number.size() + sizeof(cache_version)) {
            file.close();
            LOG_ERROR(Common_Filesystem, "Truncated Vulkan driver pipeline cache file, deleting it");
            Common::FS::RemoveFile(filename);
            return create_pipeline_cache(0, nullptr);
        }
        file.read(magic_number.data(), magic_number.size())
            .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
        if (magic_number != VULKAN_CACHE_MAGIC_NUMBER || cache_version != expected_cache_version) {
            file.close();
            if (Common::FS::RemoveFile(filename)) {
                if (magic_number != VULKAN_CACHE_MAGIC_NUMBER) {
                    LOG_ERROR(Common_Filesystem, "Invalid Vulkan driver pipeline cache file");
                }
                if (cache_version != expected_cache_version) {
                    LOG_INFO(Common_Filesystem, "Deleting old Vulkan driver pipeline cache");
                }
            } else {
                LOG_ERROR(Common_Filesystem,
                          "Invalid Vulkan pipeline cache file and failed to delete it in \"{}\"",
                          Common::FS::PathToUTF8String(filename));
            }
            return create_pipeline_cache(0, nullptr);
        }

        static constexpr size_t header_size = magic_number.size() + sizeof(cache_version);
        const size_t cache_size = static_cast<size_t>(end) - header_size;
        std::vector<char> cache_data(cache_size);
        file.read(cache_data.data(), cache_size);

        LOG_INFO(Render_Vulkan,
                 "Loaded Vulkan driver pipeline cache: {}", Common::FS::PathToUTF8String(filename));

        try {
            return create_pipeline_cache(cache_size, cache_data.data());
        } catch (const vk::Exception& e) {
            // The blob came from another driver or driver version (e.g. switching between
            // MoltenVK and KosmicKrisp on macOS): the driver rejects it. Start empty instead of
            // aborting, and drop the stale file so the next run doesn't retry it.
            LOG_ERROR(Render_Vulkan,
                      "Driver rejected the Vulkan pipeline cache file ({}), starting with an empty "
                      "cache and deleting it",
                      e.what());
            Common::FS::RemoveFile(filename);
            return create_pipeline_cache(0, nullptr);
        }

    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        if (!Common::FS::RemoveFile(filename)) {
            LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                      Common::FS::PathToUTF8String(filename));
        }

        return create_pipeline_cache(0, nullptr);
    }
}

} // namespace Vulkan
