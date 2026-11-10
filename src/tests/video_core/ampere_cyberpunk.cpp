// SPDX-License-Identifier: GPL-3.0-or-later

// Cyberpunk 2077 (0400DDB020648000) instruction forms, from Satsuma's captures.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
constexpr u64 MembarGl = 0xef98000000000000ULL | (7ULL << 16) | (1ULL << 8);
} // namespace

TEST_CASE("Ampere warp reductions take AND and XOR", "[video_core][ampere][cyberpunk]") {
    // REDUX UR6, R3 with 0 .AND (Cyberpunk 2077), 0x4000 .OR and 0x8000 .XOR.
    for (const u32 mode : {0U, 0x4000U, 0x8000U}) {
        const Instruction reduction{{0x030673c4, 0, mode, 0x000ea200}};
        const auto lowered = LowerToMaxwell(std::array{reduction, Exit});
        INFO(lowered.error);
        REQUIRE(lowered);
        const auto& out = lowered.instructions;
        // Maxwell LOP numbers its operations as REDUX does: 0 AND, 1 OR, 2 XOR.
        const u64 merge = 0x5c40000000070000ULL | 222ULL | (222ULL << 8) | (251ULL << 20) |
                          (u64{mode >> 14} << 41) | (7ULL << 48);
        CHECK(std::count(out.begin(), out.end(), merge) == 32);
        // AND gives inactive lanes all ones (OR with the inverted lane mask), the others zero.
        const u64 inactive = 0x5c40000000070000ULL | 251ULL | (251ULL << 8) | (252ULL << 20) |
                             (mode == 0 ? (1ULL << 40) | (1ULL << 41) : 0) | (7ULL << 48);
        CHECK(std::count(out.begin(), out.end(), inactive) == 32);
    }
    // The signed bit only applies to .MIN/.MAX, and 0x18000 is no operation.
    for (const u32 mode : {0x200U, 0x8200U, 0x18000U}) {
        const Instruction reduction{{0x030673c4, 0, mode, 0x000ea200}};
        CHECK_FALSE(LowerToMaxwell(std::array{reduction, Exit}));
    }
}

TEST_CASE("Ampere SURED with a register handle lowers to a bindless SURED",
          "[video_core][ampere][cyberpunk]") {
    // @P1 SURED.1D_BUFFER.ADD.U32 [R10], R9, handle R4.
    const Instruction reduction{{0x0a0013a0, 0x20000009, 0x0010e004, 0x0041e200}};
    const auto lowered = LowerToMaxwell(std::array{reduction, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    CHECK(lowered.instructions.front() ==
          (0xeb40000000000000ULL | (1ULL << 16) | 9ULL | (10ULL << 8) | (1ULL << 33) |
           (4ULL << 39)));
    auto scratch_handle = reduction;
    scratch_handle.words[2] |= 0xf8; // R252 is a lowering scratch register
    CHECK_FALSE(LowerToMaxwell(std::array{scratch_handle, Exit}));
    auto exchange = reduction;
    exchange.words[2] |= 8U << 23; // no EXCH reduction
    CHECK_FALSE(LowerToMaxwell(std::array{exchange, Exit}));
}

TEST_CASE("Ampere strong GPU LDG through a uniform base is fenced",
          "[video_core][ampere][cyberpunk]") {
    // LDG.E.STRONG.GPU R7, [R7 + UR4].
    const Instruction load{{0x07077981, 0x00000004, 0x081ee900, 0x002f2800}};
    const auto lowered = LowerToMaxwell(std::array{load, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() >= 5);
    // After the two adds of the base and the offset.
    CHECK(lowered.instructions[2] == MembarGl);
    CHECK((lowered.instructions[3] >> 52) == 0xeedULL);
    CHECK(lowered.instructions[4] == MembarGl);
    // Ordering 6 (bit 78 alone) is not a defined form.
    auto unordered = load;
    unordered.words[2] &= ~0x2000U;
    CHECK_FALSE(LowerToMaxwell(std::array{unordered, Exit}));
    // The constant form stays unfenced.
    auto constant = load;
    constant.words[2] &= ~0x6000U;
    const auto plain = LowerToMaxwell(std::array{constant, Exit});
    REQUIRE(plain);
    CHECK(std::ranges::count(plain.instructions, MembarGl) == 0);
}
