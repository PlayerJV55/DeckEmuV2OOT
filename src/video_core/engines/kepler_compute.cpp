// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <bitset>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "common/assert.h"
#include "common/logging.h"
#include "common/nx2_title_defaults.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "shader_recompiler/exception.h"
#include "core/core.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/native_upscale.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/native_query_trace.h"
#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"
#endif
#include "video_core/rasterizer_interface.h"
#include "video_core/textures/decoders.h"

namespace Tegra::Engines {

namespace {

// Grid width a QMD gets in place of GPU-written grid sizes (FrontTakeIndirectArguments); the low
// byte is the slot in indirect_grids.
constexpr u32 IndirectGridMarker = 0x7e7e0000;
constexpr u32 IndirectGridMarkerMask = 0xffffff00;

} // Anonymous namespace

KeplerCompute::KeplerCompute(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_}
    , upload_state{memory_manager, regs.upload}
{
    // EDEN_NX2_QMD_TRACE: log the QMD scheduler's loads, dependency counts and launches.
    if (std::getenv("EDEN_NX2_QMD_TRACE")) {
        native_qmd.trace = [this](std::string_view event, u64 address, s32 value) {
            LOG_INFO(HW_GPU, "QMD trace {} {:#x} {} (engine {} as {})", event, address, value,
                     fmt::ptr(this), memory_manager.GetID());
        };
    }
    execution_mask.reset();
    execution_mask[KEPLER_COMPUTE_REG_INDEX(exec_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(data_upload)] = true;
    execution_mask[KEPLER_COMPUTE_REG_INDEX(launch)] = true;
    // Everything ProcessNativeMethod acts on besides storing the value; the QMD and 0x5a
    // trace range too while native draw diagnostics trace it.
    native_plain_methods.set();
    for (const u32 method : {0x45U, 0x46U, 0x47U, 0x48U, 0xaeU, 0xafU, 0xb0U, 0x107U, 0x6c3U,
                             static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(exec_upload)),
                             static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(data_upload))}) {
        native_plain_methods[method] = false;
    }
    if (std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr) {
        native_plain_methods[0x5a] = false;
        for (u32 method = 0xc6; method <= 0x108; ++method) {
            native_plain_methods[method] = false;
        }
    }
}

KeplerCompute::~KeplerCompute() = default;

void KeplerCompute::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
    upload_state.BindRasterizer(rasterizer);
}

void KeplerCompute::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        if (method < Regs::NUM_REGS) {
            regs.reg_array[method] = value;
        }
    }
    method_sink.clear();
}

// SET_REPORT_SEMAPHORE_A-D (0x6c0-0x6c3): address high/low, payload, then operation in bits 1:0
// (release / acquire / report only / trap), reduction enable bit 3 with the op in bits 11:9, and
// a one-word (no timestamp) structure in bit 28. NVN2 releases queue syncs from compute this way.
void KeplerCompute::ProcessNativeReportSemaphore(u32 argument) {
    const GPUVAddr address = (GPUVAddr{native_registers[0x6c0] & 0x1ffffff} << 32) |
                             native_registers[0x6c1];
    const u32 payload = native_registers[0x6c2];
    const u32 operation = argument & 3;
    const bool one_word = (argument >> 28) & 1;
    static u32 logged{};
    if (logged++ < 32) {
        LOG_INFO(HW_GPU, "Native compute report semaphore {:#x} = {:#x}, control {:#x}", address,
                 payload, argument);
    }
    if (operation != 0 && operation != 2) {
        LOG_WARNING(HW_GPU, "Native compute report semaphore operation {} not supported", operation);
        return;
    }
    if ((argument >> 3) & 1) {
        const u32 reduction = (argument >> 9) & 7;
        rasterizer->SignalFence([this, address, payload, reduction] {
            const u32 old_value = memory_manager.Read<u32>(address);
            const u32 value = reduction == 0   ? old_value + payload
                              : reduction == 1 ? (std::min)(old_value, payload)
                              : reduction == 2 ? (std::max)(old_value, payload)
                              : reduction == 3 ? (old_value >= payload ? 0 : old_value + 1)
                              : reduction == 4 ? (old_value == 0 || old_value > payload ? payload : old_value - 1)
                              : reduction == 5 ? (old_value & payload)
                              : reduction == 6 ? (old_value | payload)
                                               : (old_value ^ payload);
            memory_manager.Write<u32>(address, value);
        });
        return;
    }
    rasterizer->Query(address, VideoCommon::QueryType::Payload,
                      one_word ? VideoCommon::QueryPropertiesFlags::IsAFence
                               : VideoCommon::QueryPropertiesFlags::HasTimeout,
                      payload, 0);
}

