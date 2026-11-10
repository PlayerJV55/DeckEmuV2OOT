// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <utility>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction exit_instruction{{0x0000794d, 0, 0x03800000, 0}};
}

TEST_CASE("Ampere lowers Yoshi's full-mask buffer surface store", "[video_core][ampere]") {
    const Instruction store{{0x00007b9b, 0x20c00004, 0x00100f00, 0x000fe200}};
    const auto result = LowerToMaxwell(std::array{store, exit_instruction});
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() == 2);
    CHECK(((result.instructions.front() >> 33) & 7) == 1);
    CHECK(((result.instructions.front() >> 20) & 15) == 15);

    Instruction unsupported_dimension = store;
    unsupported_dimension.words[1] = (unsupported_dimension.words[1] & 0x1fffffffU) | 0x40000000U;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported_dimension, exit_instruction})));
}

TEST_CASE("Ampere VOTEU writes mapped uniform registers", "[video_core][ampere]") {
    for (const auto [target, mapped] : std::array<std::pair<u32, u32>, 5>{
             {{16, 232}, {18, 234}, {27, 245}, {28, 214}, {63, 255}}}) {
        const Instruction vote{{0x00007886U | (target << 16), 0, 0x038e0100, 0}};
        const auto result = LowerToMaxwell(std::array{vote, exit_instruction});
        REQUIRE(static_cast<bool>(result));
        REQUIRE(result.instructions.size() == 2);
        REQUIRE((result.instructions.front() & 0xff) == mapped);
    }
    const Instruction invalid{{0x00407886, 0, 0x038e0100, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{invalid, exit_instruction})));
    const Instruction extra_operand{{0x00107886, 1, 0x038e0100, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{extra_operand, exit_instruction})));
}

TEST_CASE("Ampere byte F2I clamps to the byte range before converting", "[video_core][ampere]") {
    for (const bool is_signed : {false, true}) {
        for (const bool flush : {false, true}) {
            const Instruction convert{{0x00067305, 9,
                                       0x0020e000U | (is_signed ? 0x100U : 0) |
                                           (flush ? 0x10000U : 0),
                                       0}};
            const auto result = LowerToMaxwell(std::array{convert, exit_instruction});
            REQUIRE(static_cast<bool>(result));
            const auto word = std::ranges::find_if(result.instructions, [](u64 value) {
                return (value & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL;
            });
            REQUIRE(word != result.instructions.end());
            REQUIRE(((*word >> 12) & 1) == static_cast<u64>(is_signed));
            REQUIRE(((*word >> 39) & 3) == 3);
            REQUIRE(((*word >> 44) & 1) == static_cast<u64>(flush));
            const u32 minimum = is_signed ? 0xc3000000U : 0;
            const u32 maximum = is_signed ? 0x42fe0000U : 0x437f0000U;
            for (const u32 bound : {minimum, maximum}) {
                REQUIRE(std::ranges::any_of(result.instructions, [bound](u64 value) {
                    return (value & 0xff) == 252 && ((value >> 20) & 0xffffffffULL) == bound;
                }));
            }
        }
    }
    const Instruction unsupported{{0x00067305, 9, 0x0021e101, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, exit_instruction})));
}

TEST_CASE("Ampere F2I supports ceil rounding", "[video_core][ampere]") {
    for (const bool is_signed : {false, true}) {
        const Instruction convert{{0x00027305, 2,
                                   0x0020b000U | (is_signed ? 0x100U : 0), 0}};
        const auto result = LowerToMaxwell(std::array{convert, exit_instruction});
        REQUIRE(static_cast<bool>(result));
        const auto word = std::ranges::find_if(result.instructions, [](u64 value) {
            return (value & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL;
        });
        REQUIRE(word != result.instructions.end());
        REQUIRE(((*word >> 8) & 3) == 2);
        REQUIRE(((*word >> 12) & 1) == static_cast<u64>(is_signed));
        REQUIRE(((*word >> 39) & 3) == 2);
    }
}

TEST_CASE("Ampere geometry program counters use the native address", "[video_core][ampere]") {
    const Instruction nop{{0x00007918, 0, 0, 0}};
    const Instruction lepc{{0x0014734e, 0, 0, 0}};
    const std::array code{nop, lepc, exit_instruction};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    const auto result = LowerToMaxwell(code, {.geometry = true,
                                            .native_code_address = 0x00000012fffffff0ULL});
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() == 4);
    REQUIRE((result.instructions[1] & 0xff) == 20);
    REQUIRE(((result.instructions[1] >> 20) & 0xffffffffULL) == 0);
    REQUIRE((result.instructions[2] & 0xff) == 21);
    REQUIRE(((result.instructions[2] >> 20) & 0xffffffffULL) == 0x13);
}

