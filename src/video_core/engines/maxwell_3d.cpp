// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <fmt/ranges.h>
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/nx2_title_defaults.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_draw_runs.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/query_dma.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/texture_barrier_epoch.h"
#include "video_core/textures/texture.h"
#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"
#endif

namespace Tegra::Engines {

/// First register id that is actually a Macro call.
constexpr u32 MacroRegistersStart = 0xE00;


Maxwell3D::Maxwell3D(MemoryManager& memory_manager_)
    : draw_manager()
    , memory_manager{memory_manager_}
#ifdef ARCHITECTURE_x86_64
    , macro_engine(bool(Settings::values.disable_macro_jit))
#else
    , macro_engine(true)
#endif
    , upload_state{memory_manager, regs.upload}
{
    dirty.flags.flip();
    InitializeRegisterDefaults();
    reports_fast_draw_breaks = true;
    execution_mask.reset();
    for (size_t i = 0; i < execution_mask.size(); i++)
        execution_mask[i] = IsMethodExecutable(u32(i));
    fast_maxwell3d = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::Method) &&
                     !record_write_history && !trace_first_method_writes;
    memory_write_mask.reset();
    for (u32 i = 0; i < 16; ++i) {
        memory_write_mask[MAXWELL3D_REG_INDEX(const_buffer.buffer) + i] = true;
    }
    for (const u32 method : {MAXWELL3D_REG_INDEX(launch_dma), MAXWELL3D_REG_INDEX(inline_data),
                             MAXWELL3D_REG_INDEX(report_semaphore.query),
                             MAXWELL3D_REG_INDEX(sync_info), MAXWELL3D_REG_INDEX(wait_for_idle)}) {
        memory_write_mask[method] = true;
    }
    // Macros run on the back (without front macros) can do any of the above.
    for (size_t i = MacroRegistersStart; i < memory_write_mask.size(); ++i) {
        memory_write_mask[i] = true;
    }
}

Maxwell3D::~Maxwell3D() = default;

void Maxwell3D::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer_);
}

void Maxwell3D::InitializeRegisterDefaults() {
    // Initializes registers to their default values - what games expect them to be at boot. This is
    // for certain registers that may not be explicitly set by games.

    // Reset all registers to zero
    std::memset(&regs, 0, sizeof(regs));

    // Depth range near/far is not always set, but is expected to be the default 0.0f, 1.0f. This is
    // needed for ARMS.
    for (auto& viewport : regs.viewports) {
        viewport.depth_range_near = 0.0f;
        viewport.depth_range_far = 1.0f;
    }
    for (auto& viewport : regs.viewport_transform) {
        viewport.swizzle.x.Assign(Regs::ViewportSwizzle::PositiveX);
        viewport.swizzle.y.Assign(Regs::ViewportSwizzle::PositiveY);
        viewport.swizzle.z.Assign(Regs::ViewportSwizzle::PositiveZ);
        viewport.swizzle.w.Assign(Regs::ViewportSwizzle::PositiveW);
    }

    // Doom and Bomberman seems to use the uninitialized registers and just enable blend
    // so initialize blend registers with sane values
    regs.blend.color_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.color_source = Regs::Blend::Factor::One_D3D;
    regs.blend.color_dest = Regs::Blend::Factor::Zero_D3D;
    regs.blend.alpha_op = Regs::Blend::Equation::Add_D3D;
    regs.blend.alpha_source = Regs::Blend::Factor::One_D3D;
    regs.blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    for (auto& blend : regs.blend_per_target) {
        blend.color_op = Regs::Blend::Equation::Add_D3D;
        blend.color_source = Regs::Blend::Factor::One_D3D;
        blend.color_dest = Regs::Blend::Factor::Zero_D3D;
        blend.alpha_op = Regs::Blend::Equation::Add_D3D;
        blend.alpha_source = Regs::Blend::Factor::One_D3D;
        blend.alpha_dest = Regs::Blend::Factor::Zero_D3D;
    }
    regs.stencil_front_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_front_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_front_func_mask = 0xFFFFFFFF;
    regs.stencil_front_mask = 0xFFFFFFFF;
    regs.stencil_two_side_enable = 1;
    regs.stencil_back_op.fail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zfail = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.zpass = Regs::StencilOp::Op::Keep_D3D;
    regs.stencil_back_op.func = Regs::ComparisonOp::Always_GL;
    regs.stencil_back_func_mask = 0xFFFFFFFF;
    regs.stencil_back_mask = 0xFFFFFFFF;

    regs.depth_test_func = Regs::ComparisonOp::Always_GL;
    regs.gl_front_face = Regs::FrontFace::CounterClockWise;
    regs.gl_cull_face = Regs::CullFace::Back;

    // TODO(Rodrigo): Most games do not set a point size. I think this is a case of a
    // register carrying a default value. Assume it's OpenGL's default (1).
    regs.point_size = 1.0f;

    // TODO(bunnei): Some games do not initialize the color masks (e.g. Sonic Mania). Assuming a
    // default of enabled fixes rendering here.
    for (auto& color_mask : regs.color_mask) {
        color_mask.R.Assign(1);
        color_mask.G.Assign(1);
        color_mask.B.Assign(1);
        color_mask.A.Assign(1);
    }

    for (auto& format : regs.vertex_attrib_format) {
        format.constant.Assign(1);
    }

    // NVN games expect these values to be enabled at boot
    regs.rasterize_enable = 1;
    regs.color_target_mrt_enable = 1;
    regs.framebuffer_srgb = 1;
    regs.line_width_aliased = 1.0f;
    regs.line_width_smooth = 1.0f;
    regs.gl_front_face = Maxwell3D::Regs::FrontFace::ClockWise;
    regs.polygon_mode_back = Maxwell3D::Regs::PolygonMode::Fill;
    regs.polygon_mode_front = Maxwell3D::Regs::PolygonMode::Fill;

    shadow_state = regs;
}

bool Maxwell3D::IsMethodExecutable(u32 method) {
    if (method >= MacroRegistersStart) {
        return true;
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.end):
    case MAXWELL3D_REG_INDEX(draw.begin):
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
    case MAXWELL3D_REG_INDEX(index_buffer.count):
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(index_buffer32_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer16_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer8_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer32_first):
    case MAXWELL3D_REG_INDEX(index_buffer16_first):
    case MAXWELL3D_REG_INDEX(index_buffer8_first):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_first):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent):
    case MAXWELL3D_REG_INDEX(draw_texture.src_y0):
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
    case MAXWELL3D_REG_INDEX(falcon[4]):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(topology_override):
    case MAXWELL3D_REG_INDEX(clear_surface):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true;
    default:
        return false;
    }
}

void Maxwell3D::ProcessMacro(Core::System& system, u32 method, const u32* base_start, u32 amount, bool is_last_call) {
    if (executing_macro == 0) {
        // A macro call must begin by writing the macro method's register, not its argument.
        ASSERT((method % 2) == 0 && "Can't start macro execution by writing to the ARGS register");
        executing_macro = method;
    }

    macro_segments.emplace_back(current_dma_segment, amount);
    current_macro_dirty |= current_dirty;
    current_dirty = false;

    // Nearly every call arrives whole in one method run (title 1 makes ~1,000,000 a second):
    // copy it to the stack instead of growing the parameter vector. Still a copy: the command
    // list is often guest memory read in place, which the guest may reuse while the macro runs
    // (and the macro memo stores the parameters after it ran). Only accurate GPU mode rewrites
    // the parameters in place (RefreshParameters), so it keeps the vector.
    // EDEN_NX2_MACRO_COPY=1: always use the vector (for comparisons).
    static const bool always_copy = std::getenv("EDEN_NX2_MACRO_COPY") != nullptr;
    if (is_last_call && macro_params.empty() && amount <= 64 && !always_copy &&
        !Settings::IsGPULevelHigh()) {
        std::array<u32, 64> parameters;
        std::copy_n(base_start, amount, parameters.begin());
        ConsumeSink(system);
        CallMacroMethod(system, executing_macro, std::span<const u32>{parameters.data(), amount});
        macro_segments.clear();
        current_macro_dirty = false;
        return;
    }
    macro_params.insert(macro_params.end(), base_start, base_start + amount);

    // Call the macro when there are no more parameters in the command buffer
    if (is_last_call) {
        ConsumeSink(system);
        CallMacroMethod(system, executing_macro, macro_params);
        macro_params.clear();
        macro_segments.clear();
        current_macro_dirty = false;
    }
}

void Maxwell3D::RefreshParametersImpl() {
    if (!Settings::IsGPULevelHigh()) {
        return;
    }
    size_t current_index = 0;
    for (auto& segment : macro_segments) {
        if (segment.first == 0) {
            current_index += segment.second;
            continue;
        }
        memory_manager.ReadBlock(segment.first, &macro_params[current_index],
                                 sizeof(u32) * segment.second);
        current_index += segment.second;
    }
}

u32 Maxwell3D::GetMaxCurrentVertices() {
    u32 num_vertices = 0;
    for (size_t index = 0; index < Regs::NumVertexArrays; ++index) {
        const auto& array = regs.vertex_streams[index];
        if (array.enable == 0) {
            continue;
        }
        const auto& attribute = regs.vertex_attrib_format[index];
        if (attribute.constant) {
            num_vertices = (std::max)(num_vertices, 1U);
            continue;
        }
        const auto& limit = regs.vertex_stream_limits[index];
        const GPUVAddr gpu_addr_begin = array.Address();
        const GPUVAddr gpu_addr_end = limit.Address() + 1;
        const u64 stream_size = GetNativeMacroEngine().native_turing
                                    ? regs.NativeVertexStreamSize(index)
                                    : gpu_addr_end - gpu_addr_begin;
        const u32 address_size = static_cast<u32>((std::min<u64>)(stream_size, (std::numeric_limits<u32>::max)()));
        num_vertices = (std::max)(
            num_vertices, address_size / (std::max)(attribute.SizeInBytes(), array.stride.Value()));
        break;
    }
    return num_vertices;
}

size_t Maxwell3D::EstimateIndexBufferSize() {
    GPUVAddr start_address = regs.index_buffer.StartAddress();
    GPUVAddr end_address = regs.index_buffer.EndAddress();
    auto const byte_size = regs.index_buffer.FormatSizeInBytes();
    auto const max_size = 1ull << (byte_size * CHAR_BIT);
    auto const upper_cap = GetMaxCurrentVertices() * 4 * byte_size;
    auto const lower_cap = std::min<size_t>(size_t(end_address - start_address), upper_cap);
    return std::min<size_t>(memory_manager.GetMemoryLayoutSize(start_address, byte_size * max_size) / byte_size, lower_cap);
}

u32 Maxwell3D::ProcessShadowRam(u32 method, u32 argument) {
    // Keep track of the register value in shadow_state when requested.
    auto const c = shadow_state.shadow_ram_control;
    if (c == Regs::ShadowRamControl::Track || c == Regs::ShadowRamControl::TrackWithFilter)
        return shadow_state.reg_array[method] = argument;
    else if (c == Regs::ShadowRamControl::Replay)
        return shadow_state.reg_array[method];
    return argument;
}

void Maxwell3D::ConsumeSinkImpl(Core::System& system) {
    // Before the sink's writes reach the shadow RAM the front's copy would overwrite.
    if (front_macros && !VideoCommon::GpuSplit::OnBackThread()) {
        ReleaseFront();
    }
    const auto control = shadow_state.shadow_ram_control;
    // Sunk methods are state writes only (not in execution_mask): skip unchanged values, as
    // CallMethod does.
    const bool skip_same = SkipSameValueWrites();
    const auto process = [&](u32 method, u32 value) {
        if (!skip_same || regs.reg_array[method] != value) {
            ProcessDirtyRegisters(method, value);
        } else if (VideoCommon::Nx2DrawRuns::g_enabled) {
            VideoCommon::Nx2DrawRuns::OnWrite(method, false, false);
        }
    };
    if (control == Regs::ShadowRamControl::Track || control == Regs::ShadowRamControl::TrackWithFilter) {
        for (auto [method, value] : method_sink) {
            shadow_state.reg_array[method] = value;
            process(method, value);
        }
    } else if (control == Regs::ShadowRamControl::Replay) {
        for (auto [method, value] : method_sink)
            process(method, shadow_state.reg_array[method]);
    } else {
        for (auto [method, value] : method_sink)
            process(method, value);
    }
    method_sink.clear();
}

void Maxwell3D::ProcessDirtyRegisters(u32 method, u32 argument) {
    // The instances after the first are drawn together once their loop ends, and they must see
    // the state they were begun with: NVN's draw macros reset the base instance (and its copy in
    // c[0][0x644]) right after the loop, before the macro returns.
    if (draw_manager.HasDeferredDraw() && !DrawManager::IsInstanceLoopMethod(method)) [[unlikely]] {
        draw_manager.DrawDeferred(*this, true);
    }
    if (VideoCommon::Nx2DrawRuns::g_enabled) {
        NoteDrawRunsWrite(method, argument);
    }
    if (VideoCommon::Nx2FastDraw::Enabled() && regs.reg_array[method] != argument &&
        !IsFastDrawParameter(method)) {
        VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Register, method);
    }
    regs.reg_array[method] = argument;
    for (auto const& table : dirty.tables)
        dirty.flags[table[method]] = true;
}