bool KeplerCompute::ProcessNativeMethod(Core::System& system, u32 method, u32 argument, bool is_last_call) {
    if (native_context && !front_macros) {
        if (method == 0x45 || method == 0x47) native_compute_uploaded = true;
        native_macro = native_compute_uploaded ? &native_context->compute_engine : &native_context->engine;
        native_macro_positions = native_compute_uploaded ? std::span<u32>{native_context->compute_positions} : std::span<u32>{native_context->positions};
    }
    if (method >= native_registers.size()) {
        static std::atomic<u64> logged{};
        if (Common::Log::ShouldLogRepeat(logged.fetch_add(1, std::memory_order_relaxed))) {
            LOG_ERROR(HW_GPU, "Native compute method out of range: {:#x}", method);
        }
        return true;
    }
    if (method >= 0xe00) {
        if (!native_executing_macro) {
            if (method & 1) { LOG_ERROR(HW_GPU, "Native compute macro starts on argument method"); return true; }
            native_executing_macro = method;
        }
        native_parameters.push_back(argument);
        if (!is_last_call) return true;
        const u32 entry = ((*native_executing_macro - 0xe00) / 2) % native_macro_positions.size();
        native_executing_macro_logged = native_executing_macro;
        native_executing_macro.reset();
        static const u32 watch = [] {
            const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
            return value ? static_cast<u32>(std::strtoull(value, nullptr, 16)) : 0U;
        }();
        const bool watched = watch != 0 && std::ranges::any_of(native_parameters,
            [](u32 parameter) { return (parameter & ~0xfU) == watch; });
        if (watched) {
            LOG_INFO(HW_GPU, "Semaphore watch: compute macro start={:#x} parameters={:08x}",
                     native_macro_positions[entry], fmt::join(native_parameters, ","));
        }
        // EDEN_NX2_MME_DUMP: write the compute macro code once per engine, and log each macro
        // call with its entry and parameters (diagnostics).
        if (static const bool mme_dump = std::getenv("EDEN_NX2_MME_DUMP") != nullptr; mme_dump) {
            if (!native_mme_dumped && native_macro_positions[entry] == 0xda) {
                native_mme_dumped = true;
                std::ofstream dump(fmt::format("native-compute-mme-{}.bin", fmt::ptr(this)),
                                   std::ios::binary);
                dump.write(reinterpret_cast<const char*>(native_macro->native_code.data()),
                           sizeof(native_macro->native_code));
            }
            LOG_INFO(HW_GPU, "MME call engine {} method {:#x} start={:#x} parameters={:08x}",
                     fmt::ptr(this), *native_executing_macro_logged, native_macro_positions[entry],
                     fmt::join(native_parameters, ","));
        }
        if (native_data_ram) native_macro->native_interpreter.active_dram = native_data_ram;
        const u32 dram0_before = (*native_macro->native_interpreter.active_dram)[0x27];
        const u32 start = native_macro_positions[entry];
        const auto emit = [&](u32 address, u32 value) {
            static u32 diagnostic_writes{};
            static const bool log_emits = std::getenv("EDEN_NX2_SEM_WATCH_EMITS") != nullptr;
            if (watched && log_emits) LOG_INFO(HW_GPU, "Semaphore watch: compute macro emit {:#x} = {:#x}", address, value);
            if (diagnostic_writes++ < 4096) LOG_DEBUG(HW_GPU, "Native compute MME emit method={:#x}, value={:#x}", address, value);
            if (address >= 0xe00) return false;
            VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
            CallMethod(system, address, value, true);
            return true;
        };
#ifdef ARCHITECTURE_x86_64
        // Compiled macro code, as for 3D macros (macro_turing_jit.h); EDEN_NX2_MME_JIT=0 off.
        static const bool use_jit = [] {
            const char* value = std::getenv("EDEN_NX2_MME_JIT");
            return value == nullptr || value[0] != '0';
        }();
        if (use_jit && !watched) {
            if (!native_macro->native_jit) {
                native_macro->native_jit = std::make_shared<TuringMacroJit>();
            }
            native_macro->native_jit->Prepare(native_macro->NativeCode(),
                                              native_macro->native_code_generation);
            const auto result = native_macro->native_jit->Execute(
                start, native_parameters,
                std::span<u32>{*native_macro->native_interpreter.active_dram},
                [&](u32 address) { return address < native_registers.size() ? native_registers[address] : 0U; },
                emit);
            if (!result.success) {
                native_macro->native_execution_failed = true;
                LOG_ERROR(HW_GPU, "Native MME (JIT) failed: compute start={:#x}, consumed={}/{}, error={}",
                          start, result.consumed, native_parameters.size(), result.error);
            }
            native_parameters.clear();
            return true;
        }
#endif
        // Repeated calls replay their writes, as for 3D macros (macro_memo.h).
        static const bool use_memo = [] {
            const char* value = std::getenv("EDEN_NX2_MME_MEMO");
            return value == nullptr || value[0] != '0';
        }();
        auto& memo = native_macro->native_memo;
        if (use_memo && !watched) {
            memo.Synchronize(native_macro->native_code_generation);
            if (std::remove_reference_t<decltype(memo)>::Effects effects;
                memo.Find(start, native_parameters, *native_macro->native_interpreter.active_dram,
                          effects)) {
                auto& data_ram = *native_macro->native_interpreter.active_dram;
                for (size_t i = 0; i < effects.data_ram_writes.size(); i += 2) {
                    data_ram[effects.data_ram_writes[i]] = effects.data_ram_writes[i + 1];
                }
                for (size_t i = 0; i < effects.writes.size(); i += 2) {
                    emit(effects.writes[i], effects.writes[i + 1]);
                }
                native_parameters.clear();
                return true;
            }
            native_macro->native_writes.clear();
        }
        // The interpreter consumes native_parameters as its FIFO: keep the call's own parameters.
        native_memo_parameters.assign(native_parameters.begin(), native_parameters.end());
        const u64 code_generation = native_macro->native_code_generation;
        const auto result = native_macro->native_interpreter.Execute(native_macro->NativeCode(),
            start, native_parameters,
            [&](u32 address) { return address < native_registers.size() ? native_registers[address] : 0U; },
            [&](u32 address, u32 value) {
                if (use_memo) {
                    native_macro->native_writes.emplace_back(address, value);
                }
                return emit(address, value);
            });
        const auto& interpreter = native_macro->native_interpreter;
        if (use_memo && !watched && result.success && !interpreter.read_registers &&
            !interpreter.reread_data_ram && code_generation == native_macro->native_code_generation) {
            memo.Store(start, native_memo_parameters, interpreter.data_ram_reads,
                       interpreter.data_ram_writes, native_macro->native_writes);
        }
        if (static const bool mme_dump = std::getenv("EDEN_NX2_MME_DUMP") != nullptr;
            mme_dump && (*native_macro->native_interpreter.active_dram)[0x27] != dram0_before) {
            LOG_INFO(HW_GPU, "MME data RAM[0x27] {:#x} -> {:#x} by engine {} macro start={:#x}",
                     dram0_before, (*native_macro->native_interpreter.active_dram)[0x27], fmt::ptr(this),
                     native_macro_positions[entry]);
        }
        if (!result.success) {
            native_macro->native_execution_failed = true;
            if (static const bool diagnostics = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
                diagnostics) {
                std::ofstream dump("native-compute-mme.bin", std::ios::binary);
                dump.write(reinterpret_cast<const char*>(native_macro->native_code.data()), sizeof(native_macro->native_code));
            }
            LOG_ERROR(HW_GPU, "Native MME failed: compute start={:#x}, pc={:#x}, consumed={}/{}, error={}, parameters={:08x}",
                native_macro_positions[entry], result.pc, result.consumed, native_parameters.size(), result.error,
                fmt::join(native_parameters, ","));
        }
        native_parameters.clear();
        return true;
    }
    native_registers[method] = argument;
    if (method == 0x6c3) { // SET_REPORT_SEMAPHORE_D: the compute class's semaphore release
        ProcessNativeReportSemaphore(argument);
        return true;
    }
    if (method == 0x5a || (method >= 0xc6 && method <= 0x108)) {
        VideoCommon::TraceNativeQuery(12, memory_manager.GetID(), method, 0, 0, argument, 0);
    }
    if (method == 0x107) {
        const GPUVAddr address = ((GPUVAddr{native_registers[0xc6]} << 32) |
            native_registers[0xc7]) << 8;
        const GPUVAddr program = (GPUVAddr{native_registers[0xc8 + 49] & 0x1ffff} << 32) |
            native_registers[0xc8 + 48];
        const u32 control = native_registers[0xa7];
        LOG_DEBUG(HW_GPU, "Native inline QMD: address={:#x}, program={:#x}, control={:#x}, grid={}/{}/{}",
            address, program, control, native_registers[0xc8 + 12],
            native_registers[0xc8 + 13] & 0xffff, native_registers[0xc8 + 14] & 0xffff);
        if (static const bool diagnostics = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
            diagnostics) {
            std::ofstream dump("native-inline-qmd.bin", std::ios::binary);
            dump.write(reinterpret_cast<const char*>(native_registers.data() + 0xc8), 256);
            std::ofstream registers("native-inline-compute-registers.bin", std::ios::binary);
            registers.write(reinterpret_cast<const char*>(native_registers.data()), sizeof(native_registers));
            if (memory_manager.IsFullyMappedRange(program, 0x10000)) {
                std::vector<u8> code(0x10000);
                memory_manager.ReadBlockUnsafe(program, code.data(), code.size());
                std::ofstream shader("native-inline-compute-shader.bin", std::ios::binary);
                shader.write(reinterpret_cast<const char*>(code.data()), code.size());
            }
        }
        if (!memory_manager.IsFullyMappedRange(address, 256)) {
            native_macro->native_execution_failed = true;
            LOG_ERROR(HW_GPU, "Unmapped native inline QMD destination");
            return true;
        }
        rasterizer->AccelerateInlineToMemory(address, 256,
            std::span<const u8>{reinterpret_cast<const u8*>(native_registers.data() + 0xc8), 256});
        if ((control & (1U << 24)) || !(native_registers[0xcc] & (1U << 12))) {
            const u32 previous = native_registers[0xad];
            native_registers[0xad] = static_cast<u32>(address >> 8);
            ProcessNativeMethod(system, 0xb0, 3, true);
            native_registers[0xad] = previous;
        }
        return true;
    }
    if (front_macros && method >= 0x45 && method <= 0x48) {
        return true; // Loaded and bound by the split GPU thread's front (FrontApply).
    }
    switch (method) {
    case 0x45: native_macro->ClearCode(argument); return true;
    case 0x46: native_macro->AddCode(0, argument); return true;
    case 0x47: native_bind_pointer = argument; return true;
    case 0x48:
        if (native_bind_pointer < native_macro_positions.size()) native_macro_positions[native_bind_pointer++] = argument;
        else LOG_ERROR(HW_GPU, "Native compute macro bind out of bounds");
        return true;
    case 0xae: case 0xaf: case 0xb0: {
        const GPUVAddr qmd_address = GPUVAddr{native_registers[0xad]} << 8;
        VideoCommon::TraceNativeQuery(9, memory_manager.GetID(), qmd_address,
            memory_manager.GpuToCpuAddress(qmd_address).value_or(0), method, argument, 0);
        if (method == 0xae || (method == 0xaf && !(argument & 2)) ||
            (method == 0xb0 && ((argument & 15) == 0 || (argument & 15) == 1))) return true;
        static const bool qmd_diagnostics = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
        if ((argument & 15) == 12 && qmd_diagnostics) {
            static u32 captured{};
            if (captured++ == 0 && memory_manager.IsFullyMappedRange(qmd_address, 256)) {
                std::array<u32, 64> descriptor{};
                memory_manager.ReadBlockUnsafe(qmd_address, descriptor.data(), sizeof(descriptor));
                std::ofstream dump("native-increment-qmd.bin", std::ios::binary);
                dump.write(reinterpret_cast<const char*>(descriptor.data()), sizeof(descriptor));
                LOG_WARNING(HW_GPU, "Native increment dependency descriptor: address={:#x}, words={:08x}",
                    qmd_address, fmt::join(descriptor, ","));
            }
        }
        const auto error = native_qmd.Signal(qmd_address, argument,
            [&](u64 address, NativeQmdScheduler::Descriptor& descriptor) {
                if (!memory_manager.IsFullyMappedRange(address, sizeof(descriptor))) return false;
                memory_manager.ReadBlockUnsafe(address, descriptor.data(), sizeof(descriptor));
                if (static u32 logged{}; (descriptor[53] & (1U << 23)) && logged < 8) {
                    ++logged;
                    LOG_INFO(HW_GPU, "Native QMD {:#x} releases: {:08x} | {:08x} | {:08x}", address,
                             fmt::join(std::span{descriptor}.subspan(24, 4), ","),
                             fmt::join(std::span{descriptor}.subspan(28, 4), ","),
                             fmt::join(std::span{descriptor}.subspan(52, 4), ","));
                }
                for (u32 slot = 0; slot < 2; ++slot) {
                    const u32 flags = descriptor[25 + slot * 4];
                    if (!(flags & (1U << 23))) continue;
                    const GPUVAddr destination = (GPUVAddr{flags & 0xff} << 32) | descriptor[24 + slot * 4];
                    VideoCommon::TraceNativeQuery(7 + slot, memory_manager.GetID(), destination,
                        memory_manager.GpuToCpuAddress(destination).value_or(0),
                        descriptor[4], descriptor[26 + slot * 4], flags);
                }
                return true;
            },
            [&](u64 address, u32 payload, u32 flags) {
                const bool one_word = (flags & (1U << 30)) != 0;
                if (!memory_manager.IsFullyMappedRange(address, one_word ? sizeof(payload) : 16)) return false;
                if (flags & (1U << 26)) {
                    rasterizer->SignalFence([this, address, payload] {
                        const u32 value = memory_manager.Read<u32>(address) + payload;
                        memory_manager.Write<u32>(address, value);
                        if (const auto device = memory_manager.GpuToCpuAddress(address)) {
                            Tegra::Nx2SemaphoreWaiters::Get().OnWrite(*device, value);
                        }
                        static const u64 watch = [] {
                            const char* text = std::getenv("EDEN_NX2_SEM_WATCH");
                            return text ? u64{std::strtoull(text, nullptr, 16)} : u64{};
                        }();
                        if (watch != 0 && (address & ~u64{0xf}) == watch) {
                            LOG_INFO(HW_GPU, "Semaphore watch: QMD release add {:#x} -> {:#x}", address, value);
                        }
                        VideoCommon::TraceNativeQuery(1, memory_manager.GetID(), address,
                            memory_manager.GpuToCpuAddress(address).value_or(0), 0, value, 1U << 26);
                    });
                } else if (flags & (1U << 19)) {
                    // NVN2 tracks QMD completion with a max release (bit 19) into a word other
                    // releases also advance: a plain write would move it backwards and leave
                    // GEQ acquires waiting forever.
                    rasterizer->SignalFence([this, address, payload] {
                        const u32 value = (std::max)(memory_manager.Read<u32>(address), payload);
                        memory_manager.Write<u32>(address, value);
                        if (const auto device = memory_manager.GpuToCpuAddress(address)) {
                            Tegra::Nx2SemaphoreWaiters::Get().OnWrite(*device, value);
                        }
                    });
                } else {
                    static const u64 watch = [] {
                        const char* text = std::getenv("EDEN_NX2_SEM_WATCH");
                        return text ? u64{std::strtoull(text, nullptr, 16)} : u64{};
                    }();
                    if (watch != 0 && (address & ~u64{0xf}) == (watch & ~u64{0xf})) {
                        LOG_INFO(HW_GPU, "Semaphore watch: QMD release {:#x} = {:#x} flags {:#x}", address, payload, flags);
                    }
                    rasterizer->Query(address, VideoCommon::QueryType::Payload,
                        one_word ? VideoCommon::QueryPropertiesFlags::IsAFence
                                 : VideoCommon::QueryPropertiesFlags::IsAFence |
                                       VideoCommon::QueryPropertiesFlags::HasTimeout,
                        payload, 0);
                }
                return true;
            }, [&](const auto& descriptor) { return DispatchNativeCompute(descriptor); });
        if (error.empty()) {
            LOG_DEBUG(HW_GPU, "Native QMD control action={:#x}, QMD={:#x}", argument, qmd_address);
            return true;
        }
        native_macro->native_execution_failed = true;
        std::array<u32, 128> qmd{};
        if (memory_manager.IsFullyMappedRange(qmd_address, sizeof(qmd))) {
            memory_manager.ReadBlockUnsafe(qmd_address, qmd.data(), sizeof(qmd));
        }
        // Log a failing program's first few dispatches only (QMD words 48-49 hold its code
        // address); it fails the same way every frame.
        static std::unordered_map<u64, u32> failures;
        const u64 program = (u64{qmd[49]} << 32) | qmd[48];
        if (failures[program ^ std::hash<std::string>{}(std::string{error})]++ >= 2) {
            return true;
        }
        LOG_ERROR(HW_GPU, "Native QMD failed: {}", error);
        if (qmd_diagnostics && memory_manager.IsFullyMappedRange(qmd_address, sizeof(qmd))) {
            std::ofstream dump("native-qmd.bin", std::ios::binary);
            dump.write(reinterpret_cast<const char*>(qmd.data()), sizeof(qmd));
            std::ofstream macros("native-dispatch-mme.bin", std::ios::binary);
            macros.write(reinterpret_cast<const char*>(native_macro->native_code.data()), sizeof(native_macro->native_code));
            std::ofstream registers("native-dispatch-registers.bin", std::ios::binary);
            registers.write(reinterpret_cast<const char*>(native_registers.data()), sizeof(native_registers));
        }
        LOG_ERROR(HW_GPU, "Native compute dispatch unsupported: method={:#x}, value={:#x}, QMD={:#x}, words={:08x}", method, argument,
                  qmd_address, fmt::join(qmd, ","));
        return true;
    }
    default:
        // The inline upload register block is shared with the older class.
        if (method < Regs::NUM_REGS) regs.reg_array[method] = argument;
        return method != KEPLER_COMPUTE_REG_INDEX(exec_upload) && method != KEPLER_COMPUTE_REG_INDEX(data_upload);
    }
}