namespace {
template <typename Predicate>
size_t CountLowered(const Shader::Ampere::LoweredProgram& result, Predicate&& predicate) {
    return static_cast<size_t>(std::ranges::count_if(result.instructions, predicate));
}
constexpr bool IsOpcode(u64 value, u64 opcode, u64 mask = 0xfff8000000000000ULL) {
    return (value & mask) == opcode;
}
} // namespace

TEST_CASE("Ampere F2IP.U8 with RZ lowers as the byte F2I", "[video_core][ampere][oot]") {
    const Instruction convert{{0xff0a7243, 9, 0x0000c4ff, 0x000fe400}};
    const auto result = LowerToMaxwell(std::array{convert, exit_instruction});
    INFO(result.error);
    REQUIRE(static_cast<bool>(result));
    const auto word = std::ranges::find_if(result.instructions, [](u64 value) {
        return (value & 0xfff0000000000000ULL) == 0x5cb0000000000000ULL;
    });
    REQUIRE(word != result.instructions.end());
    CHECK((*word & 0xff) == 0x0a);
    CHECK(((*word >> 12) & 1) == 0);
    CHECK(((*word >> 20) & 0xff) == 250);
    CHECK(CountLowered(result, [](u64 value) {
              return (value & 0xff) == 252 && ((value >> 20) & 0xffffffffULL) == 0x437f0000U;
          }) == 1);
    Instruction second_source = convert;
    second_source.words[2] = 0x0000c405;
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{second_source, exit_instruction})));
}

TEST_CASE("Ampere F2I applies register negate and absolute", "[video_core][ampere][oot]") {
    for (const u32 modifiers : {0x40000000U, 0x80000000U, 0xc0000000U}) {
        const Instruction convert{{0x00067305, 9 | modifiers, 0x0021e000, 0}};
        const auto result = LowerToMaxwell(std::array{convert, exit_instruction});
        INFO(result.error);
        REQUIRE(static_cast<bool>(result));
        CHECK(CountLowered(result, [](u64 value) {
                  return IsOpcode(value, 0x5c98000000000000ULL) && (value & 0xff) == 249 &&
                         ((value >> 20) & 0xff) == 9;
              }) == 1);
        CHECK(CountLowered(result, [](u64 value) {
                  return (value >> 58) == 1 && (value & 0xff) == 249;
              }) == static_cast<size_t>(std::popcount(modifiers)));
    }
}

TEST_CASE("Ampere UIMAD with an immediate maps uniform registers", "[video_core][ampere][oot]") {
    const Instruction multiply{{0x040474a4, 0x00003c00, 0x0f8e0206, 0x000fc600}};
    const auto result = LowerToMaxwell(std::array{multiply, exit_instruction});
    INFO(result.error);
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() >= 2);
    CHECK((result.instructions[result.instructions.size() - 2] & 0xff) == 220);
}

TEST_CASE("Ampere signed REDUX.MIN flips the sign bits", "[video_core][ampere][oot]") {
    const auto flips = [](const Shader::Ampere::LoweredProgram& result) {
        return CountLowered(result, [](u64 value) {
            return (value >> 58) == 1 && ((value >> 53) & 3) == 2 &&
                   ((value >> 20) & 0xffffffffULL) == 0x80000000U;
        });
    };
    const Instruction signed_min{{0x020573c4, 0, 0x00010200, 0}};
    const auto result = LowerToMaxwell(std::array{signed_min, exit_instruction});
    INFO(result.error);
    REQUIRE(static_cast<bool>(result));
    CHECK(flips(result) == 33);
    const Instruction unsigned_min{{0x020573c4, 0, 0x00010000, 0}};
    const auto unsigned_result = LowerToMaxwell(std::array{unsigned_min, exit_instruction});
    REQUIRE(static_cast<bool>(unsigned_result));
    CHECK(flips(unsigned_result) == 0);
    const Instruction signed_sum{{0x020573c4, 0, 0x0000c200, 0}};
    CHECK_FALSE(static_cast<bool>(LowerToMaxwell(std::array{signed_sum, exit_instruction})));
}

TEST_CASE("Ampere ISETP.EX accumulates into both destinations", "[video_core][ampere][oot]") {
    // ISETP.LT.U32.EX P0, P1, R2, R4, P2, P3 (AND).
    const Instruction compare{{0x0200720c, 4, 0x01101130, 0}};
    const auto result = LowerToMaxwell(std::array{compare, exit_instruction});
    INFO(result.error);
    REQUIRE(static_cast<bool>(result));
    const auto saves = [](u64 opcode) {
        return [opcode](u64 value) {
            return IsOpcode(value, opcode) && ((value >> 20) & 0xff) == (1U << 4);
        };
    };
    CHECK(CountLowered(result, saves(0x38e8000000000000ULL)) == 1);
    CHECK(CountLowered(result, saves(0x38f0000000000000ULL)) == 1);
    const auto psetp = std::ranges::find_if(result.instructions, [](u64 value) {
        return IsOpcode(value, 0x5090000000000000ULL);
    });
    REQUIRE(psetp != result.instructions.end());
    CHECK((*psetp & 7) == 1);
    CHECK(((*psetp >> 3) & 7) == 0);
    CHECK(((*psetp >> 12) & 7) == 4);
    CHECK(((*psetp >> 39) & 7) == 2);

    const Instruction plain{{0x0200720c, 4, 0x03f01130, 0}};
    REQUIRE(static_cast<bool>(LowerToMaxwell(std::array{plain, exit_instruction})));
}

