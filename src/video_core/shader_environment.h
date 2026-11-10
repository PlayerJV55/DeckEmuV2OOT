// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/unique_function.h"
#include "shader_recompiler/environment.h"
#include "video_core/engines/maxwell_3d.h"

namespace Tegra {
class Memorymanager;
}

namespace VideoCommon {

/// A native (Switch 2) program after capture and lowering to Maxwell code. The shader cache keeps
/// it so the environments built for the program's pipelines don't capture and lower it again.
struct LoweredNativeCode {
    /// Shader header, lowered instructions with Maxwell scheduling words, then the native
    /// address map (what the environment's `code` used to be filled with).
    std::vector<u64> code;
    /// Bytes of the native program that were read, header included.
    size_t source_size{};
    /// Constant buffer holding the texture handles if the program requires one, otherwise ~0U
    /// (the engine's register value is used).
    u32 texture_bound = ~0U;
    /// Shader::Ampere::NativeProgramKey of the native program.
    u64 native_program_key{};
};

struct NativeProgramInputs;

/// Lowers a recorded native program from its inputs alone, as its environment would at the
/// program's first bind (EDEN_NX2_NATIVE_PROGRAMS). Null when the lowering fails.
[[nodiscard]] std::shared_ptr<const LoweredNativeCode> LowerNativeProgram(
    const NativeProgramInputs& inputs);

/// EDEN_NX2_PIPELINE_ASYNC: what translating a graphics pipeline's programs reads from the 3D
/// engine and from guest memory besides the program code, copied on the GPU thread when the build
/// is queued. The translation then runs on a worker without touching Maxwell3D or the memory
/// manager, which the GPU thread keeps changing. A read the copy cannot answer marks the
/// environment (AsyncSnapshotMissed) and the pipeline is built again on the GPU thread.
struct AsyncGraphicsSnapshot {
    struct ConstBuffer {
        bool enabled{};
        u32 size{}; ///< As bound: reads at or past it return 0, as on the GPU thread
        /// Bytes from the buffer's start: min(size, MaxConstBufferSize) rounded up to 8, plus 8
        /// (a u32 read starting just below size reads past it).
        std::vector<u8> bytes;
        /// Per 8-byte slot: the u64 there (as ReadCbufValue returns it) is a mapped GPU address.
        std::vector<bool> pointer_mapped;
    };
    struct Stage {
        std::array<ConstBuffer, Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers> const_buffers;
    };
    std::array<Stage, Tegra::Engines::Maxwell3D::Regs::MaxShaderStage> stages;
    /// The first entries of the texture header pool (all of it up to MaxTicBytes).
    std::vector<Tegra::Texture::TICEntry> tic;
    u32 tic_limit{};
    bool via_header_index{};
    bool native_tiling{};
    u32 viewport_transform_state{};
    ::Common::unordered_map<u64, Tegra::Engines::Maxwell3D::HLEReplacementAttributeType>
        replace_table;
    size_t bytes{}; ///< Copied, for the statistics

    /// A larger header pool is copied up to here; a handle past it fails the build.
    static constexpr size_t MaxTicBytes = 4ULL << 20;
};

class GenericEnvironment : public Shader::Environment {
public:
    explicit GenericEnvironment() = default;
    explicit GenericEnvironment(Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                u32 start_address_);

    ~GenericEnvironment() override;

    [[nodiscard]] u32 TextureBoundBuffer() const final;

    [[nodiscard]] u32 LocalMemorySize() const final;

    [[nodiscard]] u32 SharedMemorySize() const final;

    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const final;

    [[nodiscard]] u64 ReadInstruction(u32 address) final;

    [[nodiscard]] u32 TranslateProgramAddress(u32 address) const override;

    [[nodiscard]] bool IsNativeProgram() const override;
    [[nodiscard]] bool HasNativeInstructions() const override {
        return IsNativeProgram();
    }

