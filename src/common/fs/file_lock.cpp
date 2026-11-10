// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/fs/file_lock.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Common::FS {

#ifdef _WIN32

FileLock::FileLock(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    handle = reinterpret_cast<std::intptr_t>(file);
    OVERLAPPED overlapped{};
    held = LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                      &overlapped) != FALSE;
    busy = !held && GetLastError() == ERROR_LOCK_VIOLATION;
}

FileLock::~FileLock() {
    if (handle == -1) {
        return;
    }
    const HANDLE file = reinterpret_cast<HANDLE>(handle);
    if (held) {
        OVERLAPPED overlapped{};
        UnlockFileEx(file, 0, 1, 0, &overlapped);
    }
    CloseHandle(file);
}

#else

FileLock::FileLock(const std::filesystem::path& path) {
    const int file = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (file == -1) {
        return;
    }
    handle = file;
    int result;
    do {
        result = ::flock(file, LOCK_EX | LOCK_NB);
    } while (result != 0 && errno == EINTR);
    held = result == 0;
    busy = !held && errno == EWOULDBLOCK;
}

FileLock::~FileLock() {
    if (handle != -1) {
        ::close(static_cast<int>(handle)); // Releases the lock
    }
}

#endif

} // namespace Common::FS