bool Maxwell3D::IsFastDrawParameter(u32 method) {
    switch (method) {
    case MAXWELL3D_REG_INDEX(const_buffer.size):
    case MAXWELL3D_REG_INDEX(const_buffer.address_high):
    case MAXWELL3D_REG_INDEX(const_buffer.address_low):
    case MAXWELL3D_REG_INDEX(const_buffer.offset):
    case MAXWELL3D_REG_INDEX(global_base_vertex_index):
    case MAXWELL3D_REG_INDEX(global_base_instance_index):
    case MAXWELL3D_REG_INDEX(index_buffer.start_addr_high):
    case MAXWELL3D_REG_INDEX(index_buffer.start_addr_low):
    case MAXWELL3D_REG_INDEX(index_buffer.limit_addr_high):
    case MAXWELL3D_REG_INDEX(index_buffer.limit_addr_low):
    case MAXWELL3D_REG_INDEX(index_buffer.format):
        return true;
    default:
        return DrawManager::IsInstanceLoopMethod(method) || IsFastDrawDeltaMethod(method) ||
               IsFastDrawVertexBuffer(method);
    }
}

bool Maxwell3D::IsFastDrawVertexBuffer(u32 method) {
    constexpr u32 streams = MAXWELL3D_REG_INDEX(vertex_streams);
    constexpr u32 stream_words = sizeof(Regs::VertexStream) / sizeof(u32);
    if (method >= streams && method < streams + Regs::NumVertexArrays * stream_words) {
        const u32 word = (method - streams) % stream_words;
        return word == 1 || word == 2; // address_high, address_low
    }
    constexpr u32 sizes = 0x600 / sizeof(u32); // SET_VERTEX_STREAM_SIZE_A/B (NativeVertexStreamSize)
    constexpr u32 limits = MAXWELL3D_REG_INDEX(vertex_stream_limits);
    constexpr u32 limit_words = sizeof(Regs::VertexStreamLimit) / sizeof(u32);
    return (method >= sizes && method < sizes + Regs::NumVertexArrays * 2) ||
           (method >= limits && method < limits + Regs::NumVertexArrays * limit_words);
}

bool Maxwell3D::IsFastDrawDeltaMethod(u32 method) {
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method >= cb_data && method < cb_data + Regs::NumCBData) {
        return true;
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
        return true;
    default:
        return false;
    }
}

void Maxwell3D::NoteDrawRunsWrite(u32 method, u32 argument) {
    namespace Runs = VideoCommon::Nx2DrawRuns;
    const bool changed = regs.reg_array[method] != argument;
    Runs::OnWrite(method, changed, true);
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if ((method >= cb_data && method < cb_data + Regs::NumCBData) ||
        method == MAXWELL3D_REG_INDEX(inline_data)) {
        Runs::OnData(sizeof(u32));
        return;
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
        Runs::OnBreak(Runs::Break::Barrier);
        return;
    case MAXWELL3D_REG_INDEX(const_buffer.size):
    case MAXWELL3D_REG_INDEX(const_buffer.address_high):
    case MAXWELL3D_REG_INDEX(const_buffer.address_low):
    case MAXWELL3D_REG_INDEX(const_buffer.offset):
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(clear_surface):
    case MAXWELL3D_REG_INDEX(global_base_vertex_index):
    case MAXWELL3D_REG_INDEX(global_base_instance_index):
        return;
    default:
        break;
    }
    if (changed && !DrawManager::IsInstanceLoopMethod(method)) {
        Runs::OnBreak(Runs::Break::State);
    }
}

void Maxwell3D::ProcessMethodCall(u32 method, u32 argument, u32 nonshadow_argument, bool is_last_call) {
    if (!DrawManager::IsInstanceLoopMethod(method) && !IsFastDrawDeltaMethod(method)) {
        VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Method, method);
    }
    if (GetNativeMacroEngine().native_turing && method == MAXWELL3D_REG_INDEX(clear_surface)) {
        LOG_DEBUG(HW_GPU, "Native GPU surface clear: value={:#x}", argument);
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(wait_for_idle):
        VideoCommon::NoteTextureBarrier();
        return rasterizer->WaitForIdle();
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
        shadow_state.shadow_ram_control = static_cast<Regs::ShadowRamControl>(nonshadow_argument);
        return;
    // With macros on the split GPU thread's front (FrontWrite), the front loads and binds them.
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
        if (front_macros) return;
        return GetNativeMacroEngine().ClearCode(regs.load_mme.instruction_ptr);
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
        if (front_macros) return;
        return GetNativeMacroEngine().AddCode(regs.load_mme.instruction_ptr, argument);
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
        if (front_macros) {
            ++regs.load_mme.start_address_ptr;
            return;
        }
        return ProcessMacroBind(argument);
    case MAXWELL3D_REG_INDEX(falcon[4]):
        return ProcessFirmwareCall4();
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        return ProcessCBData(argument);
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
        return ProcessCBBind(0);
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
        return ProcessCBBind(1);
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
        return ProcessCBBind(2);
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
        return ProcessCBBind(3);
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
        return ProcessCBBind(4);
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
        return ProcessQueryGet();
    case MAXWELL3D_REG_INDEX(render_enable.mode):
        return ProcessQueryCondition();
    case MAXWELL3D_REG_INDEX(clear_report_value):
        return ProcessCounterReset();
    case MAXWELL3D_REG_INDEX(sync_info):
        return ProcessSyncPoint();
    case MAXWELL3D_REG_INDEX(launch_dma):
        return upload_state.ProcessExec(regs.launch_dma.memory_layout.Value() == Regs::LaunchDMA::Layout::Pitch);
    case MAXWELL3D_REG_INDEX(inline_data):
        upload_state.ProcessData(argument, is_last_call);
        return;
    case MAXWELL3D_REG_INDEX(fragment_barrier):
        VideoCommon::NoteTextureBarrier();
        return rasterizer->FragmentBarrier();
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
        VideoCommon::NoteTextureBarrier();
        rasterizer->InvalidateGPUCache();
        return rasterizer->WaitForIdle();
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        VideoCommon::NoteTextureBarrier();
        return rasterizer->TiledCacheBarrier();
    default:
        draw_manager.ProcessMethodCall(*this, method, argument);
        break;
    }
}

void Maxwell3D::CallMacroMethod(Core::System& system, u32 method, std::span<const u32> parameters) {
    // Reset the current macro.
    executing_macro = 0;

    // Lookup the macro offset
    const u32 entry =
        ((method - MacroRegistersStart) >> 1) % static_cast<u32>(GetNativeMacroPositions().size());

    // Execute the current macro.
    GetNativeMacroEngine().Execute(system, *this, GetNativeMacroPositions()[entry], parameters);
    draw_manager.DrawDeferred(*this);
}

void Maxwell3D::TraceFirstMethodWrite(u32 method, u32 argument) {
    static std::array<bool, Regs::NUM_REGS> seen{};
    if (method < seen.size() && !seen[method]) {
        seen[method] = true;
        LOG_INFO(HW_GPU, "3D method first write: {:#x} (byte {:#x}) = {:#x}", method, method * 4, argument);
    }
}

void Maxwell3D::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    if (!fast_maxwell3d || executing_macro != 0 || method >= MacroRegistersStart ||
        method == MAXWELL3D_REG_INDEX(shadow_ram_control) ||
        (front_macros && !VideoCommon::GpuSplit::OnBackThread()) ||
        VideoCommon::Nx2Profile::Enabled() || VideoCommon::Nx2DrawRuns::g_enabled) {
        return CallMethodSlow(system, method, method_argument, is_last_call);
    }
    if (perf_verify) {
        return VerifyCallMethod(system, method, method_argument, is_last_call);
    }
    CallMethodFast(method, method_argument, is_last_call);
}

// CallMethodSlow without the checks that cannot apply (CallMethod): no macro collects parameters,
// no write history, trace, profile or draw runs, not a macro call or SET_SHADOW_RAM_CONTROL, the
// front doesn't own the macros. The hot executable methods go straight to their handler.
void Maxwell3D::CallMethodFast(u32 method, u32 method_argument, bool is_last_call) {
    const u32 argument = ProcessShadowRam(method, method_argument);
    if (!execution_mask[method]) {
        if (!SkipSameValueWrites() || regs.reg_array[method] != argument) {
            ProcessDirtyRegisters(method, argument);
        }
        return;
    }
    ProcessDirtyRegisters(method, argument);
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method - cb_data < 16) {
        return ProcessCBData(argument);
    }
    switch (method) {
    // Executable for their dirty flags only: the draw manager does nothing with them.
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
        return;
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
        return ProcessCBBind(0);
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
        return ProcessCBBind(1);
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
        return ProcessCBBind(2);
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
        return ProcessCBBind(3);
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
        return ProcessCBBind(4);
    default:
        return ProcessMethodCall(method, argument, method_argument, is_last_call);
    }
}

void Maxwell3D::VerifyCallMethod(Core::System& system, u32 method, u32 method_argument,
                                 bool is_last_call) {
    // Only register writes can run twice: handlers draw, upload and bind, and a deferred draw
    // ProcessDirtyRegisters may issue is a draw too.
    const bool store_only = method == MAXWELL3D_REG_INDEX(vertex_buffer.first) ||
                            method == MAXWELL3D_REG_INDEX(vertex_buffer.count) ||
                            method == MAXWELL3D_REG_INDEX(index_buffer.first);
    if ((execution_mask[method] && !store_only) || draw_manager.HasDeferredDraw()) {
        return CallMethodSlow(system, method, method_argument, is_last_call);
    }
    const u32 reg = regs.reg_array[method];
    const u32 shadow = shadow_state.reg_array[method];
    const auto flags = dirty.flags;
    CallMethodFast(method, method_argument, is_last_call);
    const u32 fast_reg = regs.reg_array[method];
    const u32 fast_shadow = shadow_state.reg_array[method];
    const auto fast_flags = dirty.flags;
    regs.reg_array[method] = reg;
    shadow_state.reg_array[method] = shadow;
    dirty.flags = flags;
    CallMethodSlow(system, method, method_argument, is_last_call);
    if (fast_reg == regs.reg_array[method] && fast_shadow == shadow_state.reg_array[method] &&
        fast_flags == dirty.flags) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) >= 64) {
        return;
    }
    std::string flag_differences;
    const auto differing = fast_flags ^ dirty.flags;
    for (size_t i = 0; i < differing.size(); ++i) {
        if (differing[i]) {
            flag_differences += fmt::format(" {}={}", i, fast_flags[i] ? 1 : 0);
        }
    }
    LOG_ERROR(HW_GPU,
              "Method fast: {:#x} = {:#x}: register {:#x} (slow {:#x}), shadow {:#x} (slow {:#x}), "
              "dirty flags differ:{}",
              method, method_argument, fast_reg, regs.reg_array[method], fast_shadow,
              shadow_state.reg_array[method], flag_differences);
}

void Maxwell3D::CallConstBufferWords(Core::System& system, const u32* methods,
                                     const u32* arguments, u32 count) {
    const u32 offset = regs.const_buffer.offset;
    const u64 bytes = u64{count} * sizeof(u32);
    const GPUVAddr address = regs.const_buffer.Address();
    // Word by word when CallMethod would not take its fast path, the words don't all fit in the
    // buffer (ProcessCBMultiData's handling of those stays as it is), or they cross a device page:
    // each word is translated on its own there, and a run past the end of the address space
    // still writes the words before it.
    if (!fast_maxwell3d || executing_macro != 0 || address == 0 ||
        (front_macros && !VideoCommon::GpuSplit::OnBackThread()) ||
        VideoCommon::Nx2Profile::Enabled() || VideoCommon::Nx2DrawRuns::g_enabled ||
        offset > regs.const_buffer.size || bytes > regs.const_buffer.size - offset ||
        bytes > Core::DEVICE_PAGESIZE - ((address + offset) & Core::DEVICE_PAGEMASK)) {
        for (u32 i = 0; i < count; ++i) {
            CallMethod(system, methods[i], arguments[i], true);
        }
        return;
    }
    std::array<u32, 64> values;
    const bool replay = shadow_state.shadow_ram_control == Regs::ShadowRamControl::Replay;
    for (u32 i = 0; i < count; ++i) {
        values[i] = replay ? shadow_state.reg_array[methods[i]] : arguments[i];
    }
    if (perf_verify) {
        for (u32 i = 0; i < count; ++i) {
            CallMethodSlow(system, methods[i], arguments[i], true);
        }
        std::array<u32, 64> written;
        const bool mapped = memory_manager.IsFullyMappedRange(address + offset, bytes);
        if (mapped) {
            memory_manager.ReadBlockUnsafe(address + offset, written.data(), bytes);
        }
        const bool differs = mapped && std::memcmp(written.data(), values.data(), bytes) != 0;
        if (regs.const_buffer.offset != offset + bytes || differs) {
            static std::atomic<u32> reports{};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 64) {
                LOG_ERROR(HW_GPU,
                          "Method fast: {} constant buffer words at {:#x}+{:#x}: offset {:#x}, "
                          "expected {:#x}, memory {}",
                          count, address, offset, regs.const_buffer.offset, offset + bytes,
                          differs ? "differs" : "matches");
            }
        }
        return;
    }
    // As CallMethodFast for each word: the first may issue a deferred draw, before any upload.
    for (u32 i = 0; i < count; ++i) {
        ProcessDirtyRegisters(methods[i], ProcessShadowRam(methods[i], arguments[i]));
    }
    if (VideoCommon::Nx2FastDraw::Enabled()) {
        NoteFastDrawConstantData(address + offset, address + offset + bytes);
    }
    memory_manager.WriteWordsCached(address + offset, values.data(), count);
    regs.const_buffer.offset += static_cast<u32>(bytes);
}

