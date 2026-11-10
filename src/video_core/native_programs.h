// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "shader_recompiler/program_header.h"

namespace VideoCommon {

struct LoweredNativeCode;

namespace ShaderCacheV2 {
class Writer;
}

/// What lowering a captured native program depends on besides this build's lowering
/// (LowerNativeProgram): the program is lowered again from it alone, without guest memory.
struct NativeProgramInputs {
    std::vector<Shader::Ampere::Instruction> native; ///< As captured
    Shader::ProgramHeader sph{};
    u32 source_header_size{};
    bool geometry{};
    bool reject_native_matrix{};
    /// Where the code is in guest memory. Only geometry programs loading the program counter
    /// read it, so only theirs is part of the record key.
    u64 native_code_address{};
};

/// EDEN_NX2_NATIVE_PROGRAMS (off by default): the native programs a session lowers on the GPU
/// thread are recorded in shader cache v2's log, and later sessions lower them on the pipeline
/// workers at boot, so that a program's first bind takes it from there. Every boot lowers them
/// with this build's lowering, so a lowering change never leaves a stale program behind.
namespace NativePrograms {

[[nodiscard]] bool Enabled();

/// The record key of a program: a hash of its instructions and inputs
[[nodiscard]] u64 Key(const NativeProgramInputs& inputs);

/// A Program record's payload
[[nodiscard]] std::vector<u8> Encode(const NativeProgramInputs& inputs);

/// The inputs of a Program record, or nullopt if its layout or payload isn't of this format
[[nodiscard]] std::optional<NativeProgramInputs> Decode(u16 layout, std::span<const u8> payload);

/// Starts a title's records: `writer` appends the programs lowered at their first bind that none
/// of the records (keys `recorded`) holds.
void Begin(ShaderCacheV2::Writer& writer, std::unordered_set<u64> recorded);

/// A record's program lowered ahead of its first bind in `ns`; null when its lowering failed
void Add(u64 key, std::shared_ptr<const LoweredNativeCode> code, u64 ns);

/// Logs what the boot lowered ahead of the first binds
void ReportBoot();

/// Logs what the records saved this session and lets the writer go, before it is destroyed
void End();

/// Between Begin and End
[[nodiscard]] bool Active();

/// The program lowered ahead for this key, or null
[[nodiscard]] std::shared_ptr<const LoweredNativeCode> Find(u64 key);

/// A program the GPU thread lowered at its first bind: recorded once
void Record(u64 key, const NativeProgramInputs& inputs);

} // namespace NativePrograms

} // namespace VideoCommon