TEST_CASE("Ampere SHF.R.U64 shifts the whole 64-bit pair", "[video_core][ampere][oot]") {
    const Instruction shift{{0x02047219, 6, 0x00001203, 0}};
    const auto result = LowerToMaxwell(std::array{shift, exit_instruction});
    INFO(result.error);
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() >= 2);
    CHECK((result.instructions[result.instructions.size() - 2] & 0xff) == 4);
    CHECK(CountLowered(result, [](u64 value) {
              return IsOpcode(value, 0x1c00000000000000ULL, 0xfe00000000000000ULL) &&
                     ((value >> 20) & 0xffffffffULL) == 0xffffffe0U;
          }) == 1);
    CHECK(CountLowered(result, [](u64 value) {
              return IsOpcode(value, 0x5c28000000000000ULL) && ((value >> 8) & 0xff) == 3;
          }) == 1);
}

TEST_CASE("Ampere UFLO maps every uniform register", "[video_core][ampere][oot]") {
    const Instruction leading{{0x001472bd, 21, 0x080e0000, 0}};
    const auto result = LowerToMaxwell(std::array{leading, exit_instruction});
    INFO(result.error);
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() == 2);
    CHECK((result.instructions.front() & 0xff) == 236);
    CHECK(((result.instructions.front() >> 20) & 0xff) == 237);
}

TEST_CASE("Ampere UISETP under a UPn guard keeps the old output", "[video_core][ampere][oot]") {
    // @UP1 UISETP.LT.U32.AND UP0, UPT, UR4, UR5, UPT.
    const Instruction compare{{0x0400128c, 5, 0x03f01000, 0}};
    const auto result = LowerToMaxwell(std::array{compare, exit_instruction});
    INFO(result.error);
    REQUIRE(static_cast<bool>(result));
    const auto isetp = std::ranges::find_if(result.instructions, [](u64 value) {
        return IsOpcode(value, 0x5b60000000000000ULL);
    });
    REQUIRE(isetp != result.instructions.end());
    CHECK(((*isetp >> 16) & 15) == 5);
    // The guard (P5) and the old UP0 (P6) are loaded from R239 before the compare.
    CHECK(CountLowered(result, [](u64 value) {
              return IsOpcode(value, 0x38f0000000000000ULL) && ((value >> 8) & 0xff) == 239 &&
                     ((value >> 20) & 0xff) == 0x60;
          }) == 1);
}

TEST_CASE("Ampere SHF.R.U64 immediate keeps the shift out of the clamp bits",
          "[video_core][ampere][oot]") {
    const Instruction shift{{0x02047819, 21, 0x00001203, 0}};
    const auto result = LowerToMaxwell(std::array{shift, exit_instruction});
    INFO(result.error);
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() == 2);
    const u64 word = result.instructions.front();
    CHECK(IsOpcode(word, 0x38f8000000000000ULL));
    // The translator reads the immediate from bits 20-38: bits 37-38 must stay clear.
    CHECK(((word >> 20) & 0x7ffff) == 21);
    CHECK(((word >> 8) & 0xff) == 2);
    CHECK(((word >> 39) & 0xff) == 3);
}

TEST_CASE("Ampere HSETP2 compares a constant buffer half pair", "[video_core][ampere]") {
    // Mario Kart World: HSETP2 with B = c[7][0xfc], swizzle H1_H1.
    const Instruction compare{{0xff007634, 0x31c03f00, 0x04f24800, 0x000fca00}};
    const auto result = LowerToMaxwell(std::array{compare, exit_instruction});
    INFO(result.error);
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.instructions.size() == 3);
    const u64 load = result.instructions[0];
    CHECK((load >> 52) == 0x4c9);
    CHECK((load & 0xff) == 250);
    CHECK(((load >> 20) & 0x3fff) == 0xfc / 4);
    CHECK(((load >> 34) & 0x1f) == 7);
    const u64 hsetp2 = result.instructions[1];
    CHECK((hsetp2 >> 51) == (0x5d20000000000000ULL >> 51));
    CHECK(((hsetp2 >> 20) & 0xff) == 250);
    CHECK(((hsetp2 >> 28) & 3) == 3);
}