void Maxwell3D::CallMethodSlow(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    if (front_macros && !VideoCommon::GpuSplit::OnBackThread()) {
        ReleaseFront();
    }
    // It is an error to write to a register other than the current macro's ARG register before
    // it has finished execution.
    if (executing_macro != 0 && method != executing_macro + 1) {
        DumpRecentWrites(method);
    }
    if (executing_macro != 0) {
        ASSERT(method == executing_macro + 1);
    }
    RecordWrite(method, method_argument, 1, is_last_call ? 1 : 2);

    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        ProcessMacro(system, method, &method_argument, 1, is_last_call);
        return;
    }
    if (trace_first_method_writes) [[unlikely]] {
        TraceFirstMethodWrite(method, method_argument);
    }

    ASSERT(method < Regs::NUM_REGS && "Invalid Maxwell3D register, increase the size of the Regs structure");
    if (VideoCommon::Nx2Profile::Enabled()) [[unlikely]] {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::MethodWrites, 1);
        if (regs.reg_array[method] == method_argument) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::SameValueWrites, 1);
        }
    }
    if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && front_macros) [[unlikely]] {
        back_control_writes[back_control_count++ % back_control_writes.size()] = {
            method_argument, static_cast<u32>(shadow_state.shadow_ram_control), is_last_call ? 1U : 0U};
    }
    const u32 argument = ProcessShadowRam(method, method_argument);
    // A state register written with the value it holds changes nothing: no dirty flags (they
    // would make the next draw re-check that state), no method processing. Title 1 makes ~6
    // million 3D register writes a second, ~60% of them same-value, most from macro replays.
    // Methods with side effects (execution_mask: draws, clears, constant buffer data and
    // binds, semaphores, ...) always run. EDEN_NX2_SKIP_SAME=0 turns it off.
    if (!execution_mask[method]) {
        // ProcessMethodCall (and the draw manager behind it) acts only on executable methods.
        if (!SkipSameValueWrites() || regs.reg_array[method] != argument) {
            ProcessDirtyRegisters(method, argument);
        } else if (VideoCommon::Nx2DrawRuns::g_enabled) {
            VideoCommon::Nx2DrawRuns::OnWrite(method, false, false);
        }
        return;
    }
    ProcessDirtyRegisters(method, argument);
    ProcessMethodCall(method, argument, method_argument, is_last_call);
}

void Maxwell3D::DumpRecentWrites(u32 method) {
    static u32 dumps{};
    if (dumps++ >= 4) {
        return;
    }
    LOG_ERROR(HW_GPU, "Write to {:#x} while macro {:#x} collects parameters ({} so far); last writes:",
              method, executing_macro, macro_params.size());
    if (!record_write_history) {
        LOG_ERROR(HW_GPU, "Register write history disabled; EDEN_NX2_3D_HISTORY=1 enables it");
        return;
    }
    for (size_t i = 0; i < recent_writes.size(); ++i) {
        const auto& w = recent_writes[(recent_write_index + i) % recent_writes.size()];
        if (w.amount != 0) {
            LOG_ERROR(HW_GPU, "  method {:#x} arg {:#x} amount {} pending {}", w.method, w.argument,
                      w.amount, w.pending);
        }
    }
}

void Maxwell3D::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    if (front_macros && !VideoCommon::GpuSplit::OnBackThread()) {
        ReleaseFront();
    }
    RecordWrite(method, amount ? base_start[0] : 0, amount, methods_pending);
    // Methods after 0xE00 are special, they're actually triggers for some microcode that was
    // uploaded to the GPU during initialization.
    if (method >= MacroRegistersStart) {
        ProcessMacro(system, method, base_start, amount, amount == methods_pending);
        return;
    }
    // As in ProcessDirtyRegisters: the paths below write without it.
    if (draw_manager.HasDeferredDraw() && !DrawManager::IsInstanceLoopMethod(method)) [[unlikely]] {
        draw_manager.DrawDeferred(*this, true);
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(const_buffer.buffer):
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 1:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 2:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 3:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 4:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 5:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 6:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 7:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 8:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 9:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 10:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 11:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 12:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 13:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 14:
    case MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15:
        if (VideoCommon::Nx2DrawRuns::g_enabled) {
            VideoCommon::Nx2DrawRuns::OnData(u64{amount} * sizeof(u32));
        }
        ProcessCBMultiData(base_start, amount);
        break;
    case MAXWELL3D_REG_INDEX(inline_data): {
        ASSERT(methods_pending == amount);
        VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::InlineData);
        if (VideoCommon::Nx2DrawRuns::g_enabled) {
            VideoCommon::Nx2DrawRuns::OnData(u64{amount} * sizeof(u32));
        }
        upload_state.ProcessData(base_start, amount);
        return;
    }
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
        if (shadow_state.shadow_ram_control != Regs::ShadowRamControl::Replay) {
            ProcessInlineIndexMultiData(method, base_start, amount);
            break;
        }
        [[fallthrough]];
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

void Maxwell3D::ProcessMacroUpload(u32 data) {
    GetNativeMacroEngine().AddCode(regs.load_mme.instruction_ptr++, data);
}

void Maxwell3D::ProcessMacroBind(u32 data) {
    const auto positions = GetNativeMacroPositions();
    const u32 index = regs.load_mme.start_address_ptr++;
    if (index >= positions.size()) {
        static std::atomic<u32> rejected{};
        if (rejected.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOG_ERROR(HW_GPU, "Rejecting macro bind: index {:#x}, data {:#x}, table size {:#x}",
                      index, data, positions.size());
        }
        return;
    }
    positions[index] = data;
}

void Maxwell3D::ProcessFirmwareCall4() {
    LOG_DEBUG(HW_GPU, "(STUBBED) called");

    // Firmware call 4 is a blob that changes some registers depending on its parameters.
    // These registers don't affect emulation and so are stubbed by setting 0xd00 to 1.
    regs.shadow_scratch[0] = 1;
}

void Maxwell3D::StampQueryResult(Core::System& system, u64 payload, bool long_query) {
    const GPUVAddr sequence_address{regs.report_semaphore.Address()};
    if (long_query) {
        memory_manager.Write<u64>(sequence_address + sizeof(u64), system.GPU().GetTicks());
        memory_manager.Write<u64>(sequence_address, payload);
    } else {
        memory_manager.Write<u32>(sequence_address, static_cast<u32>(payload));
    }
}

void Maxwell3D::ProcessQueryGet() {
    VideoCommon::QueryPropertiesFlags flags{};
    if (regs.report_semaphore.query.short_query == 0) {
        flags |= VideoCommon::QueryPropertiesFlags::HasTimeout;
    }
    const GPUVAddr sequence_address{regs.report_semaphore.Address()};
    const VideoCommon::QueryType query_type =
        static_cast<VideoCommon::QueryType>(regs.report_semaphore.query.report.Value());
    const u32 payload = regs.report_semaphore.payload;
    const u32 subreport = regs.report_semaphore.query.sub_report;
    switch (regs.report_semaphore.query.operation) {
    case Regs::ReportSemaphore::Operation::Release:
        if (regs.report_semaphore.query.short_query != 0) {
            flags |= VideoCommon::QueryPropertiesFlags::IsAFence;
        }
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Acquire:
        // TODO(Blinkhawk): Under this operation, the GPU waits for the CPU to write a value that
        // matches the current payload.
        UNIMPLEMENTED_MSG("Unimplemented query operation ACQUIRE");
        break;
    case Regs::ReportSemaphore::Operation::ReportOnly:
        rasterizer->Query(sequence_address, query_type, flags, payload, subreport);
        break;
    case Regs::ReportSemaphore::Operation::Trap:
        UNIMPLEMENTED_MSG("Unimplemented query operation TRAP");
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown query operation");
        break;
    }
}

void Maxwell3D::ProcessQueryCondition() {
    if (rasterizer->AccelerateConditionalRendering()) {
        execute_on = true;
        return;
    }
    const GPUVAddr condition_address{regs.render_enable.Address()};
    switch (regs.render_enable_override) {
    case Regs::RenderEnable::Override::AlwaysRender:
        execute_on = true;
        break;
    case Regs::RenderEnable::Override::NeverRender:
        execute_on = false;
        break;
    case Regs::RenderEnable::Override::UseRenderEnable: {
        switch (regs.render_enable.mode) {
        case Regs::RenderEnable::Mode::True: {
            execute_on = true;
            break;
        }
        case Regs::RenderEnable::Mode::False: {
            execute_on = false;
            break;
        }
        case Regs::RenderEnable::Mode::Conditional: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != 0U && cmp.initial_mode != 0U;
            break;
        }
        case Regs::RenderEnable::Mode::IfEqual: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence == cmp.current_sequence &&
                         cmp.initial_mode == cmp.current_mode;
            break;
        }
        case Regs::RenderEnable::Mode::IfNotEqual: {
            Regs::ReportSemaphore::Compare cmp;
            memory_manager.ReadBlock(condition_address, &cmp, sizeof(cmp));
            execute_on = cmp.initial_sequence != cmp.current_sequence ||
                         cmp.initial_mode != cmp.current_mode;
            break;
        }
        default: {
            UNIMPLEMENTED_MSG("Uninplemented Condition Mode!");
            execute_on = true;
            break;
        }
        }
        break;
    }
    }
}

void Maxwell3D::ProcessCounterReset() {
    const auto query_type = [clear_report = regs.clear_report_value]() {
        switch (clear_report) {
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::ZPassPixelCount:
            return VideoCommon::QueryType::ZPassPixelCount64;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::StreamingPrimitivesSucceeded:
            return VideoCommon::QueryType::StreamingPrimitivesSucceeded;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::PrimitivesGenerated:
            return VideoCommon::QueryType::PrimitivesGenerated;
        case Tegra::Engines::Maxwell3D::Regs::ClearReport::VtgPrimitivesOut:
            return VideoCommon::QueryType::VtgPrimitivesOut;
        default:
            LOG_DEBUG(HW_GPU, "Unimplemented counter reset={}", clear_report);
            return VideoCommon::QueryType::Payload;
        }
    }();
    rasterizer->ResetCounter(query_type);
}

void Maxwell3D::ProcessSyncPoint() {
    if (GetNativeMacroEngine().native_turing) {
        LOG_DEBUG(HW_GPU, "Native 3D syncpoint increment: id={}, raw={:#x}", regs.sync_info.sync_point.Value(), regs.reg_array[MAXWELL3D_REG_INDEX(sync_info)]);
    }
    const u32 sync_point = regs.sync_info.sync_point.Value();
    [[maybe_unused]] const u32 cache_flush = regs.sync_info.clean_l2.Value();
    rasterizer->SignalSyncPoint(sync_point);
}

void Maxwell3D::ProcessCBBind(size_t stage_index) {
    // Bind the buffer currently in CB_ADDRESS to the specified index in the desired shader
    // stage.
    const auto& bind_data = regs.bind_groups[stage_index];
    static std::array<std::array<bool, 32>, 5> seen{};
    if (!seen[stage_index][bind_data.shader_slot]) {
        seen[stage_index][bind_data.shader_slot] = true;
        LOG_INFO(HW_GPU, "Constant buffer bind: stage {} slot {} address {:#x} size {:#x}", stage_index,
                 bind_data.shader_slot.Value(), regs.const_buffer.Address(), regs.const_buffer.size);
    }
    if (bind_data.shader_slot >= Regs::MaxConstBuffers) {
        return; // slot field is 5 bits; the state array holds 18
    }
    auto& buffer = state.shader_stages[stage_index].const_buffers[bind_data.shader_slot];
    if (VideoCommon::Nx2DrawRuns::g_enabled) {
        VideoCommon::Nx2DrawRuns::OnConstBufferBind(
            buffer.enabled == (bind_data.valid.Value() != 0) &&
            buffer.address == regs.const_buffer.Address() && buffer.size == regs.const_buffer.size);
    }
    if (VideoCommon::Nx2FastDraw::Enabled() &&
        (buffer.enabled != (bind_data.valid.Value() != 0) ||
         buffer.address != regs.const_buffer.Address() || buffer.size != regs.const_buffer.size)) {
        VideoCommon::Nx2FastDraw::NoteBind(stage_index, bind_data.shader_slot);
    }
    buffer.enabled = bind_data.valid.Value() != 0;
    buffer.address = regs.const_buffer.Address();
    buffer.size = regs.const_buffer.size;

    const bool is_enabled = bind_data.valid.Value() != 0;
    if (!is_enabled) {
        rasterizer->DisableGraphicsUniformBuffer(stage_index, bind_data.shader_slot);
        return;
    }
    const GPUVAddr gpu_addr = regs.const_buffer.Address();
    const u32 size = regs.const_buffer.size;
    rasterizer->BindGraphicsUniformBuffer(stage_index, bind_data.shader_slot, gpu_addr, size);
}

