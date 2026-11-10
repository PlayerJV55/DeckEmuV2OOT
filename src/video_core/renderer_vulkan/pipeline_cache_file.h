// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::ShaderCacheV2 {
struct LogFile;
}

namespace Vulkan {
struct GraphicsPipelineCacheKey;
}

// Reading and merging Vulkan pipeline cache files (vulkan.bin, vulkan_mesh.bin) outside of a
// running game, for the shader manager.
namespace Vulkan::PipelineCacheFile {

/// Version of the records this build writes and reads. A new version sets every game's cache
/// aside (vulkan.v<old>.bin) and starts it empty, so bump it only when what a record holds
/// changes: the environments' layout (GenericEnvironment::Serialize), the pipeline keys, the
/// meaning of a recorded value, or what the native lowering makes of a program besides its code
/// (LoweredNativeCode::texture_bound). A change to the translation or to the lowered code needs
/// none: records hold guest inputs that are translated again at every boot, a program whose
/// lowered code changes gets a new hash (its old records no longer match a draw), and a record
/// lacking a value the new translation reads is stored again from the game's build
/// (EDEN_NX2_CACHE_REPAIR).
inline constexpr u32 CurrentVersion = 33;

/// Older versions whose records are laid out as CurrentVersion's, bumped for a translation change
/// only (32: depth-compare images and storage views). Their caches, set aside by the bump, can be
/// imported.
inline constexpr std::array<u32, 1> SameLayoutVersions{32};

/// Lichii's cache versions, whose records are laid out like CurrentVersion's (the same
/// environments, keys and framing; Lichii bumps the version when its translation changes). Their
/// pipelines can be imported, and are translated again by this build as any cached pipeline is.
inline constexpr std::array<u32, 3> LichiiVersions{52, 53, 54};

/// Whether this build can read and import a cache of `version`.
[[nodiscard]] constexpr bool IsImportable(u32 version) {
    return version == CurrentVersion ||
           std::ranges::find(SameLayoutVersions, version) != SameLayoutVersions.end() ||
           std::ranges::find(LichiiVersions, version) != LichiiVersions.end();
}

/// Whether shader cache v2 reads a pipeline record of this layout: the cache version of the
/// vulkan.bin record it holds, CurrentVersion or one of SameLayoutVersions. A version bump for a
/// translation change alone doesn't touch the log; add the old version to SameLayoutVersions.
[[nodiscard]] constexpr bool IsLogRecordLayout(u32 layout) {
    return layout == CurrentVersion ||
           std::ranges::find(SameLayoutVersions, layout) != SameLayoutVersions.end();
}

/// The version in a pipeline cache file's header, or nullopt if the file isn't one.
[[nodiscard]] std::optional<u32> ReadVersion(const std::filesystem::path& path);

struct Contents {
    u32 graphics{};
    u32 compute{};
    u32 mesh{};
    /// Reading stopped at a record it couldn't read; the counts are those before it.
    bool damaged{};
};

/// Counts the pipelines in a cache of an importable version. Only reads the file.
[[nodiscard]] Contents Inspect(const std::filesystem::path& path);

struct MergeResult {
    u32 added{};
    u32 duplicates{};
    /// Records past a damaged one, left out.
    bool source_damaged{};
    /// Set when nothing was merged.
    std::string error;
};

/// Appends the pipelines of `source`, a cache of an importable version, that the game's cache in
/// `game_dir` doesn't hold yet: task/mesh ones to vulkan_mesh.bin, the others to vulkan.bin.
/// The game must not be running.
[[nodiscard]] MergeResult Merge(const std::filesystem::path& source,
                                const std::filesystem::path& game_dir);

/// The shader cache v2 record key of a graphics pipeline: its key's hash without the driver, so
/// that a driver update keeps the record.
[[nodiscard]] u64 GraphicsLogKey(const GraphicsPipelineCacheKey& key);

struct LogImport {
    u32 graphics{};
    u32 compute{};
    bool used{};
    bool failed{};                    ///< Records couldn't be written to the log
    std::vector<std::string> sources; ///< Files read
};

/// Adds to shader cache v2's log `log_file`, read into `log`, the pipelines of the game's
/// vulkan.bin and vulkan_mesh.bin (and their set-aside copies of SameLayoutVersions) that it
/// doesn't hold, and vulkan_used.bin while it holds no used list. A file is read again only once
/// it changed (a Shader Manager import, a session with the log off). The files are only read.
[[nodiscard]] LogImport ImportIntoLog(const std::filesystem::path& game_dir,
                                      const std::filesystem::path& log_file,
                                      const VideoCommon::ShaderCacheV2::LogFile& log);

/// Counts the pipelines of shader cache v2's log that the boot load reads. `damaged` tells that
/// damaged records were passed over. Only reads the file.
[[nodiscard]] Contents InspectLog(const std::filesystem::path& path);

/// Merge into the game's shader cache v2 log: appends the pipelines of `source` that neither the
/// log nor the older files it takes in hold. The game must not be running.
[[nodiscard]] MergeResult MergeIntoLog(const std::filesystem::path& source,
                                       const std::filesystem::path& game_dir);

struct ExportResult {
    u32 pipelines{};
    bool used{};       ///< A used list was written
    std::string error; ///< Set when the files couldn't be written
};

/// Writes the pipelines that shader cache v2 loads for the game (its log, then what it takes in
/// from the older files) to vulkan.bin and vulkan_mesh.bin in `target_dir`, which every build
/// imports, and its used list to vulkan_used.bin. Only reads the game's files.
[[nodiscard]] ExportResult ExportLog(const std::filesystem::path& game_dir,
                                     const std::filesystem::path& target_dir);

} // namespace Vulkan::PipelineCacheFile