void KeplerCompute::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    if (front_macros && !VideoCommon::GpuSplit::OnBackThread()) {
        ReleaseFront();
    }
    if (native_mode) {
        // Most of the compute class's register writes (QMD fields replayed by its macros for
        // each of thousands of dispatches a second) only store the value: ~4% of title 1's GPU
        // thread went through ProcessNativeMethod's checks for them.
        if (method < native_plain_methods.size() && native_plain_methods[method]) {
            native_registers[method] = method_argument;
            if (method < Regs::NUM_REGS) {
                regs.reg_array[method] = method_argument;
            }
            return;
        }
        if (ProcessNativeMethod(system, method, method_argument, is_last_call)) return;
    }
    if (method >= Regs::NUM_REGS) {
        LOG_ERROR(HW_GPU, "Compute register out of range: {:#x}", method);
        return;
    }

    regs.reg_array[method] = method_argument;

    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(exec_upload): {
        // Only the older class's launch (below) reads and clears the list. Native mode launches
        // through its QMD (0xaf in ProcessNativeMethod) and never gets there: the list grew by
        // an entry per inline upload for the whole session, with ever larger reallocations.
        if (!native_mode) {
            UploadInfo info{.upload_address = upload_address,
                            .exec_address = upload_state.ExecTargetAddress(),
                            .copy_size = upload_state.GetUploadSize(),
                            .was_dirty = upload_dirty};
            uploads.push_back(info);
        }
        upload_state.ProcessExec(regs.exec_upload.linear != 0);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(data_upload): {
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(method_argument, is_last_call);
        break;
    }
    case KEPLER_COMPUTE_REG_INDEX(launch): {
        const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();

        for (auto& data : uploads) {
            const GPUVAddr offset = data.exec_address - launch_desc_loc;
            if (offset / sizeof(u32) == LAUNCH_REG_INDEX(grid_dim_x)) {
                const bool source_dirty = memory_manager.IsMemoryDirty(data.upload_address, data.copy_size);
                if (data.was_dirty || source_dirty) {
                    indirect_compute = {data.upload_address};
                }
            }
        }
        uploads.clear();
        ProcessLaunch();
        indirect_compute = std::nullopt;
        break;
    }
    default:
        break;
    }
}

