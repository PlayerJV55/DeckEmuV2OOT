# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Embeds the bundled Switch 2 title list as a byte array, as MSVC limits string literal lengths.

set(JSON_FILE ${CMAKE_ARGV3})
set(HEADER_FILE ${CMAKE_ARGV4})

file(READ "${JSON_FILE}" JSON_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," JSON_BYTES "${JSON_HEX}")
# One line of bytes per line of JSON
string(REPLACE "0x0a," "0x0a,\n" JSON_BYTES "${JSON_BYTES}")

get_filename_component(OUTPUT_DIR "${HEADER_FILE}" DIRECTORY)
make_directory("${OUTPUT_DIR}")

file(WRITE "${HEADER_FILE}"
"// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Generated from dist/switch2_titles.json, do not edit.

#pragma once

namespace FileSys {

constexpr unsigned char SWITCH2_TITLES_JSON[]{
${JSON_BYTES}};

} // namespace FileSys
")
