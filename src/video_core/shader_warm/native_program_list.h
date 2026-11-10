// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "shader_recompiler/frontend/ampere/native_program_image.h"

namespace Core {
class System;
}

namespace VideoCommon::ShaderWarm {

/// EDEN_NX2_PRECOMPILE_COMPUTE (off by default): Prepare Shaders also builds every compute program
/// of the game's files, with the workgroup and memory sizes of its shader control sections, and
/// drops it, so the driver's cache holds it when the game first dispatches it.
[[nodiscard]] bool PrecompileComputeEnabled();

/// EDEN_NX2_PRECOMPILE_GRAPHICS (off by default): Prepare Shaders also builds the vertex and pixel
/// program pairs of the game's files for the render setups the title's pipeline cache recorded,
/// and drops them.
[[nodiscard]] bool PrecompileGraphicsEnabled();

/// The running application's RomFS as the game sees it (its update and LayeredFS mods applied),
/// null when it has none.
[[nodiscard]] FileSys::VirtualFile OpenApplicationRomFS(Core::System& system, u64 program_id);

/// The scan result of a title's RomFS, beside its pipeline cache: native_programs.bin.
std::filesystem::path NativeProgramListPath(u64 title_id);

/// What CollectNativePrograms did, for the Prepare Shaders summary.
struct NativeProgramScan {
    Shader::Ampere::NativeProgramSet set;
    size_t files{};       ///< Files read, 0 when the list came from the cache file
    double seconds{};     ///< Time spent scanning or reading the cache file
    bool from_cache{};
};

/// The distinct native programs stored in an application's RomFS (Switch 2 shader containers,
/// decompressed when they end in .zs, with the game's zstd dictionaries), the vertex and pixel
/// programs its shader program tables combine, and how its compute programs are dispatched
/// (Shader::Ampere::ScanNativePrograms). The result is kept in cache_file and read from it while
/// the RomFS keeps its size. Empty when stopped.
[[nodiscard]] NativeProgramScan CollectNativePrograms(const FileSys::VirtualFile& romfs,
                                                      const std::filesystem::path& cache_file,
                                                      std::stop_token stop);

} // namespace VideoCommon::ShaderWarm
