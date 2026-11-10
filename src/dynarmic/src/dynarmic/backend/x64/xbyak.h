// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// You must ensure this matches with src/common/x64/xbyak.h on root dir
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <boost/unordered_map.hpp>
#define XBYAK_STD_UNORDERED_SET ::Common::unordered_set
#define XBYAK_STD_UNORDERED_MAP ::Common::unordered_map
#define XBYAK_STD_UNORDERED_MULTIMAP boost::unordered_multimap
// dynarmic does not pass xbyak's strict checks (see its CMakeLists.txt). The checks live in
// xbyak's inline functions, of which the linker keeps one copy, so every user must agree.
#ifndef XBYAK_STRICT_CHECK_MEM_REG_SIZE
#define XBYAK_STRICT_CHECK_MEM_REG_SIZE 0
#endif
#ifndef XBYAK_OLD_DISP_CHECK
#define XBYAK_OLD_DISP_CHECK 1
#endif
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
