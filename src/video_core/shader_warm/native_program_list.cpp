// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nx2_title_defaults.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/loader/loader.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/frontend/ampere/program_scan.h"
#include "video_core/shader_warm/native_program_list.h"
#include "video_core/shader_warm/shader_archives.h"

namespace VideoCommon::ShaderWarm {
namespace {

namespace fs = std::filesystem;
using Shader::Ampere::Instruction;
using Shader::Ampere::NativeProgramSet;

constexpr std::array<char, 8> CacheMagic{'N', 'M', 'P', 'R', 'G', '0', '0', '1'};
/// Sanity limits for the cache file: a damaged one is scanned again instead of read.
constexpr u32 MaxPrograms = 1U << 22;
constexpr size_t HeaderSize = 0x50;
constexpr size_t MaxCodeSize = Shader::Ampere::ProgramScanDetail::MaximumBytes;

// Media and physics data never holds shader code: skipping it saves most of the scan.
constexpr std::array SkippedExtensions{
    std::string_view{".bntx"},  std::string_view{".bwav"}, std::string_view{".bfstm"},
    std::string_view{".webm"},  std::string_view{".mp4"},  std::string_view{".bphsh"},
    std::string_view{".bphsc"}, std::string_view{".extm"}, std::string_view{".mate"},
    std::string_view{".norm"},  std::string_view{".hght"},
};

bool IsSkipped(std::string_view name) {
    return std::ranges::any_of(SkippedExtensions, [name](std::string_view extension) {
        return name.find(extension) != std::string_view::npos;
    });
}

template <typename T>
bool Read(std::ifstream& file, T& value) {
    return static_cast<bool>(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename T>
void Write(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

NativeProgramSet ReadCache(const fs::path& path, u64 romfs_size) {
    std::ifstream file(path, std::ios::binary);
    std::array<char, 8> magic{};
    u64 size{};
    u32 count{};
    if (!file.read(magic.data(), magic.size()) || magic != CacheMagic || !Read(file, size) ||
        size != romfs_size || !Read(file, count) || count > MaxPrograms) {
        return {};
    }
    NativeProgramSet set;
    set.programs.resize(count);
    for (auto& program : set.programs) {
        u32 header_size{};
        u32 code_size{};
        if (!Read(file, header_size) || !Read(file, code_size) ||
            (header_size != 0 && header_size != HeaderSize) || code_size == 0 ||
            code_size > MaxCodeSize || code_size % sizeof(Instruction) != 0) {
            return {};
        }
        program.header.resize(header_size);
        program.code.resize(code_size);
        if (!file.read(reinterpret_cast<char*>(program.header.data()), header_size) ||
            !file.read(reinterpret_cast<char*>(program.code.data()), code_size)) {
            return {};
        }
    }
    u32 pair_count{};
    if (!Read(file, pair_count) || pair_count > MaxPrograms) {
        return {};
    }
    set.pairs.resize(pair_count);
    for (auto& [vertex, pixel] : set.pairs) {
        if (!Read(file, vertex) || !Read(file, pixel) || vertex >= count || pixel >= count) {
            return {};
        }
    }
    u32 compute_count{};
    if (!Read(file, compute_count) || compute_count > MaxPrograms) {
        return {};
    }
    set.compute.resize(compute_count);
    for (auto& dispatch : set.compute) {
        if (!Read(file, dispatch.program) || !Read(file, dispatch.workgroup) ||
            !Read(file, dispatch.shared_memory) || !Read(file, dispatch.local_memory) ||
            dispatch.program >= count) {
            return {};
        }
    }
    return set;
}

void WriteCache(const fs::path& path, u64 romfs_size, const NativeProgramSet& set) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    fs::path temporary{path};
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(CacheMagic.data(), CacheMagic.size());
        Write(file, romfs_size);
        Write(file, static_cast<u32>(set.programs.size()));
        for (const auto& program : set.programs) {
            Write(file, static_cast<u32>(program.header.size()));
            Write(file, static_cast<u32>(program.code.size()));
            file.write(reinterpret_cast<const char*>(program.header.data()),
                       static_cast<std::streamsize>(program.header.size()));
            file.write(reinterpret_cast<const char*>(program.code.data()),
                       static_cast<std::streamsize>(program.code.size()));
        }
        Write(file, static_cast<u32>(set.pairs.size()));
        for (const auto& [vertex, pixel] : set.pairs) {
            Write(file, vertex);
            Write(file, pixel);
        }
        Write(file, static_cast<u32>(set.compute.size()));
        for (const auto& dispatch : set.compute) {
            Write(file, dispatch.program);
            Write(file, dispatch.workgroup);
            Write(file, dispatch.shared_memory);
            Write(file, dispatch.local_memory);
        }
        if (!file) {
            LOG_ERROR(Render_Vulkan, "Cannot write the native program list {}",
                      Common::FS::PathToUTF8String(path));
            file.close();
            fs::remove(temporary, error);
            return;
        }
    }
    fs::rename(temporary, path, error);
}

struct GameFile {
    std::string path; ///< Inside the RomFS, '/' separated
    FileSys::VirtualFile file;
};

void ListFiles(const FileSys::VirtualDir& dir, const std::string& prefix,
               std::vector<GameFile>& out) {
    for (const auto& file : dir->GetFiles()) {
        out.push_back({prefix + file->GetName(), file});
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        ListFiles(sub, prefix + sub->GetName() + "/", out);
    }
}

struct ScanState {
    std::unordered_map<u64, u32> indices; ///< NativeProgramKey -> index in set.programs
    std::set<std::pair<u32, u32>> pairs;
    std::set<std::tuple<u32, std::array<u32, 3>, u32, u32>> compute;
    NativeProgramSet set;
    size_t files{};
};

void ScanFile(std::span<const u8> data, ScanState& state) {
    ++state.files;
    const auto scanned = Shader::Ampere::ScanNativePrograms(data);
    std::vector<u32> indices(scanned.size());
    for (size_t i = 0; i < scanned.size(); ++i) {
        auto image = Shader::Ampere::CopyNativeProgram(data, scanned[i]);
        std::vector<Instruction> code(image.code.size() / sizeof(Instruction));
        std::memcpy(code.data(), image.code.data(), code.size() * sizeof(Instruction));
        const auto [it, inserted] = state.indices.emplace(
            Shader::Ampere::NativeProgramKey(code), static_cast<u32>(state.set.programs.size()));
        if (inserted) {
            state.set.programs.push_back(std::move(image));
        }
        indices[i] = it->second;
    }
    for (const auto& [vertex, pixel] : Shader::Ampere::ScanProgramPairs(data, scanned)) {
        state.pairs.emplace(indices[vertex], indices[pixel]);
    }
    for (const auto& dispatch : Shader::Ampere::ScanComputeDispatches(data, scanned)) {
        state.compute.emplace(indices[dispatch.program], dispatch.workgroup,
                              dispatch.shared_memory, dispatch.local_memory);
    }
}

} // Anonymous namespace

bool PrecompileComputeEnabled() {
    return Common::Nx2::Setting("EDEN_NX2_PRECOMPILE_COMPUTE", "0") == "1";
}

bool PrecompileGraphicsEnabled() {
    return Common::Nx2::Setting("EDEN_NX2_PRECOMPILE_GRAPHICS", "0") == "1";
}

// As FileSys::RomFSFactory::OpenCurrentProcess opens it for the game.
FileSys::VirtualFile OpenApplicationRomFS(Core::System& system, u64 program_id) {
    Loader::AppLoader& loader = system.GetAppLoader();
    FileSys::VirtualFile romfs;
    if (loader.ReadRomFS(romfs) != Loader::ResultStatus::Success || !romfs) {
        LOG_WARNING(Render_Vulkan, "Native programs: the application has no RomFS");
        return nullptr;
    }
    const FileSys::PatchManager patches{program_id, system.GetFileSystemController(),
                                        system.GetContentProvider()};
    if (!loader.IsRomFSUpdatable()) {
        return patches.PatchRomFS(nullptr, romfs, FileSys::ContentRecordType::Program);
    }
    FileSys::VirtualFile packed_update;
    void(loader.ReadUpdateRaw(packed_update));
    const auto nca =
        system.GetContentProvider().GetEntry(program_id, FileSys::ContentRecordType::Program);
    return patches.PatchRomFS(nca.get(), romfs, FileSys::ContentRecordType::Program,
                              packed_update);
}

fs::path NativeProgramListPath(u64 title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
           fmt::format("{:016x}", title_id) / "native_programs.bin";
}

NativeProgramScan CollectNativePrograms(const FileSys::VirtualFile& romfs,
                                        const fs::path& cache_file, std::stop_token stop) {
    NativeProgramScan result;
    if (!romfs) {
        return result;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    const u64 romfs_size = romfs->GetSize();
    if (auto cached = ReadCache(cache_file, romfs_size); !cached.programs.empty()) {
        result.set = std::move(cached);
        result.from_cache = true;
        result.seconds = elapsed();
        LOG_INFO(Render_Vulkan,
                 "Native programs: {}, {} vertex and pixel pairs and {} compute dispatches read "
                 "from {}",
                 result.set.programs.size(), result.set.pairs.size(), result.set.compute.size(),
                 Common::FS::PathToUTF8String(cache_file));
        return result;
    }
    const FileSys::VirtualDir root = FileSys::ExtractRomFS(romfs);
    if (!root) {
        return result;
    }
    std::vector<GameFile> files;
    ListFiles(root, "", files);
    ZstdDictionaries dictionaries;
    for (const GameFile& file : files) {
        if (ClassifyGameFile(file.path) != GameFileKind::ZstdDictionaries) {
            continue;
        }
        try {
            dictionaries.Load(file.file->ReadAllBytes());
        } catch (const ArchiveError& e) {
            LOG_WARNING(Render_Vulkan, "Native programs: zstd dictionary pack: {}", e.what());
        }
    }
    ScanState state;
    size_t unreadable = 0;
    for (const GameFile& file : files) {
        if (stop.stop_requested()) {
            return {};
        }
        if (IsSkipped(file.path)) {
            continue;
        }
        std::vector<u8> data = file.file->ReadAllBytes();
        if (file.path.ends_with(".zs")) {
            try {
                data = DecompressZstd(data, &dictionaries);
            } catch (const ArchiveError& e) {
                ++unreadable;
                LOG_DEBUG(Render_Vulkan, "Native programs: {}: {}", file.path, e.what());
                continue;
            }
        }
        ScanFile(data, state);
    }
    state.set.pairs.assign(state.pairs.begin(), state.pairs.end());
    for (const auto& [program, workgroup, shared, local] : state.compute) {
        state.set.compute.push_back({program, workgroup, shared, local});
    }
    result.set = std::move(state.set);
    result.files = state.files;
    result.seconds = elapsed();
    LOG_INFO(Render_Vulkan,
             "Native programs: {}, {} vertex and pixel pairs and {} compute dispatches found in "
             "{} files ({} compressed ones unreadable) in {:.1f} s",
             result.set.programs.size(), result.set.pairs.size(), result.set.compute.size(),
             result.files, unreadable, result.seconds);
    if (!result.set.programs.empty()) {
        WriteCache(cache_file, romfs_size, result.set);
    }
    return result;
}

} // namespace VideoCommon::ShaderWarm
