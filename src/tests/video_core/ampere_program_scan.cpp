// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/program_scan.h"

namespace {
using Shader::Ampere::Instruction;

// MOV R1, 0x1 and the self-branch every program ends with (@PT BRA -16).
constexpr Instruction Filler{{0x00007202, 0x1, 0x00000f00, 0x000fe200}};
constexpr Instruction SelfBranch{{0x00007947, 0xfffffff0, 0x0383ffff, 0x000fc000}};

void Put(std::vector<u8>& data, size_t offset, const Instruction& instruction) {
    std::memcpy(data.data() + offset, &instruction, sizeof(instruction));
}

template <typename T>
void PutValue(std::vector<u8>& data, size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

// Three instructions and the self-branch at code_offset.
void PutProgram(std::vector<u8>& data, size_t code_offset) {
    for (size_t index = 0; index < 3; ++index) {
        Put(data, code_offset + index * sizeof(Instruction), Filler);
    }
    Put(data, code_offset + 3 * sizeof(Instruction), SelfBranch);
}

// A graphics program: header type in the block's first word, zero tail, code at +0x80.
void PutGraphicsProgram(std::vector<u8>& data, size_t block, u32 type) {
    PutValue<u32>(data, block, 0x00060000 | type);
    PutValue<u32>(data, block + 4, 0x12345);
    PutProgram(data, block + 0x80);
}
} // Anonymous namespace

TEST_CASE("Program scan finds compute and graphics programs", "[video_core][ampere]") {
    std::vector<u8> data(0x600);
    PutProgram(data, 0x100);
    PutGraphicsProgram(data, 0x200, 1);
    PutGraphicsProgram(data, 0x400, 2);

    const auto programs = Shader::Ampere::ScanNativePrograms(data);
    REQUIRE(programs.size() == 3);
    CHECK(!programs[0].graphics);
    CHECK(programs[0].block_offset == 0x100);
    CHECK(programs[0].code_offset == 0x100);
    CHECK(programs[0].code_size == 4 * sizeof(Instruction));
    CHECK(programs[1].graphics);
    CHECK(programs[1].block_offset == 0x200);
    CHECK(programs[1].code_offset == 0x280);
    CHECK(programs[1].code_size == 4 * sizeof(Instruction));
    CHECK(programs[2].graphics);
    CHECK(programs[2].block_offset == 0x400);

    const auto vertex = Shader::Ampere::CopyNativeProgram(data, programs[1]);
    CHECK(vertex.header.size() == 0x50);
    CHECK(vertex.code.size() == 4 * sizeof(Instruction));
    CHECK(!vertex.IsPixel());
    CHECK(Shader::Ampere::CopyNativeProgram(data, programs[2]).IsPixel());
    CHECK(Shader::Ampere::CopyNativeProgram(data, programs[0]).header.empty());
}

TEST_CASE("Program scan rejects a code run without a closing self-branch",
          "[video_core][ampere]") {
    std::vector<u8> data(0x300);
    for (size_t index = 0; index < 8; ++index) {
        Put(data, 0x100 + index * sizeof(Instruction), Filler);
    }
    CHECK(Shader::Ampere::ScanNativePrograms(data).empty());

    // A self-branch with no capturable code before it is no program either.
    std::vector<u8> lone(0x300);
    Put(lone, 0x200, SelfBranch);
    CHECK(Shader::Ampere::ScanNativePrograms(lone).empty());
}

TEST_CASE("Program scan pairs vertex and pixel programs of a BNSH table", "[video_core][ampere]") {
    std::vector<u8> data(0x800);
    PutGraphicsProgram(data, 0x200, 1);
    PutGraphicsProgram(data, 0x400, 2);
    // Container header at 0x40, its stage entries at 0x600: offsets from the header.
    std::memcpy(data.data() + 0x40, "BNSH", 4);
    PutValue<u64>(data, 0x600, 0x200 - 0x40);
    PutValue<u64>(data, 0x608, (u64{0xd8} << 32) | 0x40);
    PutValue<u64>(data, 0x640, 0x400 - 0x40);

    const auto programs = Shader::Ampere::ScanNativePrograms(data);
    REQUIRE(programs.size() == 2);
    const auto pairs = Shader::Ampere::ScanProgramPairs(data, programs);
    REQUIRE(pairs.size() == 1);
    CHECK(pairs[0].first == 0);
    CHECK(pairs[0].second == 1);

    // Entries past the next container belong to it, whose header the offsets don't count from.
    std::memcpy(data.data() + 0x5f0, "BNSH", 4);
    CHECK(Shader::Ampere::ScanProgramPairs(data, programs).empty());
}

TEST_CASE("Program scan reads compute dispatches from control sections", "[video_core][ampere]") {
    std::vector<u8> data(0x800);
    PutProgram(data, 0x100);
    std::memcpy(data.data() + 0x20, "BNSH", 4);
    // Stage entry at 0x300, its control section at 0x340.
    PutValue<u64>(data, 0x300, 0x100 - 0x20);
    PutValue<u32>(data, 0x308, 0xd8);
    PutValue<u32>(data, 0x340, 0x98761234);
    PutValue<u32>(data, 0x340 + 0x58, 64);
    PutValue<u32>(data, 0x340 + 0x5c, 2);
    PutValue<u32>(data, 0x340 + 0x60, 1);
    PutValue<u32>(data, 0x340 + 0x64, 0x123);
    PutValue<u32>(data, 0x340 + 0x68, 0x40);

    const auto programs = Shader::Ampere::ScanNativePrograms(data);
    REQUIRE(programs.size() == 1);
    const auto dispatches = Shader::Ampere::ScanComputeDispatches(data, programs);
    REQUIRE(dispatches.size() == 1);
    constexpr std::array<u32, 3> workgroup{64, 2, 1};
    CHECK(dispatches[0].program == 0);
    CHECK(dispatches[0].workgroup == workgroup);
    CHECK(dispatches[0].shared_memory == 0x200);
    CHECK(dispatches[0].local_memory == 0x40);

    // More than 1024 invocations is no workgroup a dispatch can have.
    PutValue<u32>(data, 0x340 + 0x5c, 32);
    CHECK(Shader::Ampere::ScanComputeDispatches(data, programs).empty());
}
