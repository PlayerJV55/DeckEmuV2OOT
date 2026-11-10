// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

namespace {
// Reads are paced to a Switch 1 game card's speed (ReadBytesPerSecond, each read charged up to
// MaxChargedReadSize): Mario Strikers' intro froze when its movie data arrived faster. Switch 2
// storage is much faster, and Pokemon Pokopia's boot loading screen spent ~13 s in this wait
// (600+ reads a second, each charged 64 KiB), so Switch 2 titles are not paced.
// EDEN_NX2_READ_THROTTLE=<MB/s> sets the rate for every title instead (0: no pacing).
s64 ReadThrottle(Core::System& system) {
    if (const char* text = std::getenv("EDEN_NX2_READ_THROTTLE")) {
        return std::max<s64>(std::strtoll(text, nullptr, 10), 0) * 1'000'000;
    }
    const auto* process = system.ApplicationProcess();
    return process != nullptr && process->IsSwitch2() ? 0 : IStorage::ReadBytesPerSecond;
}

// Course and area data come in large files; a game also reads thousands of small ones.
constexpr u64 MinLoggedFileSize = 1ULL << 20;
constexpr u32 MaxLoggedFiles = 2000;
} // namespace

RomFSReadLog::RomFSReadLog(const FileSys::VirtualFile& romfs) {
    const auto add = [this](const auto& self, const FileSys::VirtualDir& dir,
                            const std::string& prefix) -> void {
        for (const auto& file : dir->GetFiles()) {
            // ExtractRomFS makes every file an OffsetVfsFile into romfs (romfs.cpp ProcessFile),
            // and the build has no RTTI for a dynamic cast.
            const auto offset_file = std::static_pointer_cast<FileSys::OffsetVfsFile>(file);
            if (file->GetSize() >= MinLoggedFileSize) {
                files.push_back(File{offset_file->GetOffset(), file->GetSize(),
                                     prefix + file->GetName(), false});
            }
        }
        for (const auto& subdirectory : dir->GetSubdirectories()) {
            self(self, subdirectory, prefix + subdirectory->GetName() + '/');
        }
    };
    if (const auto root = FileSys::ExtractRomFS(romfs)) {
        add(add, root, "");
    }
    std::ranges::sort(files, std::ranges::less{}, &File::offset);
    LOG_INFO(Service_FS, "RomFS read log: {} files of {} KiB or more", files.size(),
             MinLoggedFileSize / 1024);
}

void RomFSReadLog::Note(s64 offset, s64 length) {
    const u64 begin = static_cast<u64>(offset);
    const u64 end = begin + static_cast<u64>(length);
    std::scoped_lock lock{mutex};
    if (logged_files >= MaxLoggedFiles) {
        return;
    }
    // From the last file starting at or before the read: RomFS files don't overlap.
    auto it = std::ranges::upper_bound(files, begin, std::ranges::less{}, &File::offset);
    if (it != files.begin()) {
        --it;
    }
    for (; it != files.end() && it->offset < end; ++it) {
        if (it->logged || it->offset + it->size <= begin) {
            continue;
        }
        it->logged = true;
        LOG_INFO(Service_FS, "RomFS: first read of {} ({} KiB)", it->path, it->size / 1024);
        if (++logged_files == MaxLoggedFiles) {
            LOG_INFO(Service_FS, "RomFS: {} files named, no more are logged", MaxLoggedFiles);
            return;
        }
    }
}

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_,
                   std::shared_ptr<RomFSReadLog> read_log_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)),
      read_log(std::move(read_log_)), read_bytes_per_second{ReadThrottle(system_)} {
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    if (read_log) {
        read_log->Note(offset, length);
    }

    if (read_bytes_per_second > 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_read_tick) {
            std::this_thread::sleep_for(next_read_tick - now);
        }
        next_read_tick = std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds{std::min(length, MaxChargedReadSize) *
                                                  1'000'000'000 / read_bytes_per_second};
    }

    // Read the data from the Storage backend
    backend->Read(out_bytes.data(), length, offset);

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
