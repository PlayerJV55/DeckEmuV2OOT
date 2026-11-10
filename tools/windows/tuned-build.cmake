# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Optional tuned Windows builds (see tools/windows/build-pgo.ps1 and docs/Build.md). CMake runs
# this file right after project(yuzu) only when the configure step passes
# -DCMAKE_PROJECT_yuzu_INCLUDE=<this file>; a normal configure never reads it. The flags are
# directory options, so every target added afterwards gets them, the externals included, and
# paths with spaces need no quoting.

set(NEMYU_TUNED_ARCH "v3" CACHE STRING "Instruction set: v3 (x86-64-v3: AVX2, FMA, BMI2) or generic")
set_property(CACHE NEMYU_TUNED_ARCH PROPERTY STRINGS v3 generic)
set(NEMYU_PGO "off" CACHE STRING "Profile-guided optimization (clang-cl only): off, generate or use")
set_property(CACHE NEMYU_PGO PROPERTY STRINGS off generate use)
set(NEMYU_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata read when NEMYU_PGO is use")
set(NEMYU_PGO_RUNTIME "" CACHE FILEPATH "clang_rt.profile library linked when NEMYU_PGO is generate")
set(NEMYU_LTO_JOBS "0" CACHE STRING "Link-time code generation threads (0: the linker's default)")

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(NEMYU_TUNED_CLANG_CL ON)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(NEMYU_TUNED_CLANG_CL OFF)
else()
    message(FATAL_ERROR "tuned-build.cmake supports MSVC and clang-cl, not ${CMAKE_CXX_COMPILER_ID}")
endif()

if (NEMYU_TUNED_ARCH STREQUAL "v3")
    if (NEMYU_TUNED_CLANG_CL)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/clang:-march=x86-64-v3>")
    else()
        # The closest MSVC has: AVX2 with FMA, BMI1/2, LZCNT and F16C.
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/arch:AVX2>")
    endif()
elseif (NOT NEMYU_TUNED_ARCH STREQUAL "generic")
    message(FATAL_ERROR "NEMYU_TUNED_ARCH must be v3 or generic, not ${NEMYU_TUNED_ARCH}")
endif()

if (NOT NEMYU_PGO STREQUAL "off" AND NOT NEMYU_TUNED_CLANG_CL)
    message(FATAL_ERROR "NEMYU_PGO needs clang-cl (CMAKE_C_COMPILER and CMAKE_CXX_COMPILER)")
endif()

if (NEMYU_PGO STREQUAL "generate")
    if (NOT EXISTS "${NEMYU_PGO_RUNTIME}")
        message(FATAL_ERROR "NEMYU_PGO_RUNTIME (${NEMYU_PGO_RUNTIME}) does not exist: point it at the "
            "clang_rt.profile library of the LLVM that provides the compiler")
    endif()
    file(TO_CMAKE_PATH "${NEMYU_PGO_RUNTIME}" runtime)
    get_filename_component(runtime_dir "${runtime}" DIRECTORY)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/clang:-fprofile-generate>")
    # Starts the periodic profile write (common/pgo_profile_writer.h).
    add_compile_definitions(NEMYU_PGO_GENERATE)
    # The objects ask for the runtime by name (/DEFAULTLIB); the path also covers the other name.
    add_link_options("/LIBPATH:${runtime_dir}" "${runtime}")
    message(STATUS "Tuned build: instrumented for profile training (${runtime})")
elseif (NEMYU_PGO STREQUAL "use")
    if (NOT EXISTS "${NEMYU_PGO_PROFILE}")
        message(FATAL_ERROR "NEMYU_PGO_PROFILE (${NEMYU_PGO_PROFILE}) does not exist: merge the "
            "training runs with tools/windows/build-pgo.ps1 -Mode merge first")
    endif()
    file(TO_CMAKE_PATH "${NEMYU_PGO_PROFILE}" profile)
    # Functions changed since the training build simply get no profile; that is not an error.
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX>:/clang:-fprofile-use=${profile}>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-profile-instr-unprofiled>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-profile-instr-out-of-date>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-profile-instr-missing>"
        "$<$<COMPILE_LANGUAGE:C,CXX>:-Wno-backend-plugin>")
    message(STATUS "Tuned build: optimized with the profile ${profile}")
elseif (NOT NEMYU_PGO STREQUAL "off")
    message(FATAL_ERROR "NEMYU_PGO must be off, generate or use, not ${NEMYU_PGO}")
endif()

if (ENABLE_LTO)
    if (NEMYU_TUNED_CLANG_CL)
        # ThinLTO (CMake's IPO for clang-cl); the cache makes relinks after small changes cheap.
        add_link_options("/lldltocache:${CMAKE_BINARY_DIR}/thinlto-cache")
        if (NOT NEMYU_LTO_JOBS STREQUAL "0")
            add_link_options("/opt:lldltojobs=${NEMYU_LTO_JOBS}")
        endif()
    elseif (NOT NEMYU_LTO_JOBS STREQUAL "0")
        # /GL + /LTCG (CMake's IPO for MSVC); /CGTHREADS takes 1 to 8.
        add_link_options("/CGTHREADS:${NEMYU_LTO_JOBS}")
    endif()
endif()

message(STATUS "Tuned build: ${CMAKE_CXX_COMPILER_ID}, arch ${NEMYU_TUNED_ARCH}, PGO ${NEMYU_PGO}, LTO ${ENABLE_LTO}")
