// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <span>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/fs/path_util.h"
#include "shader_recompiler/stage.h"
#include "video_core/renderer_vulkan/pipeline_cache_file.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_cache_v2.h"
#include "video_core/shader_environment.h"

namespace Vulkan::PipelineCacheFile {
namespace {

constexpr std::array<char, 8> MagicNumber{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
constexpr size_t HeaderSize = MagicNumber.size() + sizeof(u32);

struct Record {
    std::streamoff begin{};
    std::streamoff end{};
    bool mesh{};
    bool compute{};
    std::string key;
};

// Visits the records of a cache of an importable version in order. Returns false if one couldn't
// be read (the ones before it were visited) or the file isn't such a cache.
bool Walk(const std::filesystem::path& path, const std::function<void(Record&&)>& visit) {
    const std::optional<u32> version = ReadVersion(path);
    if (!version || !IsImportable(*version)) {
        return false;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    try {
        file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        const std::streamoff end = file.tellg();
        file.seekg(static_cast<std::streamoff>(HeaderSize), std::ios::beg);
        while (file.tellg() != end) {
            Record record{.begin = file.tellg()};
            u32 num_envs{};
            file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
            if (num_envs == 0 || num_envs > Maxwell::MaxShaderProgram) {
                return false;
            }
            std::vector<VideoCommon::FileEnvironment> envs(num_envs);
            for (auto& env : envs) {
                env.Deserialize(file);
            }
            // Another lineage's cache of a listed version may be laid out differently: stop at
            // the first record that doesn't read as one rather than import misread pipelines.
            if (std::ranges::any_of(envs, [](const auto& env) {
                    return env.ShaderStage() > Shader::Stage::Mesh;
                })) {
                return false;
            }
            record.compute = envs.front().ShaderStage() == Shader::Stage::Compute;
            if (record.compute && num_envs != 1) {
                return false;
            }
            record.mesh = std::ranges::any_of(envs, [](const auto& env) {
                return env.ShaderStage() == Shader::Stage::Task ||
                       env.ShaderStage() == Shader::Stage::Mesh;
            });
            record.key.resize(record.compute ? sizeof(ComputePipelineCacheKey)
                                             : sizeof(GraphicsPipelineCacheKey));
            file.read(record.key.data(), static_cast<std::streamsize>(record.key.size()));
            record.end = file.tellg();
            visit(std::move(record));
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

} // Anonymous namespace

std::optional<u32> ReadVersion(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::array<char, 8> magic{};
    u32 version{};
    if (!file.read(magic.data(), magic.size()) ||
        !file.read(reinterpret_cast<char*>(&version), sizeof(version)) || magic != MagicNumber) {
        return std::nullopt;
    }
    return version;
}

Contents Inspect(const std::filesystem::path& path) {
    Contents contents;
    contents.damaged = !Walk(path, [&contents](Record&& record) {
        ++(record.mesh ? contents.mesh : record.compute ? contents.compute : contents.graphics);
    });
    return contents;
}

MergeResult Merge(const std::filesystem::path& source, const std::filesystem::path& game_dir) {
    MergeResult result;
    const std::optional<u32> version = ReadVersion(source);
    if (!version) {
        result.error = "not a pipeline cache";
        return result;
    }
    if (!IsImportable(*version)) {
        result.error = "cache version " + std::to_string(*version);
        return result;
    }
    const std::array targets{game_dir / "vulkan.bin", game_dir / "vulkan_mesh.bin"};
    std::unordered_set<std::string> known;
    for (const auto& target : targets) {
        std::error_code error;
        if (!std::filesystem::exists(target, error)) {
            continue;
        }
        if (ReadVersion(target) != CurrentVersion) {
            result.error = "the game's own cache is of another version";
            return result;
        }
        (void)Walk(target, [&known](Record&& record) { known.insert(std::move(record.key)); });
    }
    std::vector<Record> records;
    result.source_damaged = !Walk(source, [&](Record&& record) {
        if (!known.insert(record.key).second) {
            ++result.duplicates;
            return;
        }
        records.push_back(std::move(record));
    });
    if (result.source_damaged && *version != CurrentVersion) {
        // A Lichii version's file that stops reading may be another fork's layout: what came
        // before the failure may be misread too.
        result.error = "the file doesn't read as a Lichii cache of version " +
                       std::to_string(*version);
        return result;
    }
    if (records.empty()) {
        return result;
    }
    std::error_code error;
    std::filesystem::create_directories(game_dir, error);
    std::ifstream input(source, std::ios::binary);
    std::array<std::ofstream, 2> outputs;
    std::vector<char> bytes;
    for (const Record& record : records) {
        std::ofstream& output = outputs[record.mesh ? 1 : 0];
        if (!output.is_open()) {
            const auto& target = targets[record.mesh ? 1 : 0];
            output.open(target, std::ios::binary | std::ios::app | std::ios::ate);
            if (!output.is_open()) {
                result.error = "can't write to the game's cache";
                return result;
            }
            if (output.tellp() == 0) {
                const u32 current = CurrentVersion;
                output.write(MagicNumber.data(), MagicNumber.size())
                    .write(reinterpret_cast<const char*>(&current), sizeof(current));
            }
        }
        bytes.resize(static_cast<size_t>(record.end - record.begin));
        input.seekg(record.begin);
        if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
            !output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
            result.error = "the copy failed after " + std::to_string(result.added) + " pipelines";
            return result;
        }
        ++result.added;
    }
    return result;
}

u64 GraphicsLogKey(const GraphicsPipelineCacheKey& key) {
    GraphicsPipelineCacheKey cleared{key};
    cleared.state.driver_id = 0;
    cleared.state.driver_version = 0;
    return cleared.Hash();
}

namespace {

namespace CacheV2 = VideoCommon::ShaderCacheV2;

/// The older cache files of a game that shader cache v2 takes in, in the order it reads them
std::vector<std::filesystem::path> LogSources(const std::filesystem::path& game_dir) {
    std::vector<std::filesystem::path> sources{game_dir / "vulkan.bin", game_dir / "vulkan_mesh.bin"};
    for (const u32 version : SameLayoutVersions) {
        sources.push_back(game_dir / ("vulkan.v" + std::to_string(version) + ".bin"));
        sources.push_back(game_dir / ("vulkan_mesh.v" + std::to_string(version) + ".bin"));
    }
    return sources;
}

struct PipelineBytes {
    bool compute{};
    bool mesh{};
    u64 log_key{};             ///< Its shader cache v2 record key
    std::span<const u8> bytes; ///< The record as vulkan.bin holds it
};

/// Visits the pipelines of a cache of an importable version as Walk does, with their bytes.
bool WalkPipelines(const std::filesystem::path& path,
                   const std::function<void(const PipelineBytes&)>& visit) {
    std::ifstream input(path, std::ios::binary);
    std::vector<u8> bytes;
    return Walk(path, [&](Record&& record) {
        bytes.resize(static_cast<size_t>(record.end - record.begin));
        input.clear();
        input.seekg(record.begin);
        if (!input.read(reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()))) {
            return;
        }
        u64 log_key = CacheV2::Hash64(bytes);
        if (!record.compute) {
            GraphicsPipelineCacheKey key;
            std::memcpy(&key, record.key.data(), sizeof(key));
            log_key = GraphicsLogKey(key);
        }
        visit({.compute = record.compute, .mesh = record.mesh, .log_key = log_key, .bytes = bytes});
    });
}

/// Whether the boot load reads this log record: a pipeline of a layout this build reads
bool IsLoadedRecord(const CacheV2::LogRecord& record) {
    return (record.kind == CacheV2::RecordKind::Graphics ||
            record.kind == CacheV2::RecordKind::Compute) &&
           IsLogRecordLayout(record.layout);
}

/// Whether a log record's pipeline has task or mesh programs
bool IsMeshRecord(std::span<const u8> payload) {
    std::istringstream stream{
        std::string{reinterpret_cast<const char*>(payload.data()), payload.size()},
        std::ios::binary};
    try {
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        u32 num_envs{};
        stream.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
        if (num_envs > Maxwell::MaxShaderProgram) {
            return false;
        }
        for (u32 index = 0; index < num_envs; ++index) {
            VideoCommon::FileEnvironment env;
            env.Deserialize(stream);
            if (env.ShaderStage() == Shader::Stage::Task ||
                env.ShaderStage() == Shader::Stage::Mesh) {
                return true;
            }
        }
    } catch (const std::exception&) {
    }
    return false;
}

} // Anonymous namespace

LogImport ImportIntoLog(const std::filesystem::path& game_dir,
                        const std::filesystem::path& log_file,
                        const VideoCommon::ShaderCacheV2::LogFile& log) {
    using CacheV2::RecordKind;
    // Written in parts, so that a large cache isn't held twice
    constexpr size_t ChunkBytes = 64u << 20;
    LogImport result;
    std::unordered_set<u64> graphics_keys;
    std::unordered_set<u64> compute_keys;
    std::unordered_map<u64, std::span<const u8>> migrated;
    bool has_used = false;
    for (const CacheV2::LogRecord& record : log.live) {
        switch (record.kind) {
        case RecordKind::Graphics:
            graphics_keys.insert(record.key);
            break;
        case RecordKind::Compute:
            compute_keys.insert(record.key);
            break;
        case RecordKind::Migrated:
            migrated.insert_or_assign(record.key, record.payload);
            break;
        case RecordKind::Used:
            has_used = true;
            break;
        case RecordKind::Program:
        case RecordKind::Tombstone:
            break;
        }
    }
    std::vector<u8> records;
    const auto flush = [&] {
        if (!records.empty() &&
            CacheV2::AppendToLog(log_file, records) != CacheV2::WriteResult::Written) {
            result.failed = true;
        }
        records.clear();
    };
    for (const std::filesystem::path& source : LogSources(game_dir)) {
        std::error_code error;
        const auto size = std::filesystem::file_size(source, error);
        if (error) {
            continue;
        }
        const auto time = std::filesystem::last_write_time(source, error);
        if (error) {
            continue;
        }
        std::array<u8, 16> stamp{};
        const u64 stamp_size = static_cast<u64>(size);
        const s64 stamp_time = static_cast<s64>(time.time_since_epoch().count());
        std::memcpy(stamp.data(), &stamp_size, sizeof(stamp_size));
        std::memcpy(stamp.data() + sizeof(stamp_size), &stamp_time, sizeof(stamp_time));
        const std::string name = Common::FS::PathToUTF8String(source.filename());
        const u64 name_key =
            CacheV2::Hash64(std::span{reinterpret_cast<const u8*>(name.data()), name.size()});
        if (const auto it = migrated.find(name_key);
            it != migrated.end() && std::ranges::equal(it->second, stamp)) {
            continue;
        }
        const std::optional<u32> version = ReadVersion(source);
        if (!version || !IsLogRecordLayout(*version)) {
            continue;
        }
        const u16 layout = static_cast<u16>(*version);
        // A later record of a pipeline in the same file (EDEN_NX2_CACHE_REPAIR stored it again
        // after its earlier record failed) follows it into the log and takes its place there.
        std::unordered_set<u64> source_graphics_keys;
        // Reading stops at a damaged record; the ones before it are taken.
        (void)WalkPipelines(source, [&](const PipelineBytes& pipeline) {
            if (pipeline.compute) {
                if (!compute_keys.insert(pipeline.log_key).second) {
                    return;
                }
                CacheV2::EncodeRecord(records, RecordKind::Compute, pipeline.log_key,
                                      pipeline.bytes, layout);
                ++result.compute;
            } else {
                if (graphics_keys.contains(pipeline.log_key)) {
                    return;
                }
                if (source_graphics_keys.insert(pipeline.log_key).second) {
                    ++result.graphics;
                }
                CacheV2::EncodeRecord(records, RecordKind::Graphics, pipeline.log_key,
                                      pipeline.bytes, layout);
            }
            if (records.size() >= ChunkBytes) {
                flush();
            }
        });
        graphics_keys.insert(source_graphics_keys.begin(), source_graphics_keys.end());
        CacheV2::EncodeRecord(records, RecordKind::Migrated, name_key, stamp);
        result.sources.push_back(name);
        flush();
    }
    if (!has_used) {
        std::ifstream used(game_dir / "vulkan_used.bin", std::ios::binary | std::ios::ate);
        const std::streamoff size = used.is_open() ? std::streamoff{used.tellg()} : 0;
        if (size > 0) {
            std::vector<u8> bytes(static_cast<size_t>(size));
            used.seekg(0);
            if (used.read(reinterpret_cast<char*>(bytes.data()), size)) {
                CacheV2::EncodeRecord(records, RecordKind::Used, 0, bytes);
                result.used = true;
                flush();
            }
        }
    }
    return result;
}

Contents InspectLog(const std::filesystem::path& path) {
    Contents contents;
    const CacheV2::LogFile log = CacheV2::ReadLogFile(path);
    for (const CacheV2::LogRecord& record : log.live) {
        if (!IsLoadedRecord(record)) {
            continue;
        }
        if (record.kind == CacheV2::RecordKind::Compute) {
            ++contents.compute;
        } else if (IsMeshRecord(record.payload)) {
            ++contents.mesh;
        } else {
            ++contents.graphics;
        }
    }
    contents.damaged = log.stats.damaged != 0;
    return contents;
}

MergeResult MergeIntoLog(const std::filesystem::path& source,
                         const std::filesystem::path& game_dir) {
    MergeResult result;
    const std::optional<u32> version = ReadVersion(source);
    if (!version) {
        result.error = "not a pipeline cache";
        return result;
    }
    if (!IsImportable(*version)) {
        result.error = "cache version " + std::to_string(*version);
        return result;
    }
    const std::filesystem::path log_file = game_dir / CacheV2::FileName;
    std::unordered_set<u64> graphics_keys;
    std::unordered_set<u64> compute_keys;
    {
        const CacheV2::LogFile log = CacheV2::ReadLogFile(log_file);
        if (log.header == CacheV2::HeaderStatus::OtherVersion) {
            result.error = "the game's shader cache log is of another format version";
            return result;
        }
        for (const CacheV2::LogRecord& record : log.live) {
            if (IsLoadedRecord(record)) {
                (record.kind == CacheV2::RecordKind::Compute ? compute_keys : graphics_keys)
                    .insert(record.key);
            }
        }
    }
    // What the next boot takes in from the older files is cached too.
    for (const std::filesystem::path& file : LogSources(game_dir)) {
        const std::optional<u32> file_version = ReadVersion(file);
        if (!file_version || !IsLogRecordLayout(*file_version)) {
            continue;
        }
        (void)WalkPipelines(file, [&](const PipelineBytes& pipeline) {
            (pipeline.compute ? compute_keys : graphics_keys).insert(pipeline.log_key);
        });
    }
    std::vector<u8> records;
    result.source_damaged = !WalkPipelines(source, [&](const PipelineBytes& pipeline) {
        if (!(pipeline.compute ? compute_keys : graphics_keys).insert(pipeline.log_key).second) {
            ++result.duplicates;
            return;
        }
        CacheV2::EncodeRecord(records,
                              pipeline.compute ? CacheV2::RecordKind::Compute
                                               : CacheV2::RecordKind::Graphics,
                              pipeline.log_key, pipeline.bytes, static_cast<u16>(CurrentVersion));
        ++result.added;
    });
    if (result.source_damaged && *version != CurrentVersion) {
        // As in Merge: what came before the failure may be misread too.
        result.error = "the file doesn't read as a Lichii cache of version " +
                       std::to_string(*version);
        result.added = 0;
        return result;
    }
    if (records.empty()) {
        return result;
    }
    std::error_code error;
    std::filesystem::create_directories(game_dir, error);
    const CacheV2::WriteResult written = CacheV2::AppendToLog(log_file, records);
    if (written != CacheV2::WriteResult::Written) {
        result.error = written == CacheV2::WriteResult::Busy
                           ? "another emulator is writing the game's shader cache log"
                           : "can't write to the game's shader cache log";
        result.added = 0;
    }
    return result;
}

ExportResult ExportLog(const std::filesystem::path& game_dir,
                       const std::filesystem::path& target_dir) {
    ExportResult result;
    const std::array targets{target_dir / "vulkan.bin", target_dir / "vulkan_mesh.bin"};
    const std::filesystem::path used_target = target_dir / "vulkan_used.bin";
    std::error_code error;
    for (const std::filesystem::path& target : targets) {
        std::filesystem::remove(target, error);
    }
    std::filesystem::remove(used_target, error);

    std::array<std::ofstream, 2> outputs;
    bool failed = false;
    const auto write = [&](bool mesh, std::span<const u8> bytes) {
        std::ofstream& output = outputs[mesh ? 1 : 0];
        if (!output.is_open()) {
            output.open(targets[mesh ? 1 : 0], std::ios::binary | std::ios::trunc);
            const u32 current = CurrentVersion;
            output.write(MagicNumber.data(), MagicNumber.size())
                .write(reinterpret_cast<const char*>(&current), sizeof(current));
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        failed |= !output;
        ++result.pipelines;
    };
    std::unordered_set<u64> graphics_keys;
    std::unordered_set<u64> compute_keys;
    std::span<const u8> used;
    const CacheV2::LogFile log = CacheV2::ReadLogFile(game_dir / CacheV2::FileName);
    for (const CacheV2::LogRecord& record : log.live) {
        if (record.kind == CacheV2::RecordKind::Used) {
            used = record.payload;
        }
        if (!IsLoadedRecord(record)) {
            continue;
        }
        const bool compute = record.kind == CacheV2::RecordKind::Compute;
        (compute ? compute_keys : graphics_keys).insert(record.key);
        write(!compute && IsMeshRecord(record.payload), record.payload);
    }
    for (const std::filesystem::path& file : LogSources(game_dir)) {
        const std::optional<u32> version = ReadVersion(file);
        if (!version || !IsLogRecordLayout(*version)) {
            continue;
        }
        (void)WalkPipelines(file, [&](const PipelineBytes& pipeline) {
            if ((pipeline.compute ? compute_keys : graphics_keys).insert(pipeline.log_key).second) {
                write(pipeline.mesh, pipeline.bytes);
            }
        });
    }
    for (std::ofstream& output : outputs) {
        if (output.is_open()) {
            output.close();
            failed |= output.fail();
        }
    }
    if (!used.empty()) {
        std::ofstream output(used_target, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(used.data()),
                     static_cast<std::streamsize>(used.size()));
        output.close();
        failed |= output.fail();
        result.used = true;
    } else if (std::filesystem::is_regular_file(game_dir / "vulkan_used.bin", error)) {
        std::filesystem::copy_file(game_dir / "vulkan_used.bin", used_target,
                                   std::filesystem::copy_options::overwrite_existing, error);
        failed |= static_cast<bool>(error);
        result.used = !error;
    }
    if (failed) {
        result.error = "the files can't be written to " + Common::FS::PathToUTF8String(target_dir);
    }
    return result;
}

} // namespace Vulkan::PipelineCacheFile
