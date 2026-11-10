// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/service.h"

namespace Service::FileSystem {

/// EDEN_NX2_ROMFS_READ_LOG: logs the path of each large file of the running program's RomFS the
/// first time the game reads it, so a log shows what was loaded when (a course, an area).
class RomFSReadLog {
public:
    explicit RomFSReadLog(const FileSys::VirtualFile& romfs);

    void Note(s64 offset, s64 length);

private:
    struct File {
        u64 offset;
        u64 size;
        std::string path;
        bool logged;
    };

    std::mutex mutex;
    std::vector<File> files; ///< Sorted by offset
    u32 logged_files{};
};

class IStorage final : public ServiceFramework<IStorage> {
public:
    explicit IStorage(Core::System& system_, FileSys::VirtualFile backend_,
                      std::shared_ptr<RomFSReadLog> read_log_ = nullptr);

    /// Switch 1 game card pacing (see ReadThrottle in fs_i_storage.cpp).
    static constexpr s64 ReadBytesPerSecond = 40'000'000;
    static constexpr s64 MaxChargedReadSize = 64 * 1024;

private:
    FileSys::VirtualFile backend;
    std::shared_ptr<RomFSReadLog> read_log;

    s64 read_bytes_per_second; ///< 0: reads are not paced
    std::chrono::steady_clock::time_point next_read_tick{};

    Result Read(
        OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
        s64 offset, s64 length);
    Result GetSize(Out<u64> out_size);
};

} // namespace Service::FileSystem