void KeplerCompute::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    if (native_mode) {
        for (u32 i = 0; i < amount; ++i) CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        return;
    }
    switch (method) {
    case KEPLER_COMPUTE_REG_INDEX(data_upload):
        upload_address = current_dma_segment;
        upload_dirty = current_dirty;
        current_dirty = false;
        upload_state.ProcessData(base_start, amount);
        return;
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

bool KeplerCompute::DispatchNativeCompute(const NativeQmdScheduler::Descriptor& qmd) {
    launch_description = {};
    launch_description.grid_dim_x.Assign(qmd[12]);
    launch_description.grid_dim_y.Assign(qmd[13] & 0xffff);
    launch_description.grid_dim_z.Assign(qmd[14] & 0xffff);
    launch_description.shared_alloc.Assign(qmd[17] & 0x3ffff);
    launch_description.block_dim_x.Assign(qmd[18] >> 16);
    launch_description.block_dim_y.Assign(qmd[19] & 0xffff);
    launch_description.block_dim_z.Assign(qmd[19] >> 16);
    launch_description.const_buffer_enable_mask.Assign(qmd[20] & 0xff);
    launch_description.local_pos_alloc.Assign(qmd[23] & 0xffffff);
    launch_description.local_crs_alloc.Assign(qmd[50] & 0xffffff);
    regs.code_loc.address_low = qmd[48];
    regs.code_loc.address_high = qmd[49] & 0x1ffff;
    for (u32 slot = 0; slot < NumConstBuffers; ++slot) {
        const u32 high = qmd[33 + slot * 2];
        if (high & 0x1ff00) {
            LOG_ERROR(HW_GPU, "Native compute constant address exceeds supported GPU width");
            return false;
        }
        auto& buffer = launch_description.const_buffer_config[slot];
        buffer.address_low = qmd[32 + slot * 2];
        buffer.address_high.Assign(high & 0xff);
        buffer.size.Assign(((high >> 19) & 0x1fff) << 4);
    }
    if (!launch_description.block_dim_x || !launch_description.block_dim_y || !launch_description.block_dim_z)
        return false;
    if ((qmd[12] & IndirectGridMarkerMask) == IndirectGridMarker) {
        const GPUVAddr grid = indirect_grids[qmd[12] & ~IndirectGridMarkerMask];
        if (grid == 0) {
            // Never dispatch the marker itself (over two billion groups).
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_ERROR(HW_GPU, "Compute grid marker {:#x} without a grid address", qmd[12]);
            }
            return true;
        }
        indirect_compute = grid;
        indirect_compute_gpu_written = true;
    }
    SCOPE_EXIT {
        indirect_compute = std::nullopt;
        indirect_compute_gpu_written = false;
    };
    // Fortune's Weave's DLSS evaluation runs from its first pass to the pass that writes the
    // upscaled image. Only followed in titles known to use DLSS (UsesGameDlss).
    if (UsesGameDlss() && RememberDlssInputs()) {
        dlss_chain.active = true;
        dlss_chain.rejected = 0;
        dlss_chain.dispatches = 0;
        dlss_chain.pending.clear();
    }
    const bool output_pass = dlss_chain.active && IsDlssOutputPass();
    // Fortune's Weave's evaluation is 13 dispatches. Without an output pass by then, the first
    // pass was another program with a similar constant buffer: stop following it. Skipped
    // dispatches count too, so a chain always ends.
    constexpr u32 max_dispatches = 64;
    if (dlss_chain.active && !output_pass && ++dlss_chain.dispatches > max_dispatches) {
        dlss_chain.active = false;
        dlss_chain.pending.clear();
    }
    if (dlss_chain.active) {
        // While the network can't be shown, the passes of earlier evaluations stop running and
        // the replacement upscale draws the output at the output pass.
        const GPUVAddr program = regs.code_loc.Address();
        if (GameDlssBlocker() != VideoCore::GameDlssReason::None &&
            dlss_chain.programs.contains(program)) {
            return output_pass ? FinishDlssEvaluation() : true;
        }
        dlss_chain.pending.insert(program);
    }
    // Ocarina of Time's movie network (port of Eglantine 92f119195f): a pass that reads one
    // colour image and stores none retains its colour and opens a skip window; the pass that
    // stores an image upscaling it is replaced by a plain copy and closes it. The gameplay
    // network is left alone: its pair (+0xc0 -> +0xd0) carries the tonemap.
    if (!native_upscale_disabled && !dlss_chain.active &&
        Settings::GetCurrentProgramID() == Tegra::Nx2OcarinaOfTimeProgramId &&
        (launch_description.const_buffer_enable_mask & 1)) {
        const auto& cbuf = launch_description.const_buffer_config[0];
        const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
        const auto image = [&](u32 offset) -> std::optional<Texture::TICEntry> {
            if (offset + sizeof(u32) > cbuf.size.Value()) return std::nullopt;
            const u32 handle = memory_manager.Read<u32>(cbuf_address + offset);
            if (handle == 0 || (handle & 0xfffff) > regs.tic.limit) return std::nullopt;
            return GetTICEntry(handle & 0xfffff);
        };
        const auto upscale_pair = [](const std::optional<Texture::TICEntry>& from,
                                     const std::optional<Texture::TICEntry>& to,
                                     bool same_format) {
            if (!from || !to || (same_format && from->format != to->format) ||
                from->Address() == 0 || to->Address() == 0 ||
                from->Address() == to->Address() || !from->IsBlockLinear() ||
                !to->IsBlockLinear())
                return false;
            const u64 from_width = from->Width(), from_height = from->Height();
            const u64 to_width = to->Width(), to_height = to->Height();
            const u64 cross_a = to_width * from_height, cross_b = to_height * from_width;
            return from_width >= 64 && to_width > from_width && to_width <= from_width * 4 &&
                   (cross_a > cross_b ? cross_a - cross_b : cross_b - cross_a) * 32 <= cross_a;
        };
        const auto [reads, stores] = NativeImageHandles(regs.code_loc.Address());
        std::optional<Texture::TICEntry> replacement_input;
        std::optional<Texture::TICEntry> replacement_output;
        if (native_upscale_input && network_skip_remaining > 0 && !stores.empty()) {
            if (const auto store = FindNativeUpscaleOutput(native_upscale_input, stores, image,
                                                           upscale_pair)) {
                replacement_input = native_upscale_input;
                replacement_output = image(*store);
            }
        }
        bool skipping{};
        if (reads.size() == 1 && stores.empty() && reads.front() == 0x58) {
            // Ocarina's movie network reads its colour at +0x58; anchoring the window there
            // keeps gameplay compute untouched.
            RememberNativeUpscaleInput();
            if (native_upscale_input) {
                network_skip_remaining = 32;
                skipping = true;
            }
        } else if (!replacement_input && network_skip_remaining > 0) {
            if (--network_skip_remaining == 0) {
                native_upscale_input.reset();
            }
            skipping = true;
        }
        if (replacement_input && replacement_output) {
            const auto surface = [](const Texture::TICEntry& tic)
                -> std::optional<Fermi2D::Surface> {
                using RTFormat = Tegra::RenderTargetFormat;
                Fermi2D::Surface result{};
                switch (tic.format) {
                case Texture::TextureFormat::B10G11R11:
                    result.format = RTFormat::B10G11R11_FLOAT;
                    break;
                case Texture::TextureFormat::R16G16B16A16:
                    result.format = RTFormat::R16G16B16A16_FLOAT;
                    break;
                case Texture::TextureFormat::A8B8G8R8:
                    result.format = RTFormat::A8B8G8R8_UNORM;
                    break;
                case Texture::TextureFormat::A2B10G10R10:
                    result.format = RTFormat::A2B10G10R10_UNORM;
                    break;
                default:
                    return std::nullopt;
                }
                if (tic.Address() == 0 || !tic.IsBlockLinear()) {
                    return std::nullopt;
                }
                result.linear = Fermi2D::MemoryLayout::BlockLinear;
                result.block_width.Assign(tic.block_width);
                result.block_height.Assign(tic.block_height);
                result.block_depth.Assign(tic.block_depth);
                result.depth = 1;
                result.width = tic.Width();
                result.height = tic.Height();
                result.addr_upper = static_cast<u32>(tic.Address() >> 32);
                result.addr_lower = static_cast<u32>(tic.Address());
                return result;
            };
            const auto source = surface(*replacement_input);
            const auto dest = surface(*replacement_output);
            if (source && dest) {
                // Does the program decode sRGB itself? (Ocarina's movie network does.)
                const bool decodes_srgb = [&] {
                    const GPUVAddr program = regs.code_loc.Address();
                    std::array<u32, 0x400> block{};
                    bool threshold = false, exponent = false;
                    for (u64 offset = 0; offset < 0x20000 && !(threshold && exponent);
                         offset += sizeof(block)) {
                        if (!memory_manager.IsFullyMappedRange(program + offset, sizeof(block)))
                            break;
                        memory_manager.ReadBlock(program + offset, block.data(), sizeof(block));
                        for (size_t word = 0; word < block.size(); word += 4) {
                            const u32 opcode = block[word] & 0xfff;
                            threshold |= opcode == 0x434 && block[word + 1] == 0x292d292d;
                            exponent |= opcode == 0x832 && block[word + 1] == 0x40cc40cc;
                        }
                    }
                    return threshold && exponent;
                }();
                const Fermi2D::Config config{
                    .operation = decodes_srgb ? Fermi2D::Operation::SrcCopySrgbToLinear
                                              : Fermi2D::Operation::SrcCopy,
                    .filter = Fermi2D::Filter::Bilinear,
                    .must_accelerate = true,
                    .dst_x0 = 0,
                    .dst_y0 = 0,
                    .dst_x1 = static_cast<s32>(replacement_output->Width()),
                    .dst_y1 = static_cast<s32>(replacement_output->Height()),
                    .src_x0 = 0,
                    .src_y0 = 0,
                    .src_x1 = static_cast<s32>(replacement_input->Width()),
                    .src_y1 = static_cast<s32>(replacement_input->Height()),
                };
                const bool done = rasterizer->AccelerateSurfaceCopy(*source, *dest, config);
                LOG_DEBUG(HW_GPU,
                          "Native network pass {:#x} replaced by input copy {}->{} ({}): {}",
                          regs.code_loc.Address(), replacement_input->Address(),
                          replacement_output->Address(), decodes_srgb ? "sRGB" : "plain", done);
                native_upscale_input.reset();
                network_skip_remaining = 0;
                if (done) {
                    return true;
                }
                // Run the real pass, and the whole network on later frames.
                native_upscale_disabled = true;
                skipping = false;
            }
        }
        if (skipping) {
            return true;
        }
    }
    bool dispatched = true;
    try {
        rasterizer->DispatchCompute();
    } catch (const Shader::Exception& error) {
        static std::unordered_set<std::string> logged;
        if (logged.insert(error.what()).second) {
            LOG_ERROR(HW_GPU, "Native compute shader rejected: {}", error.what());
        }
        dispatched = false;
        if (dlss_chain.active) {
            ++dlss_chain.rejected;
        } else if (std::string_view{error.what()}.find("matrix multiply") !=
                   std::string_view::npos) {
            DescribeNativeResources(qmd);
            // The console runs this on tensor cores; stand in with a plain upscale of its input.
            return UpscaleInsteadOfNativeProgram();
        }
    }
    if (output_pass) {
        return FinishDlssEvaluation() || dispatched;
    }
    return dispatched;
}

// Titles whose DLSS evaluation is followed: Fire Emblem: Fortune's Weave, the only one whose
// evaluation layout is known. EDEN_NX2_GAME_DLSS=1 follows it in every title (to look for the same
// layout elsewhere). In other titles a dispatch costs one comparison more.
bool KeplerCompute::UsesGameDlss() const {
    static const bool every_title = [] {
        const char* value = std::getenv("EDEN_NX2_GAME_DLSS");
        return value != nullptr && value[0] == '1';
    }();
    return every_title ||
           Settings::GetCurrentProgramID() == Tegra::Nx2FireEmblemFortunesWeaveProgramId;
}

// Why the replacement upscale draws instead of the game's own network, apart from passes that
// can't run (None when nothing keeps the network from being shown).
VideoCore::GameDlssReason KeplerCompute::GameDlssBlocker() const {
    // NVIDIA DLSS, when turned on and running, comes first.
    if (rasterizer->UsesNvidiaDlss()) {
        return VideoCore::GameDlssReason::NvidiaDlss;
    }
    if (!Settings::values.game_dlss_network.GetValue()) {
        return VideoCore::GameDlssReason::TurnedOff;
    }
    return VideoCore::GameDlssReason::None;
}

// Logs what draws the game's DLSS evaluation and tells the settings dialog, when that changes.
void KeplerCompute::UpdateGameDlssStatus(bool complete, VideoCore::GameDlssReason blocker) {
    using VideoCore::GameDlssReason;
    using VideoCore::GameDlssState;
    VideoCore::GameDlssStatus shown{.state = GameDlssState::Network};
    if (blocker != GameDlssReason::None) {
        shown = {.state = GameDlssState::Replacement, .reason = blocker};
    } else if (!complete) {
        shown = {.state = GameDlssState::Replacement,
                 .reason = GameDlssReason::PassesCannotRun,
                 .rejected_passes = dlss_chain.rejected};
    }
    if (shown.state == dlss_chain.status.state && shown.reason == dlss_chain.status.reason &&
        shown.rejected_passes == dlss_chain.status.rejected_passes) {
        return;
    }
    if (shown.state == GameDlssState::Network) {
        LOG_INFO(HW_GPU, "Game DLSS network: every pass runs, its own output is shown");
    } else if (shown.reason == GameDlssReason::PassesCannotRun) {
        LOG_INFO(HW_GPU,
                 "Game DLSS network: {} pass(es) can't run, the replacement upscale draws the "
                 "output",
                 shown.rejected_passes);
    } else {
        LOG_INFO(HW_GPU, "Game DLSS network: {}, the replacement upscale draws the output",
                 VideoCore::GameDlssReasonText(shown.reason));
    }
    dlss_chain.status = shown;
    VideoCore::SetGameDlssStatus(shown);
}