void Maxwell3D::NoteFastDrawConstantData(GPUVAddr begin, GPUVAddr end) {
    namespace FastDraw = VideoCommon::Nx2FastDraw;
    // Texture and sampler headers are read when image views are resolved, which delta draws skip.
    constexpr u64 HeaderSize = 0x20;
    const GPUVAddr tic = regs.tex_header.Address();
    const u64 tic_size = (u64{regs.tex_header.limit} + 1) * HeaderSize;
    // As TextureCache::SynchronizeDescriptors sizes the sampler table.
    const GPUVAddr tsc = regs.tex_sampler.Address();
    const u64 tsc_size = (u64{regs.sampler_binding == Regs::SamplerBinding::ViaHeaderBinding
                                  ? regs.tex_header.limit
                                  : regs.tex_sampler.limit} +
                          1) *
                         HeaderSize;
    if ((begin < tic + tic_size && tic < end) || (begin < tsc + tsc_size && tsc < end)) {
        FastDraw::Break(FastDraw::Reason::TextureHeaders);
        return;
    }
    FastDraw::NoteConstantData(begin, end);
    // The same device memory can be mapped at other GPU addresses: compare where it lands.
    fast_draw_header_spans.Update(memory_manager, tic, tic_size);
    fast_draw_sampler_spans.Update(memory_manager, tsc, tsc_size);
    constexpr u64 PageSize = 0x1000; // the smallest GPU page
    for (GPUVAddr page = begin; page < end;) {
        const GPUVAddr page_end = (std::min)(end, (page & ~(PageSize - 1)) + PageSize);
        if (const std::optional<DAddr> device = memory_manager.GpuToCpuAddress(page)) {
            const DAddr device_end = *device + (page_end - page);
            if (fast_draw_header_spans.Intersects(*device, device_end) ||
                fast_draw_sampler_spans.Intersects(*device, device_end)) {
                FastDraw::Break(FastDraw::Reason::TextureHeaders);
                return;
            }
            FastDraw::NoteDeviceData(*device, device_end);
        }
        page = page_end;
    }
}

void Maxwell3D::ProcessCBMultiData(const u32* start_base, u32 amount) {
    // Write the input value to the current const buffer at the current position.
    const GPUVAddr buffer_address = regs.const_buffer.Address();
    ASSERT(buffer_address != 0);

    // Switch 2 titles don't write past the end of the buffer. Switch 1 titles still write every
    // word, as before, until dropping them there has been tested.
    const size_t copy_size = amount * sizeof(u32);
    const size_t room = regs.const_buffer.offset < regs.const_buffer.size
                            ? regs.const_buffer.size - regs.const_buffer.offset
                            : 0;
    size_t write_size = (std::min)(copy_size, room / sizeof(u32) * sizeof(u32));
    if (write_size != copy_size) {
        const bool drop = Settings::CurrentProgramIsSwitch2();
        static std::atomic<bool> warned{};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR(HW_GPU,
                      "Constant buffer upload past the end ({}): offset={:#x}, size={:#x}, "
                      "bytes={:#x}, inside={:#x}",
                      drop ? "dropped" : "written anyway", regs.const_buffer.offset,
                      regs.const_buffer.size, copy_size, write_size);
        }
        if (!drop) {
            write_size = copy_size;
        }
    }
    if (write_size != 0) {
        const GPUVAddr address{buffer_address + regs.const_buffer.offset};
        if (VideoCommon::Nx2FastDraw::Enabled()) {
            NoteFastDrawConstantData(address, address + write_size);
        }
        memory_manager.WriteBlockCached(address, start_base, write_size);
    }

    // Increment the current buffer position.
    regs.const_buffer.offset += static_cast<u32>(copy_size);
}

void Maxwell3D::ProcessCBData(u32 value) {
    ProcessCBMultiData(&value, 1);
}

void Maxwell3D::ProcessInlineIndexMultiData(u32 method, const u32* start_base, u32 amount) {
    if (amount == 0) {
        return;
    }
    const u32 argument = ProcessShadowRam(method, start_base[amount - 1]);
    ProcessDirtyRegisters(method, argument);
    draw_manager.SetInlineIndexBuffer(*this, method, start_base, amount);
}

Texture::TICEntry Maxwell3D::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tex_header.Address() +
                                   tic_index * sizeof(Texture::TICEntry)};
    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry Maxwell3D::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tex_sampler.Address() +
                                   tsc_index * sizeof(Texture::TSCEntry)};
    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

u32 Maxwell3D::GetRegisterValue(u32 method) const {
    ASSERT(method < Regs::NUM_REGS && "Invalid Maxwell3D register");
    return regs.reg_array[method];
}

void Maxwell3D::SetHLEReplacementAttributeType(u32 bank, u32 offset,
                                               HLEReplacementAttributeType name) {
    const u64 key = (static_cast<u64>(bank) << 32) | offset;
    replace_table.emplace(key, name);
}

// Split GPU thread with macros on its front half (gpu_split.h, EDEN_NX2_SPLIT=2). The front keeps
// what macros read: the registers as the back thread will hold them when it reaches the same
// point of the method stream. That is every write the front sends, with the shadow RAM applied,
// plus the few changes the back makes on its own (constant buffer offset, shadow_scratch[0] on
// falcon[4], the macro bind pointer); the front loads and binds the macro code itself. With
// EDEN_NX2_BACK_MACROS, macros that read memory run on the back instead (FrontRunOnBack), and the
// registers they can write are the back's until the front writes them or reads them back
// (FrontRegister).
struct Maxwell3D::FrontState {
    std::array<ControlWrite, 16> control_writes{};
    u32 control_count{};
    std::array<u32, Regs::NUM_REGS> regs{};
    std::array<u32, Regs::NUM_REGS> shadow{};
    Regs::ShadowRamControl control{};
    u32 executing_macro{};
    std::vector<u32> parameters;
    u64 calls{};
    /// A method the draw manager handles was sent since the back last ran DrawDeferred: only
    /// then can it have a deferred instanced draw to issue at the end of a macro.
    bool draw_pending{true};
    /// The back holds state writes in its method sink, which its next executable method consumes
    /// (issuing a deferred draw a changed one ends).
    bool sink_pending{};
    u32 split_start{}; ///< GpuSplit::Starts() when this copy was made
    bool skip_same{}; ///< SkipSameValueWrites() && FrontSkipSame(), read once
    /// The back's draw manager is in an instance loop (DrawMode::Instance): draw.end does nothing.
    bool instance_loop{};
    /// GPU address of the running call's indirect draw arguments (FrontTakeIndirectArguments).
    GPUVAddr indirect_arguments{};
    /// Whether the macro code at indirect_macro_start is NVN's indirect draw macro, as of
    /// indirect_macro_generation of the macro code.
    u32 indirect_macro_start{~0U};
    u64 indirect_macro_generation{~0ULL};
    bool indirect_macro{};
    bool defer_conditional{}; ///< EDEN_NX2_DEFER_CONDITIONAL, read when this copy was made
    /// Where NVN's conditional render macro is, as of conditional_macro_generation of the macro
    /// code (FrontRunConditionalRender).
    u32 conditional_macro_start{~0U};
    u64 conditional_macro_generation{~0ULL};
    /// render_enable.mode was set on the back from a query result: regs (and shadow, when the
    /// write was tracked) hold an old value until FrontSyncRenderEnable.
    bool render_enable_pending{};
    bool render_enable_shadow_pending{};
    std::vector<ParkedMacro> parked; ///< ParkMacro's calls while the front runs the macros
    /// EDEN_NX2_DMA_SNAPSHOT: device pages a drained macro DMA read flushed whole, and the split
    /// position right after. Cleared past 8192 pages.
    std::unordered_map<u64, u64> dma_snapshots;

    bool back_macros_enabled{}; ///< EDEN_NX2_BACK_MACROS, read when this copy was made
    /// Registers a macro the back ran may have written (FrontRunOnBack), and their shadow RAM
    /// entries: the ring position after the last such macro, 0 once the front knows the value.
    std::array<u64, Regs::NUM_REGS> back_regs{};
    std::array<u64, Regs::NUM_REGS> back_shadow{};
    std::bitset<Regs::NUM_REGS> back_owned; ///< back_regs or back_shadow set
    struct BackMacro {
        u32 start{};
        bool back{};                           ///< Runs on the back (else on the front)
        std::vector<u32> writes;               ///< The registers its code can write
        std::bitset<Regs::NUM_REGS> write_set; ///< The same, for the back's check
        std::atomic<bool> escaped{};           ///< The back saw it write another register
    };
    /// By start in the macro code of back_generation, once called.
    std::vector<std::unique_ptr<BackMacro>> back_macros;
    u64 back_generation{~u64{0}};
    std::vector<u32> back_demoted; ///< Starts of macros that wrote outside their write set
    /// The back's macro engine, its code a copy of the front's (when back_code_copied).
    std::optional<MacroEngine> back_engine;
    bool back_code_copied{};
    u64 back_last{};                 ///< The position after the last macro sent to the back
    std::atomic<u32> back_escapes{}; ///< Back macros that wrote outside their write set
    u32 back_escapes_seen{};
};

namespace {

// EDEN_NX2_SPLIT_VERIFY=1: every 4096 front macro calls, wait for the back and compare the
// front's registers with the back's (logs and resynchronizes differences).
bool FrontVerifyEnabled() {
    static const bool verify = std::getenv("EDEN_NX2_SPLIT_VERIFY") != nullptr;
    return verify;
}

// Executable methods ProcessMethodCall handles itself; the rest go to the draw manager.
bool FrontHandledOutsideDrawManager(u32 method) {
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method >= cb_data && method < cb_data + 16) {
        return true;
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(shadow_ram_control):
    case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
    case MAXWELL3D_REG_INDEX(load_mme.instruction):
    case MAXWELL3D_REG_INDEX(load_mme.start_address):
    case MAXWELL3D_REG_INDEX(falcon[4]):
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(report_semaphore.query):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(sync_info):
    case MAXWELL3D_REG_INDEX(launch_dma):
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true;
    default:
        return false;
    }
}

// EDEN_NX2_SPLIT_SKIP=0: send state writes that keep the register's value too.
bool FrontSkipSame() {
    static const bool skip = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT_SKIP");
        return value == nullptr || value[0] != '0';
    }();
    return skip;
}

// Nx2Perf split_overlap and forward_reads: tells the split what memory a memory-writing method
// about to be sent writes. `regs` holds the front's registers with the method's own write applied,
// `cb_offset` the constant buffer offset before it, `words` the words the method carries and
// `data` (if not null) the words a constant buffer upload writes, in address space `space`.
// Without a range the write counts as one anywhere.
void NoteFrontWriteRange(VideoCommon::GpuSplit& split, const Maxwell3D::Regs& regs, u32 method,
                         u32 cb_offset, u32 words, const u32* data, size_t space) {
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    if (method >= cb_data && method < cb_data + 16) {
        const GPUVAddr begin = regs.const_buffer.Address() + cb_offset;
        const u64 bytes = u64{words} * sizeof(u32);
        // ProcessCBMultiData drops a Switch 2 title's words past the end of the buffer.
        const bool inside = cb_offset <= regs.const_buffer.size &&
                            bytes <= regs.const_buffer.size - cb_offset;
        split.SetNextWriteRange(begin, begin + bytes, inside ? data : nullptr, space);
        return;
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(inline_data):
    case MAXWELL3D_REG_INDEX(launch_dma): {
        if (regs.launch_dma.memory_layout != Maxwell3D::Regs::LaunchDMA::Layout::Pitch) {
            return; // Block linear: left untracked
        }
        const auto& upload = regs.upload;
        const u64 line = (std::max)(upload.line_length_in, upload.dest.pitch);
        const u64 size = line * (std::max)(upload.line_count, 1U);
        if (size == 0 || size > (u64{64} << 20)) {
            return;
        }
        const GPUVAddr begin = upload.dest.Address();
        split.SetNextWriteRange(begin, begin + size);
        return;
    }
    case MAXWELL3D_REG_INDEX(report_semaphore.query): {
        const GPUVAddr begin = regs.report_semaphore.Address();
        split.SetNextWriteRange(begin, begin + 16);
        return;
    }
    default:
        return;
    }
}

// EDEN_NX2_ACQUIRE_QUEUED and EDEN_NX2_ACQUIRE_WRITER_WAIT: what the report the front just sent
// writes, from its copy of the report registers (GpuSplit::NoteQueuedRelease). A payload
// release is noted; any other report to the word (a counter, a reduction) makes the split
// forget it.
void NoteFrontReport(VideoCommon::GpuSplit& split, MemoryManager& memory_manager, const u32* regs,
                     u32 query) {
    using ReportRegs = Maxwell3D::Regs::ReportSemaphore;
    ReportRegs report{};
    report.address_high = regs[MAXWELL3D_REG_INDEX(report_semaphore.address_high)];
    report.address_low = regs[MAXWELL3D_REG_INDEX(report_semaphore.address_low)];
    report.payload = regs[MAXWELL3D_REG_INDEX(report_semaphore.payload)];
    report.query.raw = query;
    const auto operation = report.query.operation.Value();
    if (operation != ReportRegs::Operation::Release &&
        operation != ReportRegs::Operation::ReportOnly) {
        return;
    }
    const GPUVAddr address = report.Address();
    if (memory_manager.IsUpdateQueued(address, sizeof(u32))) {
        split.ForgetQueuedReleases();
        return;
    }
    const auto device = memory_manager.GpuToCpuAddress(address);
    if (!device) {
        return;
    }
    if (report.query.report.Value() == ReportRegs::Report::Payload &&
        report.query.reduction_enable == 0U) {
        split.NoteQueuedRelease(*device, report.payload);
    } else {
        split.ForgetQueuedRelease(*device);
    }
}