    [[nodiscard]] bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;

    [[nodiscard]] bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset,
                                                          u32 indirect) override;

    /// Native programs: the (index, offset) slots classified as non-pointers because they were
    /// null at translation
    [[nodiscard]] std::vector<std::pair<u32, u32>> NullStoragePointerSlots() const;

    /// Native programs: the classified slots bound as storage buffer pointers, as the sorted keys
    /// of their classification (direct and pointer-table slots)
    [[nodiscard]] std::vector<u64> MappedStoragePointerKeys() const;

    /// Answers the classification of these keys (see MappedStoragePointerKeys) as storage buffer
    /// pointers, whatever the slots hold now; called before the program is translated
    void KeepStoragePointers(std::span<const u64> keys);

    [[nodiscard]] std::optional<u64> Analyze();

    void SetCachedSize(size_t size_bytes);

    [[nodiscard]] size_t CachedSizeWords() const noexcept;

    [[nodiscard]] size_t CachedSizeBytes() const noexcept;

    [[nodiscard]] size_t ReadSizeBytes() const noexcept;

    [[nodiscard]] size_t SourceSizeBytes() const noexcept {
        return native_source_size;
    }

    [[nodiscard]] const std::shared_ptr<const LoweredNativeCode>& LoweredNative() const noexcept {
        return lowered_native;
    }

    /// EDEN_NX2_GPU_PROFILE: nanoseconds spent capturing and lowering the native program here,
    /// zero when the environment got it lowered or without the profile
    [[nodiscard]] u64 NativeCaptureNs() const noexcept {
        return native_capture_ns;
    }
    [[nodiscard]] u64 NativeLowerNs() const noexcept {
        return native_lower_ns;
    }
    /// EDEN_NX2_GPU_PROFILE: the native program was lowered ahead from a record
    [[nodiscard]] bool NativeFromRecord() const noexcept {
        return native_from_record;
    }

    [[nodiscard]] bool CanBeSerialized() const noexcept;

    /// EDEN_NX2_PIPELINE_ASYNC: a read during the translation on a worker that the snapshot
    /// could not answer; the result must be discarded.
    [[nodiscard]] bool AsyncSnapshotMissed() const noexcept {
        return async_missed;
    }

    [[nodiscard]] u64 CalculateHash() const;

    void Dump(u64 pipeline_hash, u64 shader_hash) override;

    void Serialize(std::ostream& file) const;

    bool HasHLEMacroState() const override {
        return has_hle_engine_state;
    }

protected:
    std::optional<u64> TryFindSize();

    /// Adopts an already lowered native program, or captures and lowers it when there is none.
    void InitializeNativeCode(std::shared_ptr<const LoweredNativeCode> lowered,
                              u32 source_header_size = 128, bool geometry = false);
    std::shared_ptr<const LoweredNativeCode> LowerNativeCode(u32 source_header_size,
                                                          bool geometry = false);
    size_t native_source_size{};
    u64 native_capture_ns{};
    u64 native_lower_ns{};
    bool native_from_record{};
    /// Reject matrix multiplies (KeplerCompute::EmulatesNativeTensorPrograms).
    bool reject_native_matrix{};
    std::shared_ptr<const LoweredNativeCode> lowered_native;

    Tegra::Texture::TICEntry ReadTextureInfo(GPUVAddr tic_addr, u32 tic_limit,
                                             bool via_header_index, u32 raw);

    Tegra::MemoryManager* gpu_memory{};
    GPUVAddr program_base{};

    std::vector<u64> code;
    ::Common::unordered_map<u32, Shader::TextureType> texture_types;
    ::Common::unordered_map<u32, Shader::TexturePixelFormat> texture_pixel_formats;
    ::Common::unordered_map<u32, Tegra::Texture::TICEntry> texture_descriptors;
    ::Common::unordered_map<u64, u32> cbuf_values;
    ::Common::unordered_map<u64, u8> storage_pointer_types;
    ::Common::unordered_map<u64, Shader::ReplaceConstant> cbuf_replacements;

