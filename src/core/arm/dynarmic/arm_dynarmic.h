// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>

#include <dynarmic/interface/halt_reason.h>
#include <dynarmic/interface/optimization_flags.h>

#include "core/arm/arm_interface.h"

namespace Core {

[[nodiscard]] inline bool CpuSwitchOn(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

/// Selevet's switches for Unsafe and Auto CPU accuracy, which add these unsafe optimizations
/// (Accurate adds none): EDEN_CPU_ACCURATE_FLOAT=1 drops the reduced-error, NaN and FPCR ones,
/// EDEN_CPU_FUSED_FMA=1 keeps multiply-adds fused and EDEN_CPU_GLOBAL_MONITOR=1 keeps exclusive
/// accesses checked across cores.
inline void ApplyCpuFloatSwitches(Dynarmic::OptimizationFlag& optimizations) {
    static const bool accurate_float = CpuSwitchOn("EDEN_CPU_ACCURATE_FLOAT");
    static const bool fused_fma = CpuSwitchOn("EDEN_CPU_FUSED_FMA");
    static const bool global_monitor = CpuSwitchOn("EDEN_CPU_GLOBAL_MONITOR");
    if (accurate_float) {
        optimizations &= ~(Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                           Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                           Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue);
    }
    if (fused_fma) {
        optimizations &= ~Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
    }
    if (global_monitor) {
        optimizations &= ~Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    }
}

constexpr Dynarmic::HaltReason StepThread = Dynarmic::HaltReason::Step;
constexpr Dynarmic::HaltReason DataAbort = Dynarmic::HaltReason::MemoryAbort;
constexpr Dynarmic::HaltReason BreakLoop = Dynarmic::HaltReason::UserDefined2;
constexpr Dynarmic::HaltReason SupervisorCall = Dynarmic::HaltReason::UserDefined3;
constexpr Dynarmic::HaltReason InstructionBreakpoint = Dynarmic::HaltReason::UserDefined4;
constexpr Dynarmic::HaltReason PrefetchAbort = Dynarmic::HaltReason::UserDefined6;

constexpr HaltReason TranslateHaltReason(Dynarmic::HaltReason hr) {
    static_assert(u64(HaltReason::StepThread) == u64(StepThread));
    static_assert(u64(HaltReason::DataAbort) == u64(DataAbort));
    static_assert(u64(HaltReason::BreakLoop) == u64(BreakLoop));
    static_assert(u64(HaltReason::SupervisorCall) == u64(SupervisorCall));
    static_assert(u64(HaltReason::InstructionBreakpoint) == u64(InstructionBreakpoint));
    static_assert(u64(HaltReason::PrefetchAbort) == u64(PrefetchAbort));
    return HaltReason(hr);
}

} // namespace Core