// EDEN_NX2_BACK_MACROS: whether a macro the split's back runs may write `method` (FrontRunOnBack):
// the front then only loses track of the register's value. Not the methods with effects the front
// tracks (macro code and bindings, the shadow RAM mode, draws, constant buffer data, reports,
// uploads, syncpoints), nor the registers it reads to note their memory writes, nor registers the
// back changes itself, nor MME_DMA_READ, whose words land in the front's data RAM.
bool BackWritable(const Maxwell3D& maxwell3d, u32 method) {
    constexpr u32 const_buffer = MAXWELL3D_REG_INDEX(const_buffer);
    constexpr u32 report = MAXWELL3D_REG_INDEX(report_semaphore);
    constexpr u32 upload = MAXWELL3D_REG_INDEX(upload);
    constexpr u32 upload_end = MAXWELL3D_REG_INDEX(launch_dma);
    if (method - const_buffer < 4 || method - report < 4 || method - upload < upload_end - upload) {
        return false;
    }
    switch (method) {
    case MAXWELL3D_REG_INDEX(load_mme.start_address_ptr): // Advanced by load_mme.start_address
    case MAXWELL3D_REG_INDEX(shadow_scratch):             // Set by falcon[4]
    case MAXWELL3D_REG_INDEX(logic_op.enable):            // Changed by draws on AMD
    case 0x157:
        return false;
    case MAXWELL3D_REG_INDEX(wait_for_idle):
    case MAXWELL3D_REG_INDEX(bind_groups[0].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[1].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[2].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[3].raw_config):
    case MAXWELL3D_REG_INDEX(bind_groups[4].raw_config):
    case MAXWELL3D_REG_INDEX(render_enable.mode):
    case MAXWELL3D_REG_INDEX(clear_report_value):
    case MAXWELL3D_REG_INDEX(fragment_barrier):
    case MAXWELL3D_REG_INDEX(invalidate_texture_data_cache):
    case MAXWELL3D_REG_INDEX(tiled_cache_barrier):
        return true; // Executable, with effects on the back only
    default:
        return method < MacroRegistersStart && !maxwell3d.execution_mask[method];
    }
}

} // Anonymous namespace

Maxwell3D::FrontState& Maxwell3D::Front(Core::System& system, VideoCommon::GpuSplit& split) {
    // Copied during an earlier start: while the split was stopped, state writes went to the
    // method sink without reaching it (they hand the front back only once consumed).
    if (front && front->split_start != split.Starts()) {
        split.Drain(VideoCommon::GpuSplit::DrainSite::FrontStart);
        ReleaseFront();
    }
    if (!front) [[unlikely]] {
        // Copy the back's state and take the macros over while it is idle (state writes it
        // still holds in its method sink first).
        split.Drain(VideoCommon::GpuSplit::DrainSite::FrontStart);
        ConsumeSink(system);
        front = std::make_unique<FrontState>();
        std::memcpy(front->regs.data(), regs.reg_array.data(), sizeof(front->regs));
        std::memcpy(front->shadow.data(), shadow_state.reg_array.data(), sizeof(front->shadow));
        front->control = shadow_state.shadow_ram_control;
        front->skip_same = SkipSameValueWrites() && FrontSkipSame();
        front->split_start = split.Starts();
        front->back_macros_enabled =
            Common::Nx2::Setting("EDEN_NX2_BACK_MACROS", "0").starts_with('1');
        front->defer_conditional =
            Common::Nx2::Setting("EDEN_NX2_DEFER_CONDITIONAL", "0").starts_with('1');
        front->executing_macro = executing_macro;
        front->parameters = std::move(macro_params);
        macro_params.clear();
        executing_macro = 0;
        front_macros = true;
    }
    return *front;
}

// With the split on, engine methods run on its back only: one on another thread means the split
// stopped (the accuracy settings changed mid-game). Hand the macros back to the engine, which
// otherwise drops the game's macro uploads and binds; a restart copies the state again (Front).
void Maxwell3D::ReleaseFront() {
    executing_macro = front->executing_macro;
    macro_params = std::move(front->parameters);
    // Entries macros the back ran may have written are the engine's already (FrontRunOnBack), and
    // so is a render enable mode it set from a query result (FrontRunConditionalRender).
    constexpr u32 render_enable_mode = MAXWELL3D_REG_INDEX(render_enable.mode);
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (front->back_shadow[i] == 0 &&
            !(i == render_enable_mode && front->render_enable_shadow_pending)) {
            shadow_state.reg_array[i] = front->shadow[i];
        }
    }
    shadow_state.shadow_ram_control = front->control;
    front.reset();
    front_macros = false;
    LOG_INFO(HW_GPU, "GPU thread split stopped: 3D macros back on the engine");
}

// The condition under which DmaPusher sends macro methods to FrontCallMethod.
bool Maxwell3D::MacroParkedOnFront(VideoCommon::GpuSplit& split) {
    return split.Active() && split.FrontMacros() && GetNativeMacroEngine().native_turing;
}

std::optional<Maxwell3D::ParkedMacro> Maxwell3D::TakeParkedMacro(std::vector<ParkedMacro>& parked,
                                                                 GPUVAddr key) {
    const auto it = std::ranges::find(parked, key, &ParkedMacro::key);
    if (it == parked.end()) {
        return std::nullopt;
    }
    ParkedMacro macro = std::move(*it);
    parked.erase(it);
    return macro;
}

void Maxwell3D::ParkMacro(Core::System& system, GPUVAddr key) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (MacroParkedOnFront(split)) {
        auto& fs = Front(system, split);
        fs.parked.push_back(ParkedMacro{key, std::exchange(fs.executing_macro, 0U),
                                        std::exchange(fs.parameters, {}), {}, false});
        return;
    }
    const auto park = [this, key] {
        parked_macros.push_back(ParkedMacro{key, std::exchange(executing_macro, 0U),
                                            std::exchange(macro_params, {}),
                                            std::exchange(macro_segments, {}),
                                            std::exchange(current_macro_dirty, false)});
    };
    if (split.Active()) {
        split.Defer(park, false);
    } else {
        park();
    }
}

void Maxwell3D::ResumeMacro(Core::System& system, GPUVAddr key) {
    static std::atomic<u32> reported{};
    auto& split = VideoCommon::GpuSplit::Get();
    if (MacroParkedOnFront(split)) {
        auto& fs = Front(system, split);
        if (auto macro = TakeParkedMacro(fs.parked, key)) {
            if (fs.executing_macro != 0 && reported.fetch_add(1) < 8) {
                LOG_WARNING(HW_GPU, "Resumed macro {:#x} replaces macro {:#x} with {} parameters",
                            macro->method, fs.executing_macro, fs.parameters.size());
            }
            fs.executing_macro = macro->method;
            fs.parameters = std::move(macro->parameters);
        }
        return;
    }
    const auto resume = [this, key] {
        auto macro = TakeParkedMacro(parked_macros, key);
        if (!macro) {
            return;
        }
        if (executing_macro != 0 && reported.fetch_add(1) < 8) {
            LOG_WARNING(HW_GPU, "Resumed macro {:#x} replaces macro {:#x} with {} parameters",
                        macro->method, executing_macro, macro_params.size());
        }
        executing_macro = macro->method;
        macro_params = std::move(macro->parameters);
        macro_segments = std::move(macro->segments);
        current_macro_dirty = macro->dirty;
    };
    if (split.Active()) {
        split.Defer(resume, false);
    } else {
        resume();
    }
}

void Maxwell3D::DropParkedMacro(Core::System& system, GPUVAddr key) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (MacroParkedOnFront(split)) {
        TakeParkedMacro(Front(system, split).parked, key);
        return;
    }
    const auto drop = [this, key] { TakeParkedMacro(parked_macros, key); };
    if (split.Active()) {
        split.Defer(drop, false);
    } else {
        drop();
    }
}

void Maxwell3D::FrontWrite(VideoCommon::GpuSplit& split, FrontState& fs, u32 method,
                           u32 argument, bool is_last_call, bool from_macro) {
    namespace Profile = VideoCommon::Nx2Profile;
    if (method == MAXWELL3D_REG_INDEX(render_enable.mode)) {
        FrontWriteRenderEnable(split, fs);
    }
    const u32 cb_offset = fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)];
    // As ProcessShadowRam does on the back. A register a macro the back ran may have written is
    // the front's again, and its write is sent even if the front's copies hold the value.
    u32 value = argument;
    bool shadow_kept = !(fs.back_owned[method] && FrontTakeOver(split, fs, method));
    if (fs.control == Regs::ShadowRamControl::Track ||
        fs.control == Regs::ShadowRamControl::TrackWithFilter) {
        shadow_kept = shadow_kept && fs.shadow[method] == argument;
        fs.shadow[method] = argument;
    } else if (fs.control == Regs::ShadowRamControl::Replay) {
        value = fs.shadow[method];
    }
    if (Profile::Enabled()) [[unlikely]] {
        Profile::Add(Profile::Counter::MethodWrites, 1);
    }
    if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && FrontVerifyEnabled()) [[unlikely]] {
        fs.control_writes[fs.control_count++ % fs.control_writes.size()] = {
            argument, static_cast<u32>(fs.control), from_macro ? 2U : 1U};
    }
    // From zortos293/Lichii#178 (tibzejoker): instance loop writes the back would ignore.
    if (execution_mask[method] && FrontSkipsLoopWrite(fs, method, value, shadow_kept)) {
        if (Profile::Enabled()) [[unlikely]] {
            Profile::Add(Profile::Counter::SameValueWrites, 1);
        }
        return;
    }
    if (!execution_mask[method]) {
        // Nothing would change on the back: CallMethod and ConsumeSinkImpl skip same-value state
        // writes, and the shadow RAM (tracked by default) holds the value already.
        if (fs.regs[method] == value && shadow_kept && fs.skip_same) {
            if (Profile::Enabled()) [[unlikely]] {
                Profile::Add(Profile::Counter::SameValueWrites, 1);
            }
            return;
        }
        fs.regs[method] = value;
    } else {
        fs.regs[method] = value;
        constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
        switch (method) {
        case MAXWELL3D_REG_INDEX(shadow_ram_control):
            // The back's mode is shadow_state.shadow_ram_control: the shadow copy of this very
            // register, which a later Replay write reads back.
            fs.control = static_cast<Regs::ShadowRamControl>(argument);
            fs.shadow[method] = argument;
            break;
        case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
            GetNativeMacroEngine().ClearCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)]);
            break;
        case MAXWELL3D_REG_INDEX(load_mme.instruction):
            GetNativeMacroEngine().AddCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)], value);
            break;
        case MAXWELL3D_REG_INDEX(load_mme.start_address): {
            const auto positions = GetNativeMacroPositions();
            u32& pointer = fs.regs[MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)];
            if (pointer < positions.size()) {
                positions[pointer] = value;
            }
            ++pointer;
            break;
        }
        case MAXWELL3D_REG_INDEX(falcon[4]):
            fs.regs[MAXWELL3D_REG_INDEX(shadow_scratch)] = 1;
            break;
        default:
            if (method >= cb_data && method < cb_data + 16) {
                fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += sizeof(u32);
            } else if (!FrontHandledOutsideDrawManager(method)) {
                fs.draw_pending = true;
            }
            break;
        }
    }
    if (VideoCommon::GpuSplit::RangesEnabled() && memory_write_mask[method]) {
        NoteFrontWriteRange(split, *reinterpret_cast<const Regs*>(fs.regs.data()), method,
                            cb_offset, 1, &value, memory_manager.GetID() + 1);
    }
    if (from_macro) {
        // Direct writes consume the sink first.
        split.MacroWrite(this, method, argument);
        fs.sink_pending = false;
    } else {
        split.Method(this, method, argument, is_last_call, nullptr);
        fs.sink_pending = !execution_mask[method];
    }
    if (method == MAXWELL3D_REG_INDEX(report_semaphore.query) && split.NotesQueuedReleases()) {
        NoteFrontReport(split, memory_manager, fs.regs.data(), value);
    }
}

