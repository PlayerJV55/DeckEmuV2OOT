// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "common/cityhash.h"
#include "common/logging.h"
#include "common/nx2_title_defaults.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "video_core/native_programs.h"
#include "video_core/shader_cache_v2.h"
#include "video_core/shader_environment.h"

namespace VideoCommon::NativePrograms {
namespace {

using Shader::Ampere::Instruction;

/// Program record payload format, kept in the record's layout field
constexpr u16 PayloadVersion = 1;
/// u32 flags, u32 source header size, u32 instruction count, u32 zero, u64 code address, the
/// shader header; the instructions follow.
constexpr size_t FixedSize = 4 * sizeof(u32) + sizeof(u64) + sizeof(Shader::ProgramHeader);
constexpr size_t HeaderOffset = 4 * sizeof(u32) + sizeof(u64);
constexpr u32 GeometryFlag = 1;
constexpr u32 RejectMatrixFlag = 2;

struct State {
    std::mutex mutex;
    ShaderCacheV2::Writer* writer{};
    std::unordered_map<u64, std::shared_ptr<const LoweredNativeCode>> lowered;
    std::unordered_set<u64> recorded;
    size_t records{};
    u64 ahead{};
    u64 ahead_ns{};
    u64 ahead_bytes{};
    u64 failed{};
    u64 taken{};
    u64 added{};
};

State& GetState() {
    static State state;
    return state;
}

std::atomic<bool> g_active{false};

u32 Flags(const NativeProgramInputs& inputs) {
    return (inputs.geometry ? GeometryFlag : 0u) |
           (inputs.reject_native_matrix ? RejectMatrixFlag : 0u);
}

/// Geometry programs that load the program counter (LowerToMaxwell's opcode 0x34e) read where
/// their code is
bool LoadsProgramCounter(const NativeProgramInputs& inputs) {
    return inputs.geometry && std::ranges::any_of(inputs.native, [](const Instruction& instruction) {
               return instruction.Bits(0, 12) == 0x34e;
           });
}

template <typename T>
void Put(std::vector<u8>& out, const T& value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

template <typename T>
T Get(std::span<const u8> bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

} // Anonymous namespace

bool Enabled() {
    return Common::Nx2::Setting("EDEN_NX2_NATIVE_PROGRAMS", "0") == "1";
}

u64 Key(const NativeProgramInputs& inputs) {
    std::array<u8, sizeof(Shader::ProgramHeader) + 2 * sizeof(u32) + sizeof(u64)> fixed{};
    const u32 flags = Flags(inputs);
    const u64 address = LoadsProgramCounter(inputs) ? inputs.native_code_address : 0;
    std::memcpy(fixed.data(), &inputs.sph, sizeof(inputs.sph));
    std::memcpy(fixed.data() + sizeof(inputs.sph), &inputs.source_header_size, sizeof(u32));
    std::memcpy(fixed.data() + sizeof(inputs.sph) + sizeof(u32), &flags, sizeof(u32));
    std::memcpy(fixed.data() + sizeof(inputs.sph) + 2 * sizeof(u32), &address, sizeof(u64));
    const u64 seed = Common::CityHash64(reinterpret_cast<const char*>(fixed.data()), fixed.size());
    return Common::CityHash64WithSeed(reinterpret_cast<const char*>(inputs.native.data()),
                                      inputs.native.size() * sizeof(Instruction), seed);
}

std::vector<u8> Encode(const NativeProgramInputs& inputs) {
    std::vector<u8> out;
    out.reserve(FixedSize + inputs.native.size() * sizeof(Instruction));
    Put<u32>(out, Flags(inputs));
    Put<u32>(out, inputs.source_header_size);
    Put<u32>(out, static_cast<u32>(inputs.native.size()));
    Put<u32>(out, 0);
    Put<u64>(out, inputs.native_code_address);
    Put(out, inputs.sph);
    const size_t at = out.size();
    out.resize(at + inputs.native.size() * sizeof(Instruction));
    std::memcpy(out.data() + at, inputs.native.data(), inputs.native.size() * sizeof(Instruction));
    return out;
}

std::optional<NativeProgramInputs> Decode(u16 layout, std::span<const u8> payload) {
    if (layout != PayloadVersion || payload.size() < FixedSize) {
        return std::nullopt;
    }
    const u32 flags = Get<u32>(payload, 0);
    const u32 count = Get<u32>(payload, 8);
    if (count == 0 || count > Shader::Ampere::ProgramCaptureState::MaximumInstructions ||
        payload.size() != FixedSize + size_t{count} * sizeof(Instruction)) {
        return std::nullopt;
    }
    NativeProgramInputs inputs;
    inputs.geometry = (flags & GeometryFlag) != 0;
    inputs.reject_native_matrix = (flags & RejectMatrixFlag) != 0;
    inputs.source_header_size = Get<u32>(payload, 4);
    inputs.native_code_address = Get<u64>(payload, 16);
    std::memcpy(&inputs.sph, payload.data() + HeaderOffset, sizeof(inputs.sph));
    inputs.native.resize(count);
    std::memcpy(inputs.native.data(), payload.data() + FixedSize,
                size_t{count} * sizeof(Instruction));
    return inputs;
}

void Begin(ShaderCacheV2::Writer& writer, std::unordered_set<u64> recorded) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.writer = &writer;
    state.lowered.clear();
    state.recorded = std::move(recorded);
    state.records = state.recorded.size();
    state.ahead = state.ahead_ns = state.ahead_bytes = state.failed = state.taken = state.added = 0;
    g_active.store(true, std::memory_order_release);
}

void Add(u64 key, std::shared_ptr<const LoweredNativeCode> code, u64 ns) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.writer == nullptr) {
        return;
    }
    if (!code) {
        ++state.failed;
        return;
    }
    ++state.ahead;
    state.ahead_ns += ns;
    state.ahead_bytes += code->code.size() * sizeof(u64);
    state.lowered.insert_or_assign(key, std::move(code));
}

void ReportBoot() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.writer == nullptr) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Native program records: {} read, {} lowered ahead of their first bind ({:.1f} ms on "
             "the workers, {:.1f} MiB kept), {} failed to lower",
             state.records, state.ahead, static_cast<double>(state.ahead_ns) / 1e6,
             static_cast<double>(state.ahead_bytes) / (1024.0 * 1024.0), state.failed);
}

void End() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.writer == nullptr) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Native program records this session: {} first binds took a program lowered ahead, "
             "{} programs lowered on the GPU thread were recorded",
             state.taken, state.added);
    g_active.store(false, std::memory_order_release);
    state.writer = nullptr;
    state.lowered.clear();
    state.recorded.clear();
}

bool Active() {
    return g_active.load(std::memory_order_acquire);
}

std::shared_ptr<const LoweredNativeCode> Find(u64 key) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    const auto it = state.lowered.find(key);
    if (it == state.lowered.end()) {
        return {};
    }
    ++state.taken;
    return it->second;
}

void Record(u64 key, const NativeProgramInputs& inputs) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.writer == nullptr || !state.recorded.insert(key).second) {
        return;
    }
    state.writer->Append(ShaderCacheV2::RecordKind::Program, key, Encode(inputs), PayloadVersion);
    ++state.added;
}

} // namespace VideoCommon::NativePrograms