// Ends this frame's DLSS evaluation at its output pass: either the network's own result stays,
// or the replacement upscale draws over it. Returns whether the replacement ran.
bool KeplerCompute::FinishDlssEvaluation() {
    dlss_chain.active = false;
    dlss_chain.programs.insert(dlss_chain.pending.begin(), dlss_chain.pending.end());
    dlss_chain.pending.clear();
    const bool complete = dlss_chain.rejected == 0;
    const VideoCore::GameDlssReason blocker = GameDlssBlocker();
    UpdateGameDlssStatus(complete, blocker);
    if (complete && blocker == VideoCore::GameDlssReason::None) {
        return false;
    }
    return UpscaleInsteadOfNativeProgram();
}

// Fortune's Weave's last DLSS network pass: it reads the input colour (the first pass's) through
// cbuf0+0x328 and writes the upscaled image through +0x348.
bool KeplerCompute::IsDlssOutputPass() {
    const auto& cbuf = launch_description.const_buffer_config[0];
    if (cbuf.size.Value() < 0x34c) {
        return false;
    }
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    const auto image = [&](u32 offset) -> std::optional<Texture::TICEntry> {
        const u32 tic_index = memory_manager.Read<u32>(cbuf_address + offset) & 0xfffff;
        if (tic_index > regs.tic.limit) {
            return std::nullopt;
        }
        return GetTICEntry(tic_index);
    };
    const auto input = image(0x328), output = image(0x348);
    return input && output && input->Address() == dlss_color && output->Address() != 0 &&
           input->format == output->format && output->Width() > input->Width();
}

// Fire Emblem: Fortune's Weave's first DLSS pass has the input colour (B10G11R11) at cbuf0+0x278,
// its motion vectors (R16G16 float, the input's size) at +0x290, its depth (Z32, reversed, the
// input's size) at +0x298 and this frame's camera jitter (x, y; input pixels) at +0x1b0.
bool KeplerCompute::RememberDlssInputs() {
    const auto& cbuf = launch_description.const_buffer_config[0];
    if (cbuf.size.Value() < 0x294) {
        return false;
    }
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    const auto image = [&](u32 offset) -> std::optional<Texture::TICEntry> {
        const u32 tic_index = memory_manager.Read<u32>(cbuf_address + offset) & 0xfffff;
        if (tic_index > regs.tic.limit) {
            return std::nullopt;
        }
        return GetTICEntry(tic_index);
    };
    const auto color = image(0x278), motion = image(0x290);
    if (!color || !motion || color->format != Texture::TextureFormat::B10G11R11 ||
        motion->format != Texture::TextureFormat::R16G16 || motion->Address() == 0 ||
        motion->Width() != color->Width() || motion->Height() != color->Height() ||
        !motion->IsBlockLinear()) {
        return false;
    }
    dlss_color = color->Address();
    dlss_motion_vectors = *motion;
    const auto depth = cbuf.size.Value() >= 0x29c ? image(0x298) : std::nullopt;
    if (depth && depth->format == Texture::TextureFormat::Z32 && depth->Address() != 0 &&
        depth->Width() == color->Width() && depth->Height() == color->Height() &&
        depth->IsBlockLinear()) {
        dlss_depth = *depth;
    } else {
        dlss_depth.reset();
    }
    dlss_jitter = {std::bit_cast<f32>(memory_manager.Read<u32>(cbuf_address + 0x1b0)),
                   std::bit_cast<f32>(memory_manager.Read<u32>(cbuf_address + 0x1b4))};
    return true;
}

// Ocarina of Time's gameplay upscaler is a tensor-core network (768 matrix multiplies). Emulated,
// it costs GPU time every frame (minutes to compile and about 8 FPS on Mesa's Intel driver, in
// Lichii), so its matrix multiplies are rejected and UpscaleInsteadOfNativeProgram draws a plain
// upscale of its input instead: no reconstruction of detail or anti-aliasing beyond bilinear.
// Only the network's programs are rejected (IsOcarinaUpscalerNetwork in shader_environment.cpp);
// the game's other tensor programs, its 1080p upscaler among them, are emulated. Every other
// title's programs (Donkey Kong Bananza's scene passes among them) are emulated as before.
// EDEN_NX2_OOT_NETWORK=1 emulates Ocarina's network too. The passes of a DLSS evaluation
// (Fortune's Weave) are emulated only while the game's own network may be shown; otherwise they
// are rejected, never compiled, and the stand-in draws at the output pass.
bool KeplerCompute::EmulatesNativeTensorPrograms() const {
    if (dlss_chain.active) {
        return GameDlssBlocker() == VideoCore::GameDlssReason::None;
    }
    static const bool oot_network = [] {
        const char* value = std::getenv("EDEN_NX2_OOT_NETWORK");
        return value != nullptr && value[0] == '1';
    }();
    return oot_network || Settings::GetCurrentProgramID() != Tegra::Nx2OcarinaOfTimeProgramId;
}

// Logs the images a rejected program's handles (constant bank 0, bytes 0x80-0x11c) point to, so a
// substitute (e.g. an upscale blit for a tensor-core pass) can find its inputs and output.
void KeplerCompute::DescribeNativeResources(const NativeQmdScheduler::Descriptor& qmd) {
    static u32 described{};
    if (described++ >= 2) return;
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    LOG_INFO(HW_GPU, "Native resources: cbuf0 {:#x} size {:#x}, grid {}x{}x{}, block {}x{}x{}, tic pool {:#x}",
             cbuf_address, cbuf.size.Value(), qmd[12], qmd[13] & 0xffff, qmd[14] & 0xffff,
             launch_description.block_dim_x.Value(), launch_description.block_dim_y.Value(),
             launch_description.block_dim_z.Value(), regs.tic.Address());
    for (u32 offset = 0x80; offset < 0x120; offset += 4) {
        const u32 handle = memory_manager.Read<u32>(cbuf_address + offset);
        const u32 tic_index = handle & 0xfffff;
        if (handle == 0 || tic_index > regs.tic.limit) continue;
        const auto tic = GetTICEntry(tic_index);
        LOG_INFO(HW_GPU, "  handle @{:#x} = {:#x}: tic {} address {:#x} {}x{} format {:#x} types {}/{}/{}/{}",
                 offset, handle, tic_index, tic.Address(), tic.Width(), tic.Height(),
                 static_cast<u32>(tic.format.Value()), static_cast<u32>(tic.r_type.Value()),
                 static_cast<u32>(tic.g_type.Value()), static_cast<u32>(tic.b_type.Value()),
                 static_cast<u32>(tic.a_type.Value()));
    }
}

std::atomic<u32> g_nx2_trace_draws_after_upscale{};

// The cbuf0 byte offsets of the texture handles a native program reads (TEX, TLD, TLD4, TXD) and
// the surface handles it stores to (SUST.P), from its code up to the first unconditional EXIT.
std::pair<std::vector<u32>, std::vector<u32>> KeplerCompute::NativeImageHandles(GPUVAddr program) {
    static std::unordered_map<GPUVAddr, std::pair<std::vector<u32>, std::vector<u32>>> cache;
    if (const auto it = cache.find(program); it != cache.end()) {
        return it->second;
    }
    std::pair<std::vector<u32>, std::vector<u32>> handles;
    const auto add = [](std::vector<u32>& list, u32 offset) {
        if (std::find(list.begin(), list.end(), offset) == list.end()) list.push_back(offset);
    };
    std::array<u32, 0x400> block{};
    bool exited = false;
    for (u64 offset = 0; offset < 0x40000 && !exited; offset += sizeof(block)) {
        if (!memory_manager.IsFullyMappedRange(program + offset, sizeof(block))) break;
        memory_manager.ReadBlock(program + offset, block.data(), sizeof(block));
        for (size_t word = 0; word < block.size() && !exited; word += 4) {
            const u32 opcode = block[word] & 0xfff;
            const u32 binding = (block[word + 1] >> 22) & 0x1f;
            const u32 handle = ((block[word + 1] >> 8) & 0x3fff) * 4;
            switch (opcode) {
            case 0xb60: case 0xb63: case 0xb66: case 0xb6d:
                if (binding == 0) add(handles.first, handle);
                break;
            case 0xb9b:
                if (binding == 0) add(handles.second, handle);
                break;
            case 0x94d: // EXIT, unconditional when guarded by PT
                exited = ((block[word] >> 12) & 0xf) == 7;
                break;
            default:
                break;
            }
        }
    }
    return cache.emplace(program, std::move(handles)).first->second;
}

void KeplerCompute::RememberNativeUpscaleInput() {
    native_upscale_input.reset();
    const auto [reads, stores] = NativeImageHandles(regs.code_loc.Address());
    // An image-fed matrix pass writes its intermediate results to buffers. Retain its colour
    // input: the final pass's sampled image is another intermediate that a skipped network never
    // produced (Ocarina's movie network reads its colour at +0x58).
    if (reads.size() != 1 || !stores.empty()) return;
    const auto& cbuf = launch_description.const_buffer_config[0];
    if (reads.front() + sizeof(u32) > cbuf.size.Value()) return;
    const GPUVAddr address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    const u32 handle = memory_manager.Read<u32>(address + reads.front());
    if (handle == 0 || (handle & 0xfffff) > regs.tic.limit) return;
    const auto input = GetTICEntry(handle & 0xfffff);
    if (input.Address() != 0 && input.IsBlockLinear() && input.Width() >= 64 &&
        input.Height() >= 64) {
        native_upscale_input = input;
    }
}