void Maxwell3D::FrontCallMethod(Core::System& system, VideoCommon::GpuSplit& split, u32 method,
                                u32 argument, bool is_last_call) {
    auto& fs = Front(system, split);
    if (method >= MacroRegistersStart) {
        if (fs.executing_macro == 0) {
            fs.executing_macro = method;
        }
        fs.parameters.push_back(argument);
        if (is_last_call) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    FrontWrite(split, fs, method, argument, is_last_call, false);
}

void Maxwell3D::FrontCallMultiMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                     u32 method, const u32* base_start, u32 amount,
                                     u32 methods_pending) {
    auto& fs = Front(system, split);
    if (method >= MacroRegistersStart) {
        if (fs.executing_macro == 0) {
            fs.executing_macro = method;
        }
        fs.parameters.insert(fs.parameters.end(), base_start, base_start + amount);
        if (amount == methods_pending) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    if (method == MAXWELL3D_REG_INDEX(render_enable.mode) && amount != 0) {
        FrontWriteRenderEnable(split, fs);
    }
    // The instance loop state the back ends in (Replay: it acts on the shadow copy). The back
    // runs draw.begin once per word, so each word can switch the loop; other methods leave it
    // as their last word does.
    const bool replay = fs.control == Regs::ShadowRamControl::Replay;
    if (method == MAXWELL3D_REG_INDEX(draw.begin)) {
        for (u32 i = 0; i < amount; ++i) {
            FrontTrackInstanceLoop(fs, method, replay ? fs.shadow[method] : base_start[i]);
        }
    } else if (amount != 0) {
        FrontTrackInstanceLoop(fs, method, replay ? fs.shadow[method] : base_start[amount - 1]);
    }
    // What CallMultiMethod does to the registers on the back, then one entry for all the words
    // (sent whatever the front's copies hold).
    if (fs.back_owned[method]) {
        FrontTakeOver(split, fs, method);
    }
    constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
    const u32 cb_offset = fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)];
    const auto tracked = [&fs] {
        return fs.control == Regs::ShadowRamControl::Track ||
               fs.control == Regs::ShadowRamControl::TrackWithFilter;
    };
    if (method >= cb_data && method < cb_data + 16) {
        fs.regs[MAXWELL3D_REG_INDEX(const_buffer.offset)] += amount * sizeof(u32);
    } else if (method == MAXWELL3D_REG_INDEX(inline_data)) {
        // Upload data only.
    } else if ((method == MAXWELL3D_REG_INDEX(draw_inline_index) ||
                method == MAXWELL3D_REG_INDEX(inline_index_2x16.even) ||
                method == MAXWELL3D_REG_INDEX(inline_index_4x8.index0)) &&
               fs.control != Regs::ShadowRamControl::Replay) {
        if (amount != 0) {
            const u32 last = base_start[amount - 1];
            if (tracked()) {
                fs.shadow[method] = last;
            }
            fs.regs[method] = last;
        }
        fs.draw_pending = true;
    } else {
        // CallMethod for each word, macro uploads among them (load_mme.instruction). A word may
        // change the shadow RAM mode for the next (a run of SET_SHADOW_RAM_CONTROL writes).
        for (u32 i = 0; i < amount; ++i) {
            const u32 argument = base_start[i];
            u32 value = argument;
            if (method == MAXWELL3D_REG_INDEX(shadow_ram_control) && FrontVerifyEnabled()) [[unlikely]] {
                fs.control_writes[fs.control_count++ % fs.control_writes.size()] = {
                    argument, static_cast<u32>(fs.control), 3U};
            }
            if (tracked()) {
                fs.shadow[method] = argument;
            } else if (fs.control == Regs::ShadowRamControl::Replay) {
                value = fs.shadow[method];
            }
            fs.regs[method] = value;
            if (!execution_mask[method]) {
                continue;
            }
            switch (method) {
            case MAXWELL3D_REG_INDEX(shadow_ram_control):
                fs.control = static_cast<Regs::ShadowRamControl>(argument);
                fs.shadow[method] = argument; // The same word on the back (see FrontWrite)
                break;
            case MAXWELL3D_REG_INDEX(load_mme.instruction_ptr):
                GetNativeMacroEngine().ClearCode(value);
                break;
            case MAXWELL3D_REG_INDEX(load_mme.instruction):
                GetNativeMacroEngine().AddCode(fs.regs[MAXWELL3D_REG_INDEX(load_mme.instruction_ptr)], value);
                break;
            case MAXWELL3D_REG_INDEX(load_mme.start_address): {
                const auto positions = GetNativeMacroPositions();
                u32& pointer = fs.regs[MAXWELL3D_REG_INDEX(load_mme.start_address_ptr)];
                if (pointer < positions.size()) {
                    positions[pointer] = value;
                }
                ++pointer;
                break;
            }
            case MAXWELL3D_REG_INDEX(falcon[4]):
                fs.regs[MAXWELL3D_REG_INDEX(shadow_scratch)] = 1;
                break;
            default:
                if (!FrontHandledOutsideDrawManager(method)) {
                    fs.draw_pending = true;
                }
                break;
            }
        }
    }
    if (VideoCommon::GpuSplit::RangesEnabled() && memory_write_mask[method]) {
        // The back uploads the words as sent; only replayed shadow RAM is left out, to be safe.
        NoteFrontWriteRange(split, *reinterpret_cast<const Regs*>(fs.regs.data()), method,
                            cb_offset, amount,
                            fs.control != Regs::ShadowRamControl::Replay ? base_start : nullptr,
                            memory_manager.GetID() + 1);
    }
    split.MultiMethod(this, method, base_start, amount, methods_pending, nullptr);
    fs.sink_pending = false; // The back consumes the sink before a run.
    // Each word is a report to the same word: the last one decides what it holds.
    if (method == MAXWELL3D_REG_INDEX(report_semaphore.query) && amount != 0 &&
        split.AcquireQueued() != 0) {
        NoteFrontReport(split, memory_manager, fs.regs.data(), fs.regs[method]);
    }
}

void Maxwell3D::FrontRunMacro(Core::System& system, VideoCommon::GpuSplit& split,
                              FrontState& fs) {
#ifdef ARCHITECTURE_x86_64
    const u32 method = fs.executing_macro;
    fs.executing_macro = 0;
    if (const GPUVAddr arguments = std::exchange(fs.indirect_arguments, 0);
        arguments != 0 && fs.parameters.size() == 6) {
        // NVN's indirect draw macro (FrontTakeIndirectArguments) with the arguments left in GPU
        // memory. The macro writes base vertex and instance to their registers and to
        // c[0][0x640..0x647] around the draw, then zeros: the draw reads them as built-ins, and
        // only the zeros are sent.
        const u32 topology = fs.parameters[0];
        fs.parameters.clear();
        fs.draw_pending = false;
        split.Defer(
            [this, system_ptr = &system, topology, arguments] {
                DrawIndexedIndirectFromMemory(*system_ptr, topology, arguments);
            },
            false);
        constexpr u32 cb_data = u32(MAXWELL3D_REG_INDEX(const_buffer.buffer));
        constexpr std::array<std::pair<u32, u32>, 6> resets{{
            {u32(MAXWELL3D_REG_INDEX(vertex_id_base)), 0U},
            {u32(MAXWELL3D_REG_INDEX(global_base_vertex_index)), 0U},
            {u32(MAXWELL3D_REG_INDEX(global_base_instance_index)), 0U},
            {u32(MAXWELL3D_REG_INDEX(const_buffer.offset)), 0x640U},
            {cb_data, 0U},
            {cb_data + 1, 0U},
        }};
        for (const auto& [reset_method, value] : resets) {
            FrontWrite(split, fs, reset_method, value, true, true);
        }
        return;
    }
    auto& engine = GetNativeMacroEngine();
    const auto positions = GetNativeMacroPositions();
    const u32 start = positions[((method - MacroRegistersStart) >> 1) % positions.size()];
    if (!FrontRunConditionalRender(system, split, fs, method, start) &&
        !FrontRunOnBack(system, split, fs, engine, start)) {
        VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Macro};
        engine.native_interpreter.active_dram = &native_data_ram;
        if (!engine.native_jit) {
            engine.native_jit = std::make_shared<TuringMacroJit>();
        }
        engine.native_jit->Prepare(engine.NativeCode(), engine.native_code_generation);
        const size_t parameter_count = fs.parameters.size();
        const auto result = engine.native_jit->Execute(
            start, fs.parameters, std::span<u32>{native_data_ram},
            [&](u32 address) {
                if (address >= Regs::NUM_REGS) {
                    return 0U;
                }
                if (address == MAXWELL3D_REG_INDEX(render_enable.mode) &&
                    fs.render_enable_pending) {
                    FrontSyncRenderEnable(split, fs);
                }
                return fs.back_owned[address] ? FrontRegister(split, fs, address)
                                              : fs.regs[address];
            },
            [&](u32 address, u32 value) {
                if (address >= MacroRegistersStart) {
                    return false;
                }
                FrontWrite(split, fs, address, value, true, true);
                // MME_DMA_READ (into data RAM) and MME_DMA_READ_FIFOED (into the load FIFO).
                if (address == 0x157 || address == 0x158) {
                    FrontDmaRead(split, fs, method, address, value);
                }
                return true;
            });
        if (!result.success) {
            engine.native_execution_failed = true;
            static std::atomic<u64> logged{};
            if (Common::Log::ShouldLogRepeat(logged.fetch_add(1, std::memory_order_relaxed))) {
                LOG_ERROR(HW_GPU,
                          "Native MME (JIT, split front) failed: start={:#x}, consumed={}/{}, error={}",
                          start, result.consumed, parameter_count, result.error);
            }
        }
    }
    fs.parameters.clear();
    if (fs.draw_pending) {
        fs.draw_pending = false;
        // Draws: their writes are visible only after a host-level wait.
        split.Defer([this, system_ptr = &system] { FinishFrontMacro(*system_ptr); }, false,
                    VideoCommon::GpuSplit::FastDrawNeutral);
    }
    if (FrontVerifyEnabled() && (++fs.calls & 4095) == 0) [[unlikely]] {
        FrontVerify(split, fs);
    }
#else
    fs.executing_macro = 0;
    fs.parameters.clear();
#endif
}

// EDEN_NX2_BACK_MACROS: a macro whose code reads memory into its FIFO (MME_DMA_READ_FIFOED) and
// writes only registers BackWritable allows runs on the back, at its point of the method stream:
// it reads the words as a drained front would, and the front goes on without waiting for the work
// queued before it. The registers it can write (and, in a tracked shadow RAM mode, their entries)
// are the back's until the front writes them (FrontTakeOver) or needs them (FrontRegister).
// Returns whether the call went to the back.
bool Maxwell3D::FrontRunOnBack(Core::System& system, VideoCommon::GpuSplit& split, FrontState& fs,
                               const MacroEngine& engine, u32 start) {
    using DrainSite = VideoCommon::GpuSplit::DrainSite;
    using BackMacro = FrontState::BackMacro;
    if (!fs.back_macros_enabled || fs.parameters.size() > 0x1000) {
        return false;
    }
    // A macro the back ran wrote another register: the front's copies may be stale anywhere. It
    // runs on the front from now on, and the front takes the back's registers (FrontVerify).
    const auto take_escapes = [&] {
        fs.back_escapes_seen = fs.back_escapes.load(std::memory_order_acquire);
        for (const auto& macro : fs.back_macros) {
            if (macro && macro->back && macro->escaped.load(std::memory_order_relaxed)) {
                macro->back = false;
                fs.back_demoted.push_back(macro->start);
                LOG_ERROR(HW_GPU,
                          "Split back macro at {:#x} wrote outside the registers its code can "
                          "write; it runs on the front from now on",
                          macro->start);
            }
        }
        FrontVerify(split, fs);
    };
    if (fs.back_generation != engine.native_code_generation) {
        // The macros sent before run the code they were sent with, and check their records.
        split.WaitUntil(fs.back_last, DrainSite::BackCode);
        if (fs.back_escapes.load(std::memory_order_relaxed) != fs.back_escapes_seen) {
            take_escapes();
        }
        fs.back_macros.clear();
        fs.back_macros.resize(engine.NativeCode().size() / 3);
        fs.back_generation = engine.native_code_generation;
        fs.back_code_copied = false;
    } else if (fs.back_escapes.load(std::memory_order_relaxed) != fs.back_escapes_seen) {
        take_escapes();
    }
    if (start >= fs.back_macros.size()) {
        return false;
    }
    auto& known = fs.back_macros[start];
    if (!known) {
        known = std::make_unique<BackMacro>();
        known->start = start;
        const auto effects = TuringMacroInterpreter::AnalyzeEffects(engine.NativeCode(), start);
        const auto& methods = effects.methods;
        const bool fifo_read = std::ranges::binary_search(methods, 0x158U);
        if (fifo_read || std::ranges::binary_search(methods, 0x157U)) {
            std::string reason;
            if (std::ranges::find(fs.back_demoted, start) != fs.back_demoted.end()) {
                reason = "it wrote another register on the back before";
            } else if (!effects.methods_known) {
                reason = "it computes the method of a write";
            } else if (effects.data_ram) {
                reason = "it uses the data RAM";
            } else if (const auto it = std::ranges::find_if(
                           methods, [this](u32 m) { return !BackWritable(*this, m); });
                       it != methods.end()) {
                reason = fmt::format("it writes {:#x}", *it);
            }
            known->back = reason.empty() && fifo_read;
            if (known->back) {
                known->writes = methods;
                for (const u32 written : methods) {
                    known->write_set.set(written);
                }
            }
            static u32 logged{};
            if (logged < 32) {
                ++logged;
                LOG_INFO(HW_GPU, "Split macro at {:#x} reads memory and runs on the {}: {}", start,
                         known->back ? "back" : "front",
                         known->back ? fmt::format("writes {:#x}", fmt::join(methods, ","))
                                     : reason);
            }
        }
    }
    if (!known->back) {
        return false;
    }
    if (!fs.back_engine) {
        fs.back_engine.emplace(true);
        fs.back_engine->native_turing = true;
    }
    MacroEngine& back = *fs.back_engine;
    if (!fs.back_code_copied) {
        // Nothing sent with the old code is left to run (waited for above).
        back.native_code = engine.native_code;
        back.native_code_words = engine.native_code_words;
        back.native_code_generation = engine.native_code_generation;
        fs.back_code_copied = true;
    }
    split.DeferWords(
        [this, system_ptr = &system, engine_ptr = &back, macro = known.get(),
         escapes = &fs.back_escapes](std::span<const u32> parameters) {
            // As ProcessMacro calls a macro, with the back's engine.
            ConsumeSink(*system_ptr);
            engine_ptr->split_writes = &macro->write_set;
            const u64 unexpected = engine_ptr->split_unexpected_writes;
            engine_ptr->Execute(*system_ptr, *this, macro->start, parameters);
            if (engine_ptr->split_unexpected_writes != unexpected) {
                macro->escaped.store(true, std::memory_order_relaxed);
                escapes->fetch_add(1, std::memory_order_release);
            }
            if (VideoCommon::Nx2Profile::Enabled()) {
                VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::BackMacros, 1);
            }
        },
        fs.parameters);
    const u64 position = split.SentPosition();
    fs.back_last = position;
    const bool tracked = fs.control == Regs::ShadowRamControl::Track ||
                         fs.control == Regs::ShadowRamControl::TrackWithFilter;
    for (const u32 written : known->writes) {
        fs.back_regs[written] = position;
        if (tracked) {
            fs.back_shadow[written] = position;
        }
        fs.back_owned[written] = true;
    }
    return true;
}

