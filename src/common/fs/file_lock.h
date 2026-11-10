// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>

namespace Common::FS {

/// An exclusive advisory lock on a file (flock on POSIX, LockFileEx on Windows), taken at once or
/// not at all, and held until the object is destroyed. The system releases it when the process
/// ends, so a crash never leaves it held. It only keeps out others that take it too; the file it
/// locks is created if missing and holds no data.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path);
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    /// False when another process holds it, or the file can't be created or locked.
    [[nodiscard]] bool Held() const noexcept {
        return held;
    }

    /// True when it isn't held because another process holds it.
    [[nodiscard]] bool Busy() const noexcept {
        return busy;
    }

private:
    std::intptr_t handle{-1}; ///< File descriptor, or a HANDLE on Windows
    bool held{};
    bool busy{};
};

} // namespace Common::FS