// Title 4's upscaler reads the tonemapped frame through the texture handle at cbuf0+0x90 and
// writes the output resolution image through the surface handle at cbuf0+0xa0 (from its
// TLD/SUST operands); Kirby Air Riders' reads a 2048x1152 image at cbuf0+0x80 and writes a
// 2560x1440 one at +0x90; Fire Emblem: Fortune's Weave's DLSS network ends with a pass that reads
// the input colour through cbuf0+0x328 and writes the DLSS output through cbuf0+0x348 (B10G11R11;
// 720p to 1440p docked). Blit one to the other with bilinear filtering when both are plain 2D
// images of the same format and the output is larger: title 4's slots first, then Fortune's
// Weave's, else the first two handles in cbuf0+0x80..0x11c that form such a pair. Star Fox's
// pair in title 4's slots differs in format and is converted.
bool KeplerCompute::UpscaleInsteadOfNativeProgram() {
    const auto& cbuf = launch_description.const_buffer_config[0];
    const GPUVAddr cbuf_address = (GPUVAddr{cbuf.address_high} << 32) | cbuf.address_low;
    const auto image = [&](u32 offset) -> std::optional<Texture::TICEntry> {
        const u32 tic_index = memory_manager.Read<u32>(cbuf_address + offset) & 0xfffff;
        if (tic_index > regs.tic.limit) return std::nullopt;
        return GetTICEntry(tic_index);
    };
    const auto upscale_pair = [](const std::optional<Texture::TICEntry>& from,
                                 const std::optional<Texture::TICEntry>& to, bool convert = false) {
        return from && to && (convert || from->format == to->format) && from->Address() != 0 &&
               to->Address() != 0 && to->Width() > from->Width() && from->IsBlockLinear() &&
               to->IsBlockLinear();
    };
    std::optional<Texture::TICEntry> input = image(0x90), output = image(0xa0);
    // Star Fox's upscaler reads a 1600x900 RGBA16F image and writes a 1920x1080 RGB10A2 one.
    const bool star_fox_pair =
        Settings::GetCurrentProgramID() == Tegra::Nx2StarFoxProgramId && input && output &&
        input->format == Texture::TextureFormat::R16G16B16A16 &&
        output->format == Texture::TextureFormat::A2B10G10R10 && input->Width() == 1600 &&
        input->Height() == 900 && output->Width() == 1920 && output->Height() == 1080;
    bool dlss_pair = false;
    if (!upscale_pair(input, output, star_fox_pair) && UsesGameDlss() &&
        cbuf.size.Value() >= 0x34c) {
        input = image(0x328);
        output = image(0x348);
        dlss_pair = upscale_pair(input, output);
    }
    if (!upscale_pair(input, output, star_fox_pair)) {
        input.reset();
        output.reset();
    }
    bool rgb9e5_pair = false;
    if (!input && Settings::GetCurrentProgramID() == Tegra::Nx2OcarinaOfTimeProgramId) {
        // Ocarina of Time's gameplay network reads the frame packed as RGB9E5, each channel the
        // square root of the tonemapped value (the copy squares it back), and writes a B10G11R11
        // image at least as wide. Its constant buffer keeps stale handles (a bloom level
        // at +0xa0) that the scan below pairs with the output, so this pair goes first: the widest
        // B10G11R11 image when nothing in the pass is wider, from the widest RGB9E5 image of the
        // same aspect at most 4x narrower.
        const u32 limit = (std::min)(cbuf.size.Value(), 0x200U);
        u32 widest = 0;
        for (u32 offset = 0; offset + 4 <= limit; offset += 4) {
            const auto tic = memory_manager.Read<u32>(cbuf_address + offset) != 0 ? image(offset)
                                                                                     : std::nullopt;
            if (!tic || tic->Address() == 0 || !tic->IsBlockLinear()) continue;
            widest = (std::max)(widest, tic->Width());
            if (tic->format == Texture::TextureFormat::B10G11R11 &&
                (!output || tic->Width() > output->Width())) {
                output = tic;
            }
            if (tic->format == Texture::TextureFormat::E5B9G9R9 &&
                (!input || tic->Width() > input->Width())) {
                input = tic;
            }
        }
        rgb9e5_pair = [&] {
            if (!input || !output || output->Width() != widest || input->Width() > widest ||
                4 * input->Width() < widest) {
                return false;
            }
            const u64 cross_a = u64{input->Width()} * output->Height();
            const u64 cross_b = u64{output->Width()} * input->Height();
            return (std::max)(cross_a, cross_b) - (std::min)(cross_a, cross_b) <= cross_b / 50;
        }();
        if (!rgb9e5_pair) {
            input.reset();
            output.reset();
        }
    }
    if (!input) {
        for (u32 first = 0x80; first < 0x120 && !input; first += 4) {
            const auto from = memory_manager.Read<u32>(cbuf_address + first) != 0 ? image(first)
                                                                                    : std::nullopt;
            for (u32 second = first + 4; from && second < 0x120; second += 4) {
                if (memory_manager.Read<u32>(cbuf_address + second) == 0) continue;
                const auto to = image(second);
                if (upscale_pair(from, to)) {
                    input = from;
                    output = to;
                    break;
                }
            }
        }
    }
    if (!input || !output)
        return false;
    using RTFormat = Tegra::RenderTargetFormat;
    const auto rt_format = [](Texture::TextureFormat format) -> std::optional<RTFormat> {
        switch (format) {
        case Texture::TextureFormat::A2B10G10R10: return RTFormat::A2B10G10R10_UNORM;
        case Texture::TextureFormat::A8B8G8R8: return RTFormat::A8B8G8R8_UNORM;
        case Texture::TextureFormat::B10G11R11: return RTFormat::B10G11R11_FLOAT;
        case Texture::TextureFormat::R16G16B16A16: return RTFormat::R16G16B16A16_FLOAT;
        default: return std::nullopt;
        }
    };
    // Only Ocarina's pair reads an RGB9E5 image, never a destination.
    const std::optional<RTFormat> input_format =
        rgb9e5_pair ? RTFormat::E5B9G9R9_FLOAT : rt_format(input->format);
    const std::optional<RTFormat> output_format = rt_format(output->format);
    if (!input_format || !output_format)
        return false;
    const auto surface = [&](const Texture::TICEntry& tic, RTFormat format) {
        Fermi2D::Surface result{};
        result.format = format;
        result.linear = Fermi2D::MemoryLayout::BlockLinear;
        result.block_width.Assign(tic.block_width);
        result.block_height.Assign(tic.block_height);
        result.block_depth.Assign(tic.block_depth);
        result.depth = 1;
        result.width = tic.Width();
        result.height = tic.Height();
        result.addr_upper = static_cast<u32>(tic.Address() >> 32);
        result.addr_lower = static_cast<u32>(tic.Address());
        return result;
    };
    // Title 4's program ends by decoding its result from sRGB to linear (the 0.04045 threshold
    // and the 2.4 exponent as half-precision immediates of HSETP2 and HMUL2). The copy of its
    // sRGB input left the frame gamma-encoded twice, washed out: decode in the copy when the
    // program does.
    static std::unordered_map<GPUVAddr, bool> decodes_srgb;
    const GPUVAddr program = regs.code_loc.Address();
    const auto [known, inserted] = decodes_srgb.try_emplace(program, false);
    if (inserted) {
        bool threshold = false, exponent = false;
        std::array<u32, 0x400> block{};
        for (u64 offset = 0; offset < 0x20000 && !(threshold && exponent); offset += sizeof(block)) {
            if (!memory_manager.IsFullyMappedRange(program + offset, sizeof(block))) break;
            memory_manager.ReadBlock(program + offset, block.data(), sizeof(block));
            for (size_t word = 0; word < block.size(); word += 4) {
                const u32 opcode = block[word] & 0xfff;
                threshold |= opcode == 0x434 && block[word + 1] == 0x292d292d;
                exponent |= opcode == 0x832 && block[word + 1] == 0x40cc40cc;
            }
        }
        known->second = threshold && exponent;
        LOG_INFO(HW_GPU, "Upscale fallback for program {:#x}: {}", program,
                 known->second ? "decodes sRGB like the program" : "plain copy");
    }
    // The stand-in for a game's DLSS is sharpened with RCAS ("RCAS Sharpness").
    const bool sharpen = dlss_pair && Settings::values.game_dlss_sharpness.GetValue() > 0;
    const Fermi2D::Config config{
        .operation = rgb9e5_pair     ? Fermi2D::Operation::SrcCopySquare
                     : known->second ? Fermi2D::Operation::SrcCopySrgbToLinear
                     : sharpen       ? Fermi2D::Operation::SrcCopyRcas
                                     : Fermi2D::Operation::SrcCopy,
        .filter = Fermi2D::Filter::Bilinear,
        .must_accelerate = true,
        .dst_x0 = 0,
        .dst_y0 = 0,
        .dst_x1 = static_cast<s32>(output->Width()),
        .dst_y1 = static_cast<s32>(output->Height()),
        .src_x0 = 0,
        .src_y0 = 0,
        .src_x1 = static_cast<s32>(input->Width()),
        .src_y1 = static_cast<s32>(input->Height()),
    };
    // Debug (EDEN_NX2_SAMPLE_AT=<n>): at the n-th upscale, download the input, the output and the
    // targets and count non-zero words. Off by default: the downloads froze title 4 for ~1.3 s.
    static const u32 sample_at = [] {
        const char* value = std::getenv("EDEN_NX2_SAMPLE_AT");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : ~0U;
    }();
    if (static u32 sampled{}; sampled++ == sample_at) {
        const auto sample = [&](const Texture::TICEntry& tic) {
            const u64 size = u64{tic.Width()} * tic.Height() * 4;
            if (const auto device = memory_manager.GpuToCpuAddress(tic.Address())) {
                rasterizer->FlushRegion(*device, size, VideoCommon::CacheType::All);
            }
            u32 nonzero{}, samples{};
            u64 sum{};
            for (u64 offset = 0; offset < size; offset += 4096 + 4) {
                const u32 word = memory_manager.Read<u32>(tic.Address() + offset);
                ++samples;
                nonzero += word != 0;
                sum += (word & 0x3ff) + ((word >> 10) & 0x3ff) + ((word >> 20) & 0x3ff);
            }
            LOG_INFO(HW_GPU, "Upscale fallback image {:#x}: {}/{} sampled words non-zero, mean channel {}",
                     tic.Address(), nonzero, samples, samples ? sum / (samples * 3) : 0);
        };
        sample(*input);
        sample(*output);
        // Every 2560x1440 target the in-game passes draw to (from EDEN_NX2_RT_TRACE).
        for (const GPUVAddr address : {0x8116a03000ULL, 0x8117ae5000ULL, 0x8114b62000ULL,
                                       0x8115a63000ULL, 0x8117904000ULL, 0x8114a02000ULL}) {
            Texture::TICEntry fake = *input;
            const u64 size = 2560ULL * 1440 * 4;
            if (const auto device = memory_manager.GpuToCpuAddress(address)) {
                rasterizer->FlushRegion(*device, size, VideoCommon::CacheType::All);
            }
            u32 nonzero{}, samples{}, distinct{};
            u32 last{};
            for (u64 offset = 0; offset < size; offset += 4096 + 4) {
                const u32 word = memory_manager.Read<u32>(address + offset);
                ++samples;
                nonzero += word != 0;
                distinct += word != last;
                last = word;
            }
            LOG_INFO(HW_GPU, "Upscale fallback target {:#x}: {}/{} non-zero, {} changes", address, nonzero,
                     samples, distinct);
            (void)fake;
        }
    }
    auto source = surface(*input, *input_format);
    // EDEN_NX2_UPSCALE_SOURCE=<gpu address>: blit from another same-sized image (debug aid).
    if (static const u64 override_source = [] {
            const char* value = std::getenv("EDEN_NX2_UPSCALE_SOURCE");
            return value ? std::strtoull(value, nullptr, 16) : 0ULL;
        }();
        override_source != 0) {
        source.addr_upper = static_cast<u32>(override_source >> 32);
        source.addr_lower = static_cast<u32>(override_source);
        if (const char* value = std::getenv("EDEN_NX2_UPSCALE_SOURCE_FORMAT")) {
            source.format = static_cast<RTFormat>(std::strtoul(value, nullptr, 16));
        }
    }
    // NVIDIA DLSS, when on, takes the evaluation's colour, motion vectors, depth and jitter (from
    // its first pass); the blit stays as the fallback.
    const auto nvidia_dlss = [&] {
        if (!dlss_pair || dlss_color != input->Address() || !dlss_motion_vectors || !dlss_depth ||
            !rasterizer->UsesNvidiaDlss()) {
            return false;
        }
        const Fermi2D::Surface motion = surface(*dlss_motion_vectors, RTFormat::R16G16_FLOAT);
        return rasterizer->AccelerateNvidiaDlss(source, surface(*output, *output_format), motion,
                                                *dlss_depth, dlss_jitter);
    };
    const bool done = nvidia_dlss() ||
                      (rgb9e5_pair &&
                       rasterizer->AccelerateOcarinaDlss(source, surface(*output, *output_format))) ||
                      rasterizer->AccelerateSurfaceCopy(source, surface(*output, *output_format),
                                                        config);
    if (static u32 checked{}; checked++ == sample_at) {
        const u64 size = u64{output->Width()} * output->Height() * 4;
        if (const auto device = memory_manager.GpuToCpuAddress(output->Address())) {
            rasterizer->FlushRegion(*device, size, VideoCommon::CacheType::All);
        }
        u32 nonzero{}, samples{}, changes{}, last{};
        for (u64 offset = 0; offset < size; offset += 4096 + 4) {
            const u32 word = memory_manager.Read<u32>(output->Address() + offset);
            ++samples;
            nonzero += word != 0;
            changes += word != last;
            last = word;
        }
        LOG_INFO(HW_GPU, "Upscale fallback output after blit: {}/{} non-zero, {} changes", nonzero, samples,
                 changes);
    }
    // Debug: trace the draws that follow one upscale (EDEN_NX2_RT_TRACE_AFTER_UPSCALE).
    static const u32 trace_at = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_AFTER_UPSCALE");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
    }();
    if (static u32 count{}; trace_at != 0 && ++count == trace_at) g_nx2_trace_draws_after_upscale = 40;
    static u32 logged{};
    if (logged++ < 4) {
        LOG_INFO(HW_GPU, "Upscale fallback {:#x} {}x{} block {}/{}/{} -> {:#x} {}x{} block {}/{}/{} levels {}: {}",
                 input->Address(), input->Width(), input->Height(), input->block_width.Value(),
                 input->block_height.Value(), input->block_depth.Value(), output->Address(),
                 output->Width(), output->Height(), output->block_width.Value(),
                 output->block_height.Value(), output->block_depth.Value(), output->max_mip_level.Value(),
                 done ? "blitted" : "not accelerated");
    }
    return done;
}