// EDEN_NX2_BACK_MACROS: a register as the back holds it once it ran what the front sent: the
// front's copy, or the back's once it ran the last macro there that can write it. No entry sent
// after that macro writes the register (a front write takes it over first, a later macro there
// would be the one waited for), so the back's value stays until the front writes it again.
u32 Maxwell3D::FrontRegister(VideoCommon::GpuSplit& split, FrontState& fs, u32 method) {
    if (fs.back_regs[method] != 0) {
        split.WaitUntil(fs.back_regs[method], VideoCommon::GpuSplit::DrainSite::BackRegister);
        fs.regs[method] = regs.reg_array[method];
        fs.back_regs[method] = 0;
        fs.back_owned[method] = fs.back_shadow[method] != 0;
    }
    return fs.regs[method];
}

// EDEN_NX2_BACK_MACROS, before the front writes `method`: the register, and its shadow RAM entry
// unless the mode leaves that alone, are the front's again; a replay takes the entry from the back
// first (as FrontRegister). Returns whether the back may hold another value or entry than the
// front's copies, so that the write must be sent even if it seems to change nothing.
bool Maxwell3D::FrontTakeOver(VideoCommon::GpuSplit& split, FrontState& fs, u32 method) {
    using Control = Regs::ShadowRamControl;
    const bool tracked = fs.control == Control::Track || fs.control == Control::TrackWithFilter;
    if (fs.back_shadow[method] != 0 && fs.control == Control::Replay) {
        split.WaitUntil(fs.back_shadow[method], VideoCommon::GpuSplit::DrainSite::BackRegister);
        fs.shadow[method] = shadow_state.reg_array[method];
        fs.back_shadow[method] = 0;
    }
    const bool differs = fs.back_regs[method] != 0 || (tracked && fs.back_shadow[method] != 0);
    fs.back_regs[method] = 0;
    if (tracked) {
        fs.back_shadow[method] = 0;
    }
    fs.back_owned[method] = fs.back_shadow[method] != 0;
    return differs;
}

void Maxwell3D::FrontTrackInstanceLoop(FrontState& fs, u32 method, u32 value) {
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.begin):
        // Instance id, bits 27:26: First leaves an instance loop, Subsequent enters one.
        if (((value >> 26) & 3) == 0) {
            fs.instance_loop = false;
        } else if (((value >> 26) & 3) == 1) {
            fs.instance_loop = true;
        }
        break;
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
        fs.instance_loop = false; // DrawMode::InlineIndex: draw.end draws
        break;
    default:
        break;
    }
}

bool Maxwell3D::FrontSkipsLoopWrite(FrontState& fs, u32 method, u32 value, bool shadow_kept) {
    FrontTrackInstanceLoop(fs, method, value);
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.end):
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
        break;
    default:
        return false;
    }
    // Sent while state writes wait in the back's sink: it consumes them at the next executable
    // method, and a changed one ends a deferred draw that must issue before the next engine's work
    // (a compute dispatch after a dropped vertex_buffer.count would otherwise run first).
    if (!fs.skip_same || fs.sink_pending || !shadow_kept || fs.regs[method] != value) {
        return false;
    }
    // Instance loop methods flush no deferred draw, and the draw manager ignores these: DrawEnd
    // draws nothing in an instance loop, and it only reads the vertex buffer first and count.
    return method != MAXWELL3D_REG_INDEX(draw.end) || fs.instance_loop;
}

void Maxwell3D::FrontDmaRead(VideoCommon::GpuSplit& split, FrontState& fs, u32 macro,
                             u32 method, u32 count) {
    // The words may be written by methods still queued for the back (indirect draw arguments
    // from a culling dispatch), and the caches that may hold newer ones belong to it: the flush
    // below runs after everything queued before it.
    const GPUVAddr source = (GPUVAddr{FrontRegister(split, fs, 0x154) & 0x1ffffff} << 32) |
                            FrontRegister(split, fs, 0x155);
    const u32 ram = FrontRegister(split, fs, 0x156);
    const u32 words = std::min<u32>(count, 0x1000);
    const size_t read_bytes = size_t{words} * sizeof(u32);
    if (VideoCommon::Nx2Profile::Enabled()) {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::DmaReadBytes, read_bytes);
    }
    const auto read_words = [&] {
        for (u32 i = 0; i < words; ++i) {
            const u32 word = memory_manager.Read<u32>(source + u64{i} * sizeof(u32));
            if (method == 0x158) {
                fs.parameters.push_back(word);
            } else if (ram + i < native_data_ram.size()) {
                native_data_ram[ram + i] = word;
            }
        }
    };
    // The words read without the full drain, for EDEN_NX2_PERF_VERIFY=1 to compare with the ones
    // the full drain and flush give, which are used.
    std::vector<u32> overlap_words;
    const char* verified_path = "split_overlap";
    // Nx2Perf forward_reads: when the newest memory write sent is a constant buffer upload the
    // front sent itself, still queued for the back, the words it puts there come from it without
    // a wait (Frizti counted 9,600 such waits a second in Ocarina's forest). Pages a report or a
    // GMMU update may still write are left to the drain.
    const auto try_forward = [&](u32* out) {
        if (!VideoCommon::GpuSplit::ForwardEnabled() || words == 0 ||
            (source < Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed) &&
             Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed) < source + read_bytes) ||
            memory_manager.IsUpdateQueued(source, read_bytes) ||
            !memory_manager.IsFullyMappedRange(source, read_bytes)) {
            return false;
        }
        for (GPUVAddr page = source & ~GPUVAddr{0xfff}; page < source + read_bytes;
             page += 0x1000) {
            const auto device = memory_manager.GpuToCpuAddress(page);
            if (!device || Tegra::Nx2IsQueryPage(*device)) {
                return false;
            }
        }
        return split.ForwardRead(memory_manager.GetID() + 1, source, words, out);
    };
    {
        thread_local std::vector<u32> forwarded;
        forwarded.resize(words);
        if (try_forward(forwarded.data())) {
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::DmaReadsForwarded, 1);
            if (!VideoCommon::Nx2Perf::Verify()) {
                for (u32 i = 0; i < words; ++i) {
                    if (method == 0x158) {
                        fs.parameters.push_back(forwarded[i]);
                    } else if (ram + i < native_data_ram.size()) {
                        native_data_ram[ram + i] = forwarded[i];
                    }
                }
                return;
            }
            overlap_words = forwarded;
            verified_path = "forward_reads";
        }
    }
    // Nx2Perf split_overlap (or EDEN_NX2_DMA_RANGES=1): words on pages no GPU work and no query
    // ever wrote can't be newer in the caches. Once the work queued before the read that can write
    // them ran, guest memory has them: wait for that only, then look again, since that work may
    // have written the pages for the first time. Draws and other register writes after the last of
    // it run on meanwhile.
    if (VideoCommon::GpuSplit::OverlapEnabled() && overlap_words.empty() && words != 0) {
        const auto pages_clean = [&] {
            if (memory_manager.IsUpdateQueued(source, read_bytes)) {
                return false;
            }
            for (GPUVAddr page = source & ~GPUVAddr{0xfff}; page < source + read_bytes;
                 page += 0x1000) {
                const auto device = memory_manager.GpuToCpuAddress(page);
                if (!device || Tegra::Nx2IsGpuWritten(*device) || Tegra::Nx2IsQueryPage(*device)) {
                    return false;
                }
            }
            return true;
        };
        if (pages_clean()) {
            split.WaitForMemoryWritersIn(source, source + read_bytes,
                                         VideoCommon::GpuSplit::DrainSite::DmaRead);
            if (pages_clean()) {
                if (!VideoCommon::Nx2Perf::Verify()) {
                    read_words();
                    return;
                }
                overlap_words.resize(words);
                memory_manager.ReadBlockUnsafe(source, overlap_words.data(), read_bytes);
            }
        }
    }
    // EDEN_NX2_DMA_SNAPSHOT (Evil's EvilOOT dma-snapshot fix, via Frizti): Ocarina reads an
    // occlusion report for each object (~35,000 macro DMA reads a second) from GPU-written pages,
    // and each read drained the back. A drained read flushes its whole device pages and remembers
    // the split position; later reads of those pages skip the drain while nothing sent since may
    // write them and the query cache lists no report there. 2 also drains and compares.
    const u32 snapshot_mode = VideoCommon::GpuSplit::DmaSnapshotMode();
    std::optional<DAddr> snapshot_device;
    if (snapshot_mode != 0 && overlap_words.empty() && method == 0x158 && words != 0 &&
        !memory_manager.IsUpdateQueued(source, read_bytes)) {
        const auto device = memory_manager.GpuToCpuAddress(source);
        if (device &&
            memory_manager.GpuToCpuAddress(source + read_bytes - 1) == *device + read_bytes - 1) {
            snapshot_device = device;
            static u64 hits{};
            static u64 misses{};
            u64 since = ~u64{0};
            for (u64 page = *device >> 12; page <= (*device + read_bytes - 1) >> 12; ++page) {
                const auto it = fs.dma_snapshots.find(page);
                since = it == fs.dma_snapshots.end() ? 0 : (std::min)(since, it->second);
                if (since == 0) {
                    break;
                }
            }
            // A report's value is up to 8 bytes, listed by its first 4: look 12 bytes back. A
            // listed report may not be in guest memory yet.
            const DAddr reports_begin = *device - (std::min<DAddr>)(*device, 12);
            const bool hit = since != 0 &&
                             !split.MayBeWrittenSince(since, source, source + read_bytes) &&
                             !rasterizer->HasCachedReports(reports_begin,
                                                           *device + read_bytes - reports_begin);
            ++(hit ? hits : misses);
            if (((hits + misses) & 0x3ffff) == 0) {
                LOG_INFO(HW_GPU, "DMA snapshot: {} reads skipped the drain, {} drained", hits,
                         misses);
            }
            if (hit) {
                thread_local std::vector<u32> snapshot_words;
                snapshot_words.resize(words);
                memory_manager.ReadBlockUnsafe(source, snapshot_words.data(), read_bytes);
                if (snapshot_mode == 1) {
                    fs.parameters.insert(fs.parameters.end(), snapshot_words.begin(),
                                         snapshot_words.end());
                    return;
                }
                overlap_words = snapshot_words;
                verified_path = "dma_snapshot";
            }
        }
    }
    // The GPU data of the whole pages for a snapshot: a flush of only the bytes read leaves its
    // neighbours stale.
    const GPUVAddr flush_begin = snapshot_device ? source & ~GPUVAddr{0xfff} : source;
    const size_t flush_bytes =
        snapshot_device
            ? static_cast<size_t>(((source + read_bytes + 0xfff) & ~GPUVAddr{0xfff}) - flush_begin)
            : read_bytes;
    split.Defer([this, macro, method, source, read_bytes, flush_begin, flush_bytes] {
        FlushForMacroDmaRead(macro, method, source, read_bytes, flush_begin, flush_bytes);
    });
    split.Drain(VideoCommon::GpuSplit::DrainSite::DmaRead);
    if (snapshot_device) {
        if (fs.dma_snapshots.size() > 8192) {
            fs.dma_snapshots.clear();
        }
        for (u64 page = *snapshot_device >> 12; page <= (*snapshot_device + read_bytes - 1) >> 12;
             ++page) {
            fs.dma_snapshots[page] = split.SentPosition();
        }
    }
    if (!overlap_words.empty()) {
        u32 differing = 0;
        for (u32 i = 0; i < words; ++i) {
            differing += memory_manager.Read<u32>(source + u64{i} * sizeof(u32)) !=
                                 overlap_words[i]
                             ? 1
                             : 0;
        }
        static std::atomic<u32> reports{};
        if (differing != 0 && reports.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_ERROR(HW_GPU,
                      "Nx2Perf {}: MME DMA read {:#x} ({} words, macro {:#x}): {} words differ "
                      "after the full drain",
                      verified_path, source, words, macro, differing);
        }
    }
    read_words();
}