    u32 local_memory_size{};
    u32 texture_bound{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};

    u32 read_lowest = (std::numeric_limits<u32>::max)();
    u32 read_highest = 0;

    u32 cached_lowest = (std::numeric_limits<u32>::max)();
    u32 cached_highest = 0;
    u32 initial_offset = 0;

    u32 viewport_transform_state = 1;

    bool has_unbound_instructions = false;
    bool has_hle_engine_state = false;

    /// EDEN_NX2_PIPELINE_ASYNC: set while the program is translated off the GPU thread. Reads
    /// are then answered from the snapshot only; one it cannot answer sets async_missed.
    [[nodiscard]] bool AsyncPointerMapped(u32 index, u32 offset, u64 address);
    std::shared_ptr<const AsyncGraphicsSnapshot> async_snapshot;
    const AsyncGraphicsSnapshot::Stage* async_stage{};
    bool async_missed = false;
};

class GraphicsEnvironment final : public GenericEnvironment {
public:
    explicit GraphicsEnvironment() = default;
    explicit GraphicsEnvironment(Tegra::Engines::Maxwell3D& maxwell3d_,
                                 Tegra::MemoryManager& gpu_memory_,
                                 Tegra::Engines::Maxwell3D::Regs::ShaderType program,
                                 GPUVAddr program_base_, u32 start_address_,
                                 std::shared_ptr<const LoweredNativeCode> lowered_native_ = {});

    ~GraphicsEnvironment() override = default;

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    Shader::TextureType ReadTextureType(u32 handle) override;

    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    bool IsTexturePixelFormatInteger(u32 handle) override;

    u32 ReadViewportTransformState() override;

    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override;

    /// EDEN_NX2_PIPELINE_ASYNC: copies, on the GPU thread, what translating these environments'
    /// programs reads besides their code (which SetCachedSize already holds), and makes them read
    /// only the copy from then on. The environments must come from the same engine state.
    static std::shared_ptr<const AsyncGraphicsSnapshot> SnapshotForAsync(
        std::span<GraphicsEnvironment* const> envs);

private:
    [[nodiscard]] Tegra::Texture::TICEntry ReadAsyncTextureInfo(u32 handle);

    Tegra::Engines::Maxwell3D* maxwell3d{};
    size_t stage_index{};
};

class ComputeEnvironment final : public GenericEnvironment {
public:
    explicit ComputeEnvironment() = default;
    explicit ComputeEnvironment(Tegra::Engines::KeplerCompute& kepler_compute_,
                                Tegra::MemoryManager& gpu_memory_, GPUVAddr program_base_,
                                u32 start_address_,
                                std::shared_ptr<const LoweredNativeCode> lowered_native_ = {});

    ~ComputeEnvironment() override = default;

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    Shader::TextureType ReadTextureType(u32 handle) override;

    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    bool IsTexturePixelFormatInteger(u32 handle) override;

    u32 ReadViewportTransformState() override;

    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(
        [[maybe_unused]] u32 bank, [[maybe_unused]] u32 offset) override {
        return std::nullopt;
    }

private:
    Tegra::Engines::KeplerCompute* kepler_compute{};
};

/// Answers for reads a FileEnvironment has no record of (the compute warm-up: programs from the
/// game files, never run, converted only to fill the driver's cache). Without it a missing read
/// is an error.
struct GuessMissing {
    u32 cbuf_bank0{0x10000000};  ///< Constant buffer 0 (driver constants, descriptor handles)
    u32 cbuf_other{0};           ///< Other constant buffers (game constants)
    Shader::TextureType texture_type{Shader::TextureType::ColorArray2D};
    Shader::TexturePixelFormat pixel_format{static_cast<Shader::TexturePixelFormat>(16)};
    bool storage_pointer_bank0{true};  ///< Bank 0 always held storage pointers in Lichii's records
    bool storage_pointer_other{false}; ///< Banks 1-4 never did
};