void KeplerCompute::ProcessLaunch() {
    const GPUVAddr launch_desc_loc = regs.launch_desc_loc.Address();
    memory_manager.ReadBlockUnsafe(launch_desc_loc, &launch_description,
                                   LaunchParams::NUM_LAUNCH_PARAMETERS * sizeof(u32));
    rasterizer->DispatchCompute();
}

Texture::TICEntry KeplerCompute::GetTICEntry(u32 tic_index) const {
    const GPUVAddr tic_address_gpu{regs.tic.Address() + tic_index * sizeof(Texture::TICEntry)};

    Texture::TICEntry tic_entry;
    memory_manager.ReadBlockUnsafe(tic_address_gpu, &tic_entry, sizeof(Texture::TICEntry));
    return tic_entry;
}

Texture::TSCEntry KeplerCompute::GetTSCEntry(u32 tsc_index) const {
    const GPUVAddr tsc_address_gpu{regs.tsc.Address() + tsc_index * sizeof(Texture::TSCEntry)};

    Texture::TSCEntry tsc_entry;
    memory_manager.ReadBlockUnsafe(tsc_address_gpu, &tsc_entry, sizeof(Texture::TSCEntry));
    return tsc_entry;
}

// Split GPU thread with macros on its front half (gpu_split.h, EDEN_NX2_SPLIT=2): the front
// runs compute macros against its own copy of the native registers (which hold the last value
// written to each, on the back too) and loads and binds the compute macro code itself.
struct KeplerCompute::FrontState {
    std::array<u32, 0x1000> registers{};
    std::optional<u32> executing_macro;
    std::vector<u32> parameters;
    bool compute_uploaded{};
    u32 bind_pointer{};
    MacroEngine* macro{}; ///< Without a native context: the one macro engine
    std::vector<ParkedMacro> parked; ///< ParkMacro's calls while the front runs the macros
    /// GPU address of the running call's grid size (FrontTakeIndirectArguments).
    GPUVAddr indirect_grid{};
    u32 indirect_grid_slot{};
    /// Whether the macro code at indirect_macro_start is NVN's grid size macro, as of
    /// indirect_macro_generation of the macro code.
    u32 indirect_macro_start{~0U};
    u64 indirect_macro_generation{~0ULL};
    bool indirect_macro{};
};

KeplerCompute::FrontState& KeplerCompute::Front(VideoCommon::GpuSplit& split) {
    if (!front) [[unlikely]] {
        // Copy the back's state and take the macros over while it is idle.
        split.Drain(VideoCommon::GpuSplit::DrainSite::FrontStart);
        front = std::make_unique<FrontState>();
        front->registers = native_registers;
        front->executing_macro = native_executing_macro;
        front->parameters = std::move(native_parameters);
        native_parameters.clear();
        native_executing_macro.reset();
        front->compute_uploaded = native_compute_uploaded;
        front->bind_pointer = native_bind_pointer;
        front->macro = native_macro;
        front_macros = true;
    }
    return *front;
}

// As Maxwell3D::ReleaseFront: the split stopped, the engine runs its macros again.
void KeplerCompute::ReleaseFront() {
    native_executing_macro = front->executing_macro;
    native_parameters = std::move(front->parameters);
    native_compute_uploaded = front->compute_uploaded;
    native_bind_pointer = front->bind_pointer;
    native_macro = front->macro;
    front.reset();
    front_macros = false;
    LOG_INFO(HW_GPU, "GPU thread split stopped: compute macros back on the engine");
}

// The condition under which DmaPusher sends macro methods to FrontCallMethod.
bool KeplerCompute::MacroParkedOnFront(VideoCommon::GpuSplit& split) const {
    return split.Active() && split.FrontMacros() && FrontRunsMacros();
}

std::optional<KeplerCompute::ParkedMacro> KeplerCompute::TakeParkedMacro(
    std::vector<ParkedMacro>& parked, GPUVAddr key) {
    const auto it = std::ranges::find(parked, key, &ParkedMacro::key);
    if (it == parked.end()) {
        return std::nullopt;
    }
    ParkedMacro macro = std::move(*it);
    parked.erase(it);
    return macro;
}

void KeplerCompute::ParkMacro(GPUVAddr key) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (MacroParkedOnFront(split)) {
        auto& fs = Front(split);
        fs.parked.push_back(ParkedMacro{key, std::exchange(fs.executing_macro, std::nullopt),
                                        std::exchange(fs.parameters, {})});
        return;
    }
    const auto park = [this, key] {
        parked_macros.push_back(ParkedMacro{key,
                                            std::exchange(native_executing_macro, std::nullopt),
                                            std::exchange(native_parameters, {})});
    };
    if (split.Active()) {
        split.Defer(park, false);
    } else {
        park();
    }
}

void KeplerCompute::ResumeMacro(GPUVAddr key) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (MacroParkedOnFront(split)) {
        auto& fs = Front(split);
        if (auto macro = TakeParkedMacro(fs.parked, key)) {
            fs.executing_macro = macro->method;
            fs.parameters = std::move(macro->parameters);
        }
        return;
    }
    const auto resume = [this, key] {
        if (auto macro = TakeParkedMacro(parked_macros, key)) {
            native_executing_macro = macro->method;
            native_parameters = std::move(macro->parameters);
        }
    };
    if (split.Active()) {
        split.Defer(resume, false);
    } else {
        resume();
    }
}

