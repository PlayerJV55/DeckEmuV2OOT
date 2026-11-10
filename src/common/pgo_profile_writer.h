// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef NEMYU_PGO_GENERATE
#include <chrono>
#include <thread>

extern "C" int __llvm_profile_write_file(void);
#endif

namespace Common {

/// Profile training builds (tools/windows/build-pgo.ps1) write their profile at exit, which a run
/// that is killed or hangs on shutdown never reaches, so they also write it every 30 s. Other
/// builds do nothing here.
inline void StartPgoProfileWriter() {
#ifdef NEMYU_PGO_GENERATE
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds{30});
            __llvm_profile_write_file();
        }
    }).detach();
#endif
}

} // namespace Common