class FileEnvironment final : public Shader::Environment {
public:
    FileEnvironment() = default;
    ~FileEnvironment() override = default;

    /// Missing reads get these answers instead of an error; the guess must outlive the
    /// environment.
    void SetGuessMissing(const GuessMissing* guess_) noexcept {
        guess = guess_;
    }

    FileEnvironment& operator=(FileEnvironment&&) noexcept = default;
    FileEnvironment(FileEnvironment&&) noexcept = default;

    FileEnvironment& operator=(const FileEnvironment&) = delete;
    FileEnvironment(const FileEnvironment&) = delete;

    void Deserialize(std::istream& file);

    /// A translation asked for a value the record doesn't hold (a build that reads more than the
    /// one that recorded it); the read failed.
    [[nodiscard]] bool LacksRecordedValue() const noexcept {
        return lacks_recorded_value;
    }

    [[nodiscard]] u64 ReadInstruction(u32 address) override;

    [[nodiscard]] u32 TranslateProgramAddress(u32 address) const override;

    [[nodiscard]] bool IsNativeProgram() const override;
    [[nodiscard]] bool HasNativeInstructions() const override {
        return IsNativeProgram();
    }

    [[nodiscard]] bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;

    [[nodiscard]] bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset,
                                                          u32 indirect) override;

    [[nodiscard]] std::vector<std::pair<u32, u32>> NullStoragePointerSlots() const;

    [[nodiscard]] std::vector<u64> MappedStoragePointerKeys() const;

    [[nodiscard]] u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) override;

    [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    [[nodiscard]] bool IsTexturePixelFormatInteger(u32 handle) override;

    [[nodiscard]] u32 ReadViewportTransformState() override;

    [[nodiscard]] u32 LocalMemorySize() const override;

    [[nodiscard]] u32 SharedMemorySize() const override;

    [[nodiscard]] u32 TextureBoundBuffer() const override;

    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const override;

    [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank,
                                                                               u32 offset) override;

    [[nodiscard]] bool HasHLEMacroState() const override {
        return cbuf_replacements.size() != 0;
    }

    void Dump(u64 pipeline_hash, u64 shader_hash) override;

private:
    std::vector<u64> code;
    ::Common::unordered_map<u32, Shader::TextureType> texture_types;
    ::Common::unordered_map<u32, Shader::TexturePixelFormat> texture_pixel_formats;
    ::Common::unordered_map<u64, u32> cbuf_values;
    ::Common::unordered_map<u64, u8> storage_pointer_types;
    ::Common::unordered_map<u64, Shader::ReplaceConstant> cbuf_replacements;
    std::array<u32, 3> workgroup_size{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    u32 texture_bound{};
    u32 read_lowest{};
    u32 read_highest{};
    u32 initial_offset{};
    u32 viewport_transform_state = 1;
    const GuessMissing* guess{};
    bool lacks_recorded_value{};
};

/// A native program found in the game's files (Shader::Ampere::ScanNativePrograms), lowered as
/// its bind would lower it and translated before the game binds it (Prepare Shaders' precompile).
/// What a bound program reads from the GPU's state is answered from `guess`. Only for builds that
/// are thrown away: a draw never takes a pipeline translated from it.
class ScannedEnvironment final : public Shader::Environment {
public:
    ScannedEnvironment() = default;
    ~ScannedEnvironment() override = default;

    /// code: the program's instructions; header: the 0x50-byte program header of a graphics
    /// program, empty for compute. Null when the lowering rejects the program. `guess` must
    /// outlive the environment.
    [[nodiscard]] static std::unique_ptr<ScannedEnvironment> Create(std::span<const u8> code,
                                                                    std::span<const u8> header,
                                                                    Shader::Stage stage,
                                                                    const GuessMissing& guess,
                                                                    bool reject_native_matrix);

    [[nodiscard]] u64 ReadInstruction(u32 address) override;

    [[nodiscard]] u32 TranslateProgramAddress(u32 address) const override;

    [[nodiscard]] bool IsNativeProgram() const override {
        return true;
    }
    [[nodiscard]] bool HasNativeInstructions() const override {
        return true;
    }

    [[nodiscard]] bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;

    [[nodiscard]] bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset,
                                                          u32 indirect) override;

    [[nodiscard]] u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;

    [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) override;

    [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;

    [[nodiscard]] bool IsTexturePixelFormatInteger(u32 handle) override;

    [[nodiscard]] u32 ReadViewportTransformState() override;

    [[nodiscard]] u32 LocalMemorySize() const override;

    [[nodiscard]] u32 SharedMemorySize() const override;

    [[nodiscard]] u32 TextureBoundBuffer() const override;

    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const override;

    [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank,
                                                                               u32 offset) override;

    [[nodiscard]] bool HasHLEMacroState() const override {
        return false;
    }

    void Dump(u64, u64) override {}

    /// Compute programs: what a dispatch gives them (from their shader control section).
    void SetComputeDispatch(const std::array<u32, 3>& workgroup, u32 shared_memory,
                            u32 local_memory);

    /// The shader hash pipeline keys hold for this program, as GenericEnvironment::Analyze gives
    /// it for the bound program.
    [[nodiscard]] u64 Hash() const;

private:
    std::shared_ptr<const LoweredNativeCode> lowered;
    const GuessMissing* guess{};
    u32 texture_bound{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};
};

/// EDEN_NX2_CACHE_REPAIR (on by default): a pipeline cache with a cut-off or damaged record keeps
/// the records before it instead of being deleted.
[[nodiscard]] bool PipelineCacheRepair();

void SerializePipeline(std::span<const char> key, std::span<const GenericEnvironment* const> envs,
                       const std::filesystem::path& filename, u32 cache_version);

template <typename Key, typename Envs>
    requires std::is_trivially_copyable_v<Key>
        && std::has_unique_object_representations_v<Key>
void SerializePipeline(const Key& key, const Envs& envs, const std::filesystem::path& filename, u32 cache_version) {
    SerializePipeline(std::span(reinterpret_cast<const char*>(&key), sizeof(key)), std::span(envs.data(), envs.size()), filename, cache_version);
}

/// The bytes SerializePipeline appends for a pipeline, for shader cache v2's records. Empty when an
/// environment can't be serialized.
[[nodiscard]] std::vector<u8> PipelineRecordBytes(std::span<const char> key,
                                                std::span<const GenericEnvironment* const> envs);

template <typename Key, typename Envs>
    requires std::is_trivially_copyable_v<Key> && std::has_unique_object_representations_v<Key>
std::vector<u8> PipelineRecordBytes(const Key& key, const Envs& envs) {
    return PipelineRecordBytes(std::span(reinterpret_cast<const char*>(&key), sizeof(key)),
                               std::span(envs.data(), envs.size()));
}

using LoadComputeFn = Common::UniqueFunction<void, std::istream&, FileEnvironment>;
using LoadGraphicsFn = Common::UniqueFunction<void, std::istream&, std::vector<FileEnvironment>>;

/// Reads one pipeline record of LoadPipelines' files from `file`, which throws on a failed read:
/// its environments, then `load_compute` or `load_graphics` reads the key that follows them.
void ReadPipelineRecord(std::istream& file, const LoadComputeFn& load_compute,
                        const LoadGraphicsFn& load_graphics);

void LoadPipelines(std::stop_token stop_loading, const std::filesystem::path& filename, u32 expected_cache_version,
    LoadComputeFn load_compute, LoadGraphicsFn load_graphics);

} // namespace VideoCommon