void KeplerCompute::DropParkedMacro(GPUVAddr key) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (MacroParkedOnFront(split)) {
        TakeParkedMacro(Front(split).parked, key);
        return;
    }
    const auto drop = [this, key] { TakeParkedMacro(parked_macros, key); };
    if (split.Active()) {
        split.Defer(drop, false);
    } else {
        drop();
    }
}

void KeplerCompute::FrontApply(FrontState& fs, u32 method, u32 argument) {
    // As ProcessNativeMethod does with the macro code and bindings.
    if (method == 0x45 || method == 0x47) {
        fs.compute_uploaded = true;
    }
    fs.registers[method] = argument;
    switch (method) {
    case 0x107: {
        // Front macros read this copy: an inline QMD whose destination is mapped launches through
        // ProcessNativeMethod(0xb0, 3) on the back, which leaves 3 in 0xb0 (0xad is restored).
        // Without this the copy kept the last 0xb0 the front sent.
        const GPUVAddr address = ((GPUVAddr{fs.registers[0xc6]} << 32) | fs.registers[0xc7]) << 8;
        const u32 control = fs.registers[0xa7];
        if (memory_manager.IsFullyMappedRange(address, 256) &&
            ((control & (1U << 24)) || !(fs.registers[0xcc] & (1U << 12)))) {
            fs.registers[0xb0] = 3;
        }
        break;
    }
    case 0x45:
        FrontMacroEngine(fs).ClearCode(argument);
        break;
    case 0x46:
        FrontMacroEngine(fs).AddCode(0, argument);
        break;
    case 0x47:
        fs.bind_pointer = argument;
        break;
    case 0x48: {
        const auto positions = FrontMacroPositions(fs);
        if (fs.bind_pointer < positions.size()) {
            positions[fs.bind_pointer++] = argument;
        } else {
            LOG_ERROR(HW_GPU, "Native compute macro bind out of bounds");
        }
        break;
    }
    default:
        break;
    }
}

MacroEngine& KeplerCompute::FrontMacroEngine(FrontState& fs) {
    if (native_context) {
        return fs.compute_uploaded ? native_context->compute_engine : native_context->engine;
    }
    return *fs.macro;
}

std::span<u32> KeplerCompute::FrontMacroPositions(FrontState& fs) {
    if (native_context) {
        return fs.compute_uploaded ? std::span<u32>{native_context->compute_positions}
                                      : std::span<u32>{native_context->positions};
    }
    return native_macro_positions;
}

void KeplerCompute::FrontCallMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                    u32 method, u32 argument, bool is_last_call,
                                    const GPUVAddr* segment) {
    auto& fs = Front(split);
    if (method >= fs.registers.size()) {
        split.Method(this, method, argument, is_last_call, segment);
        return;
    }
    if (method >= 0xe00) {
        if (!fs.executing_macro) {
            if (method & 1) {
                LOG_ERROR(HW_GPU, "Native compute macro starts on argument method");
                return;
            }
            fs.executing_macro = method;
        }
        fs.parameters.push_back(argument);
        if (is_last_call) {
            FrontRunMacro(system, split, fs);
        }
        return;
    }
    FrontApply(fs, method, argument);
    split.Method(this, method, argument, is_last_call, segment);
    if (method == 0x6c3) {
        FrontNoteReport(split, fs, argument);
    }
}

void KeplerCompute::FrontNoteReport(VideoCommon::GpuSplit& split, const FrontState& fs,
                                    u32 control) {
    if (!split.NotesQueuedReleases()) {
        return;
    }
    // As ProcessNativeReportSemaphore: a release or report writes the payload, or with a
    // reduction a value worked out at its fence; other operations write nothing.
    const u32 operation = control & 3;
    if (operation != 0 && operation != 2) {
        return;
    }
    const GPUVAddr address =
        (GPUVAddr{fs.registers[0x6c0] & 0x1ffffff} << 32) | fs.registers[0x6c1];
    if (memory_manager.IsUpdateQueued(address, sizeof(u32))) {
        split.ForgetQueuedReleases();
        return;
    }
    const auto device = memory_manager.GpuToCpuAddress(address);
    if (!device) {
        return;
    }
    if (((control >> 3) & 1) == 0) {
        split.NoteQueuedRelease(*device, fs.registers[0x6c2]);
    } else {
        split.ForgetQueuedRelease(*device);
    }
}

void KeplerCompute::FrontCallMultiMethod(Core::System& system, VideoCommon::GpuSplit& split,
                                         u32 method, const u32* base_start, u32 amount,
                                         u32 methods_pending, const GPUVAddr* segment) {
    auto& fs = Front(split);
    if (method >= 0xe00 && method < fs.registers.size()) {
        for (u32 i = 0; i < amount; ++i) {
            FrontCallMethod(system, split, method, base_start[i], methods_pending - i <= 1, segment);
        }
        return;
    }
    if (method < fs.registers.size()) {
        for (u32 i = 0; i < amount; ++i) {
            FrontApply(fs, method, base_start[i]);
        }
    }
    split.MultiMethod(this, method, base_start, amount, methods_pending, segment);
    // Each word is a report to the same word: the last one decides what it holds.
    if (method == 0x6c3 && amount != 0) {
        FrontNoteReport(split, fs, base_start[amount - 1]);
    }
}

// NVN2 sets a compute dispatch's grid size with a macro taking [x, y, z] that only keeps them for
// the QMD macro after it. Title 1's culling dispatches write those words in the command list each
// frame, and reading them on the CPU makes the split's front wait for the dispatch that wrote them
// and for the host GPU (a copy of the list, or a drain and flush without one). With
// EDEN_NX2_INDIRECT_DISPATCH=1 (an Ocarina of Time title default) they are dispatched from GPU
// memory instead, as Maxwell3D's FrontTakeIndirectArguments does for draws.
bool KeplerCompute::FrontTakeIndirectArguments(VideoCommon::GpuSplit& split, u32 method,
                                               GPUVAddr address, u32 words) {
    static Common::Nx2::SessionValue<bool> enabled{[] {
        return Common::Nx2::Setting("EDEN_NX2_INDIRECT_DISPATCH", "0").starts_with('1');
    }};
#ifdef ARCHITECTURE_x86_64
    constexpr bool front_runs_macros = true; // FrontRunMacro needs the macro JIT
#else
    constexpr bool front_runs_macros = false;
#endif
    if (!enabled.Get() || !front_runs_macros || !native_mode || words != 3 || method < 0xe00 ||
        (method & 1) != 0) {
        return false;
    }
    auto& fs = Front(split);
    if (fs.executing_macro) {
        return false;
    }
    auto& macro = FrontMacroEngine(fs);
    const auto positions = FrontMacroPositions(fs);
    const u32 start = positions[((method - 0xe00) / 2) % positions.size()];
    if (start != fs.indirect_macro_start ||
        macro.native_code_generation != fs.indirect_macro_generation) {
        // FNV-1a of the macro's first 16 instructions.
        constexpr u64 MacroHash = 0x864bbc6b7dc4dd82ULL;
        constexpr size_t HashedWords = 48;
        const auto code = macro.NativeCode();
        const size_t first = size_t{start} * 3;
        u64 hash = 0xcbf29ce484222325ULL;
        for (size_t i = first; i < first + HashedWords && i < code.size(); ++i) {
            hash = (hash ^ code[i]) * 0x100000001b3ULL;
        }
        fs.indirect_macro_start = start;
        fs.indirect_macro_generation = macro.native_code_generation;
        fs.indirect_macro = hash == MacroHash;
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 4) {
            LOG_INFO(HW_GPU, "Compute macro at {:#x} takes GPU-written arguments (code {:#x}): {}",
                     start, hash, fs.indirect_macro ? "dispatched indirectly" : "read on the CPU");
        }
    }
    if (!fs.indirect_macro) {
        return false;
    }
    fs.indirect_grid = address;
    return true;
}

void KeplerCompute::FrontRunMacro(Core::System& system, VideoCommon::GpuSplit& split,
                                  FrontState& fs) {
#ifdef ARCHITECTURE_x86_64
    const auto positions = FrontMacroPositions(fs);
    const u32 start = positions[((*fs.executing_macro - 0xe00) / 2) % positions.size()];
    fs.executing_macro.reset();
    auto& macro = FrontMacroEngine(fs);
    if (native_data_ram) {
        macro.native_interpreter.active_dram = native_data_ram;
    }
    if (const GPUVAddr grid = std::exchange(fs.indirect_grid, 0);
        grid != 0 && start == fs.indirect_macro_start && fs.parameters.size() == 3) {
        // The words read for this call are older than the GPU's: the macro keeps a marker for
        // the QMD's grid width, and the back dispatches with the GPU's words.
        const u32 slot = fs.indirect_grid_slot++ % static_cast<u32>(indirect_grids.size());
        fs.parameters = {IndirectGridMarker | slot, 1, 1};
        split.Defer([this, slot, grid] { indirect_grids[slot] = grid; }, false);
    }
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Macro};
    if (!macro.native_jit) {
        macro.native_jit = std::make_shared<TuringMacroJit>();
    }
    macro.native_jit->Prepare(macro.NativeCode(), macro.native_code_generation);
    const auto result = macro.native_jit->Execute(
        start, fs.parameters, std::span<u32>{*macro.native_interpreter.active_dram},
        [&fs](u32 address) {
            return address < fs.registers.size() ? fs.registers[address] : 0U;
        },
        [&](u32 address, u32 value) {
            if (address >= 0xe00) {
                return false;
            }
            FrontApply(fs, address, value);
            split.MacroWrite(this, address, value);
            if (address == 0x6c3) {
                FrontNoteReport(split, fs, value);
            }
            return true;
        });
    if (!result.success) {
        macro.native_execution_failed = true;
        LOG_ERROR(HW_GPU, "Native MME (JIT, split front) failed: compute start={:#x}, consumed={}, error={}",
                  start, result.consumed, result.error);
    }
#else
    fs.executing_macro.reset();
#endif
    fs.parameters.clear();
}

} // namespace Tegra::Engines