void Maxwell3D::FlushForMacroDmaRead(u32 macro, u32 method, GPUVAddr source, size_t read_bytes,
                                     GPUVAddr flush_begin, size_t flush_bytes) {
    VideoCommon::Nx2Profile::Scope flush_scope{VideoCommon::Nx2Profile::Zone::DmaReadFlush};
    // Occlusion reports too, as MacroEngine::Execute does (Mario Tennis Fever's characters).
    static const bool flush_queries = [] {
        const char* env = std::getenv("EDEN_NX2_DMA_READ_QUERY_FLUSH");
        return env == nullptr || env[0] != '0';
    }();
    const bool trace_queries =
        VideoCommon::QueryDma::TraceEnabled() &&
        VideoCommon::QueryDma::HasPendingQueries(memory_manager, *rasterizer, source, read_bytes);
    if (flush_queries && VideoCommon::QueryDma::WaitEnabled()) {
        VideoCommon::QueryDma::FlushPending(memory_manager, *rasterizer, source, read_bytes);
    } else if (flush_queries) {
        memory_manager.FlushRegion(source, read_bytes, VideoCommon::CacheType::QueryCache);
    }
    if (trace_queries) {
        VideoCommon::QueryDma::Trace(memory_manager, *rasterizer, macro, method, source,
                                     read_bytes, flush_queries);
    }
    if (memory_manager.IsMemoryDirty(flush_begin, flush_bytes,
                                     VideoCommon::CacheType::BufferCache)) {
        memory_manager.FlushRegion(flush_begin, flush_bytes, VideoCommon::CacheType::BufferCache);
    }
}

// EDEN_NX2_DEFER_CONDITIONAL: NVN2's conditional render macro takes [invert, address high,
// address low] and sets render_enable.mode to invert ^ (the 64-bit counts at address and
// address + 16 differ), an occlusion query's result. It reads them with MME_DMA_READ_FIFOED, which
// makes the front wait for the back to run everything queued; the back reads them at the same
// point of the method stream instead, after the same report and cache flushes. Macros that read
// the mode later wait for it then (FrontSyncRenderEnable). Taken before EDEN_NX2_BACK_MACROS's
// FrontRunOnBack: the front keeps the DMA registers it writes, and the back runs no macro.
bool Maxwell3D::FrontRunConditionalRender(Core::System& system, VideoCommon::GpuSplit& split,
                                          FrontState& fs, u32 macro, u32 start) {
    if (!fs.defer_conditional || fs.parameters.size() != 3) {
        return false;
    }
    auto& engine = GetNativeMacroEngine();
    if (start != fs.conditional_macro_start ||
        engine.native_code_generation != fs.conditional_macro_generation) {
        // FNV-1a of the macro's 9 instructions.
        constexpr u64 MacroHash = 0x14971503a81afe48ULL;
        constexpr size_t HashedWords = 27;
        const auto code = engine.NativeCode();
        const size_t first = size_t{start} * 3;
        u64 hash = 0xcbf29ce484222325ULL;
        for (size_t i = first; i < first + HashedWords && i < code.size(); ++i) {
            hash = (hash ^ code[i]) * 0x100000001b3ULL;
        }
        if (hash != MacroHash) {
            return false;
        }
        fs.conditional_macro_start = start;
        fs.conditional_macro_generation = engine.native_code_generation;
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 4) {
            LOG_INFO(HW_GPU, "Macro at {:#x} is the conditional render macro: read on the back",
                     start);
        }
    }
    const u32 invert = fs.parameters[0];
    const u32 address_high = fs.parameters[1];
    const u32 address_low = fs.parameters[2];
    fs.parameters.clear();
    // The macro's writes: the DMA source and word count, then the mode.
    constexpr u32 DmaReadAddressHigh = 0x154;
    constexpr u32 DmaReadAddressLow = 0x155;
    constexpr u32 DmaReadFifoed = 0x158;
    constexpr u32 ReadWords = 6;
    FrontWrite(split, fs, DmaReadAddressHigh, address_high, true, true);
    FrontWrite(split, fs, DmaReadAddressLow, address_low, true, true);
    FrontWrite(split, fs, DmaReadFifoed, ReadWords, true, true);
    const GPUVAddr address = (GPUVAddr{address_high & 0x1ffffff} << 32) | address_low;
    split.Defer([this, system_ptr = &system, macro, address, invert] {
        constexpr size_t read_bytes = ReadWords * sizeof(u32);
        FlushForMacroDmaRead(macro, DmaReadFifoed, address, read_bytes, address, read_bytes);
        const bool differ =
            memory_manager.Read<u32>(address) != memory_manager.Read<u32>(address + 16) ||
            memory_manager.Read<u32>(address + 4) != memory_manager.Read<u32>(address + 20);
        ConsumeSink(*system_ptr);
        CallMethod(*system_ptr, MAXWELL3D_REG_INDEX(render_enable.mode),
                   invert ^ (differ ? 1U : 0U), true);
    });
    fs.render_enable_pending = true;
    if (fs.control == Regs::ShadowRamControl::Track ||
        fs.control == Regs::ShadowRamControl::TrackWithFilter) {
        fs.render_enable_shadow_pending = true;
    }
    return true;
}

// Before a write to render_enable.mode lands in the front's copies: Replay writes the shadow
// value, which must be the back's.
void Maxwell3D::FrontWriteRenderEnable(VideoCommon::GpuSplit& split, FrontState& fs) {
    if (fs.control == Regs::ShadowRamControl::Replay) {
        if (fs.render_enable_shadow_pending) {
            FrontSyncRenderEnable(split, fs);
        }
    } else if (fs.control == Regs::ShadowRamControl::Track ||
               fs.control == Regs::ShadowRamControl::TrackWithFilter) {
        fs.render_enable_shadow_pending = false;
    }
    fs.render_enable_pending = false;
}

void Maxwell3D::FrontSyncRenderEnable(VideoCommon::GpuSplit& split, FrontState& fs) {
    constexpr u32 mode = MAXWELL3D_REG_INDEX(render_enable.mode);
    split.Drain(VideoCommon::GpuSplit::DrainSite::DmaRead);
    fs.regs[mode] = regs.reg_array[mode];
    fs.shadow[mode] = shadow_state.reg_array[mode];
    fs.render_enable_pending = false;
    fs.render_enable_shadow_pending = false;
}

void Maxwell3D::FrontVerify(VideoCommon::GpuSplit& split, FrontState& fs) {
    split.Drain(VideoCommon::GpuSplit::DrainSite::FrontVerify);
    if (fs.render_enable_pending || fs.render_enable_shadow_pending) {
        FrontSyncRenderEnable(split, fs);
    }
    static u64 checks{};
    static u64 differences{};
    ++checks;
    // State writes the back still holds in its method sink count as done.
    std::array<u32, Regs::NUM_REGS> expected;
    std::memcpy(expected.data(), regs.reg_array.data(), sizeof(expected));
    for (const auto& [method, value] : method_sink) {
        expected[method] = shadow_state.shadow_ram_control == Regs::ShadowRamControl::Replay
                               ? shadow_state.reg_array[method]
                               : value;
    }
    // What macros the back ran may have written, the front takes over (FrontRunOnBack).
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (fs.back_regs[i] != 0) {
            fs.regs[i] = expected[i];
        }
        if (fs.back_shadow[i] != 0) {
            fs.shadow[i] = shadow_state.reg_array[i];
        }
    }
    fs.back_regs.fill(0);
    fs.back_shadow.fill(0);
    fs.back_owned.reset();
    static u64 control_differences{};
    if ((fs.control != shadow_state.shadow_ram_control ||
         fs.shadow[MAXWELL3D_REG_INDEX(shadow_ram_control)] != shadow_state.reg_array[MAXWELL3D_REG_INDEX(shadow_ram_control)] ||
         fs.regs[MAXWELL3D_REG_INDEX(shadow_ram_control)] != expected[MAXWELL3D_REG_INDEX(shadow_ram_control)]) &&
        control_differences < 4) {
        LOG_WARNING(HW_GPU, "Split shadow RAM control writes, front ({} total) / back ({} total):",
                    fs.control_count, back_control_count);
        for (u32 i = 0; i < 16; ++i) {
            const auto& f = fs.control_writes[(fs.control_count + i) % 16];
            const auto& b = back_control_writes[(back_control_count + i) % 16];
            LOG_WARNING(HW_GPU, "  front {:#x} (mode {}, path {}) | back {:#x} (mode {}, last {})",
                        f.argument, f.before, f.path, b.argument, b.before, b.path);
        }
    }
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (fs.regs[i] != expected[i]) {
            if (differences++ < 64) {
                LOG_WARNING(HW_GPU, "Split front register {:#x}: front {:#x}, back {:#x}", i,
                            fs.regs[i], expected[i]);
            }
            fs.regs[i] = expected[i];
        }
    }
    if (fs.control != shadow_state.shadow_ram_control) {
        if (control_differences++ < 16) {
            LOG_WARNING(HW_GPU, "Split front shadow RAM control {} vs back {}",
                        static_cast<u32>(fs.control),
                        static_cast<u32>(shadow_state.shadow_ram_control));
        }
        fs.control = shadow_state.shadow_ram_control;
    }
    for (u32 i = 0; i < Regs::NUM_REGS; ++i) {
        if (fs.shadow[i] != shadow_state.reg_array[i]) {
            if (control_differences++ < 16) {
                LOG_WARNING(HW_GPU, "Split front shadow register {:#x}: front {:#x}, back {:#x}", i,
                            fs.shadow[i], shadow_state.reg_array[i]);
            }
            fs.shadow[i] = shadow_state.reg_array[i];
        }
    }
    if ((checks & 63) == 1) {
        LOG_INFO(HW_GPU, "Split front register checks: {} done, {} differences", checks, differences);
    }
}

void Maxwell3D::FinishFrontMacro(Core::System& system) {
    // What ProcessMacro and CallMacroMethod do around the macro.
    ConsumeSink(system);
    draw_manager.DrawDeferred(*this);
}

// NVN2's DrawElementsIndirect macro takes [topology, count, instances, first index, base vertex,
// base instance], the last five written by the GPU: title 1's culling dispatches fill them in
// the command list each frame. Read on the CPU, each call waited for the GPU to finish all the
// work queued so far (~8 times a frame, which kept CPU and GPU from overlapping); drawn from GPU
// memory instead, like HLE_DrawIndexedIndirect does for NVN1. EDEN_NX2_INDIRECT_DRAWS=0 reads
// them on the CPU and runs the macro.
bool Maxwell3D::FrontTakeIndirectArguments(Core::System& system, VideoCommon::GpuSplit& split,
                                           u32 method, GPUVAddr address, u32 words) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_INDIRECT_DRAWS");
        return value == nullptr || value[0] != '0';
    }();
    if (!enabled || words != 5 || method < MacroRegistersStart || (method & 1) == 0) {
        return false;
    }
    auto& fs = Front(system, split);
    if (fs.executing_macro != method - 1 || fs.parameters.size() != 1) {
        return false;
    }
    auto& engine = GetNativeMacroEngine();
    const auto positions = GetNativeMacroPositions();
    const u32 start = positions[((method - 1 - MacroRegistersStart) >> 1) % positions.size()];
    if (start != fs.indirect_macro_start ||
        engine.native_code_generation != fs.indirect_macro_generation) {
        // FNV-1a of the macro's first 16 instructions.
        constexpr u64 MacroHash = 0x04c0fa636a9d7c93ULL;
        constexpr size_t HashedWords = 48;
        const size_t first = size_t{start} * 3;
        u64 hash = 0xcbf29ce484222325ULL;
        for (size_t i = first; i < first + HashedWords && i < engine.native_code.size(); ++i) {
            hash = (hash ^ engine.native_code[i]) * 0x100000001b3ULL;
        }
        fs.indirect_macro_start = start;
        fs.indirect_macro_generation = engine.native_code_generation;
        fs.indirect_macro = hash == MacroHash;
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 4) {
            LOG_INFO(HW_GPU, "Macro at {:#x} takes GPU-written arguments (code {:#x}): {}", start,
                     hash, fs.indirect_macro ? "drawn indirectly" : "read on the CPU");
        }
    }
    if (!fs.indirect_macro) {
        return false;
    }
    fs.indirect_arguments = address;
    return true;
}

void Maxwell3D::DrawIndexedIndirectFromMemory(Core::System& system, u32 topology,
                                              GPUVAddr arguments) {
    ConsumeSink(system);
    draw_manager.DrawDeferred(*this);
    auto& params = draw_manager.indirect_state;
    params.is_byte_count = false;
    params.is_indexed = true;
    params.include_count = false;
    params.count_start_address = 0;
    params.indirect_start_address = arguments;
    params.buffer_size = 5 * sizeof(u32);
    params.max_draw_counts = 1;
    params.stride = 0;
    engine_state = EngineHint::OnHLEMacro;
    SetHLEReplacementAttributeType(0, 0x640, HLEReplacementAttributeType::BaseVertex);
    SetHLEReplacementAttributeType(0, 0x644, HLEReplacementAttributeType::BaseInstance);
    dirty.flags[VideoCommon::Dirty::IndexBuffer] = true;
    draw_manager.DrawIndexedIndirect(*this, static_cast<Regs::PrimitiveTopology>(topology), 0,
                                     static_cast<u32>(EstimateIndexBufferSize()));
    engine_state = EngineHint::None;
    replace_table.clear();
}

} // namespace Tegra::Engines
