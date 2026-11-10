// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

TEST_CASE("Ampere Ocarina high effective address negates the register base",
          "[video_core][ampere]") {
    const Instruction capture{{0x01017211, 0x80000000, 0x078ff0ff, 0x000fca00}};
    for (const u32 guard : {1U, 7U, 9U}) {
        auto instruction = capture;
        instruction.words[0] = (instruction.words[0] & ~0xf000U) | (guard << 12);
        const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
        INFO(lowered.error);
        REQUIRE(lowered);
        REQUIRE(lowered.instructions.size() == 3);
        CHECK(lowered.instructions[0] == (0x38280000002001faULL | (u64{guard} << 16)));
        CHECK(lowered.instructions[1] == (0x5c1100000000fa01ULL | (u64{guard} << 16)));
    }
    for (const u32 bit : {62U, 74U, 81U}) {
        auto invalid = capture;
        invalid.words[bit / 32] ^= 1U << (bit % 32);
        CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    }
}

TEST_CASE("Ampere surface reductions preserve every U32 atomic operation",
          "[video_core][ampere]") {
    for (u32 op = 0; op < 8; ++op) {
        for (const u32 dimension : {1U, 3U, 4U, 5U}) {
            for (const u32 guard : {1U, 7U, 9U}) {
                const Instruction instruction{{0x0400059fU | (guard << 12),
                    0x00008400U | (dimension << 29), 0x0010e000U | (op << 23), 0}};
                const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                INFO(lowered.error);
                REQUIRE(lowered);
                REQUIRE(lowered.required_texture_buffer == 0);
                CHECK(lowered.instructions[0] == (0xeb48000000000400ULL | (u64{guard} << 16) |
                    (u64{op} << 24) | (u64{dimension} << 33) | (0x84ULL << 36)));
                auto invalid = instruction;
                invalid.words[2] |= 1U << 9;
                CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
            }
        }
    }
}

TEST_CASE("Ampere SHF.R.S64 and U64 keeping the low word lower to a wide SHF.R",
          "[video_core][ampere]") {
    // SHF.R.{S64,U64}[.W] R0, R2, R4, R3 (the forms in Ocarina of Time's fog and light passes).
    for (const u32 type : {0U, 1U}) {
        for (const u32 wrap : {0U, 1U}) {
            if (type == 1 && wrap == 0) {
                continue; // SHF.R.U64 without .W already has its own lowering
            }
            CAPTURE(type, wrap);
            const Instruction shift{{0x02007219, 4, 0x00001003U | (type << 9) | (wrap << 11),
                                     0x000fe200}};
            const auto lowered = LowerToMaxwell(std::array{shift, Exit});
            INFO(lowered.error);
            REQUIRE(lowered);
            const auto& out = lowered.instructions;
            const auto wide = std::find_if(out.begin(), out.end(), [](u64 word) {
                return (word >> 51) == (0x5cf8000000000000ULL >> 51);
            });
            REQUIRE(wide != out.end());
            CHECK(((*wide >> 37) & 3) == (type == 1 ? 2U : 3U));
            CHECK(((*wide >> 39) & 0xff) == 3);
            CHECK(((*wide >> 50) & 1) == wrap);
        }
    }
}

TEST_CASE("Ampere LEA.HI with a constant base and no carry lowers", "[video_core][ampere]") {
    // LEA.HI R1, R17, c[0][0x0], RZ, 0x1e from Pokemon Pokopia's compute shaders.
    const std::array<Instruction, 2> code{{{{0x11017a11, 0x00c00000, 0x078ff0ff, 0x000fe400}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const auto& out = lowered.instructions;
    // R250 = R17 >> 2 (32 - 0x1e, unsigned), then R1 = R250 + the constant.
    REQUIRE(std::find(out.begin(), out.end(), 0x38280000002711faULL) != out.end());
    REQUIRE(std::find(out.begin(), out.end(), 0x5c1000000fb7fa01ULL) != out.end());
}

TEST_CASE("Ampere LEA.HI with a constant base still rejects a carry out", "[video_core][ampere]") {
    // The same LEA.HI writing a carry out to P0 has no lowering yet.
    Instruction carry{{0x11017a11, 0x00c00000, 0x078ff0ff, 0x000fe400}};
    carry.words[2] &= ~(7U << 17);
    const std::array<Instruction, 2> code{{carry, Exit}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
}

TEST_CASE("Ampere LEA with a constant base and no carry out lowers to ISCADD", "[video_core][ampere]") {
    // LEA R0, R0, c[0][0x2e0]-style constant, 0x8 from Pokemon Pokopia's album compute shaders.
    const std::array<Instruction, 2> code{{{{0x00007a11, 0x01002e00, 0x078e40ff, 0x000fe200}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // ISCADD R0, R0, R251, 8 with the constant loaded into R251, and no carry compare.
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), 0x5c1804000fb70000ULL) != out.end());
    REQUIRE(std::none_of(out.begin(), out.end(), [](u64 i) { return (i >> 48) == 0x5b60; }));
}

TEST_CASE("Ampere SHF.L.U32.HI immediate without .W lowers to a clamping SHL", "[video_core][ampere]") {
    // SHF.L.U32.HI R16, RZ, R14, 0xff from Pokemon Pokopia's photo compute shaders.
    const std::array<Instruction, 2> code{{{{0xff107419, 0x000000ff, 0x0001060e, 0x080fe400}}, Exit}};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // SHL R16, R250, R14 without .W (bit 39), so a shift of 32 or more gives zero.
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), 0x5c48000000e7fa10ULL) != out.end());
}

TEST_CASE("Ampere guarded UIADD3 lowers to a guarded IADD32I or IADD", "[video_core][ampere]") {
    // @!UP0 UIADD3 UR6, UR14, 0x1c, URZ from Pokemon Pokopia's photo compute shaders.
    // @UP0 UIADD3 UR6, UR6, UR7, URZ from the same shaders (UR second source).
    struct Case { u32 guarded, plain, guard; };
    // @!UP1 UIADD3 UR6, UR9, UR8, UR6 (a third source: two guarded adds).
    struct Words { u32 b, w2, w3; };
    const auto words = [](u32 guarded) {
        return guarded == 0x0e068890 ? Words{0x1c, 0x0fffe03f, 0x000fcc00}
             : guarded == 0x06060290 ? Words{0x7, 0x0fffe03f, 0x000fc800}
                                     : Words{0x8, 0x0fffe006, 0x000fc800};
    };
    for (const Case c : {Case{0x0e068890, 0x0e067890, 0xe}, Case{0x06060290, 0x06067290, 0x6},
                         Case{0x09069290, 0x09067290, 0xd}}) {
        const Words w = words(c.guarded);
        const std::array<Instruction, 2> guarded{{{{c.guarded, w.b, w.w2, w.w3}}, Exit}};
        const std::array<Instruction, 2> plain{{{{c.plain, w.b, w.w2, w.w3}}, Exit}};
        const auto lowered = LowerToMaxwell(guarded);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const auto reference = LowerToMaxwell(plain);
        REQUIRE(static_cast<bool>(reference));
        // The same add as the unguarded one, predicated on P6 (negated for @!UP0) where UP0 is swapped in.
        // Every add of the unguarded lowering appears again, predicated on the swapped-in guard.
        const auto& out = lowered.instructions;
        for (const u64 add : reference.instructions) {
            if ((add >> 52) == 0xe30) continue; // EXIT
            const u64 guarded_add = (add & ~(u64{0xf} << 16)) | (u64{c.guard} << 16);
            REQUIRE(std::find(out.begin(), out.end(), guarded_add) != out.end());
        }
    }
}

TEST_CASE("Ampere uniform add carries into any uniform predicate", "[video_core][ampere]") {
    // UIADD3 UR6, UP2, UR15, UR6, URZ from Ocarina of Time's compute shaders (0x290), the same
    // add of an immediate (0x890), then UIADD3.X UR7, UR7, UR7 (or 0x0), URZ, UPn consuming the
    // carry. The register forms have their own carry lowering; the immediate ones took UP0/UP1 only.
    for (const u32 carry : {0U, 1U, 2U, 4U, 6U}) {
        const u64 mask = 1ULL << (6 - carry);
        // The carry is computed in P(6-n), merged into R239 bit 6-n, and P(6-n) is restored.
        const u64 merge = 0x38e8000000070000ULL | 239ULL | (239ULL << 8) | (mask << 20);
        const u64 restore = 0x38f0000000070000ULL | (253ULL << 8) | (mask << 20);
        const u64 load = 0x38f0000000070000ULL | (239ULL << 8) | (mask << 20);
        for (const u32 form : {0x7290U, 0x7890U}) {
            CAPTURE(carry, form);
            const bool immediate = form == 0x7890;
            const Instruction add{{0x0f060000U | form, immediate ? 0x1cU : 6U,
                                   (0x0ff5e03fU & ~(7U << 17)) | (carry << 17), 0x000fe200}};
            const auto lowered = LowerToMaxwell(std::array{add, Exit});
            INFO(lowered.error);
            REQUIRE(lowered);
            const auto& out = lowered.instructions;
            CHECK(std::count(out.begin(), out.end(), merge) == 1);
            CHECK(std::count(out.begin(), out.end(), restore) == 1);

            const Instruction extended{{0x07070000U | form, immediate ? 0U : 7U,
                                        0x087fe43fU | (carry << 23), 0x000fc800}};
            const auto consumed = LowerToMaxwell(std::array{add, extended, Exit});
            INFO(consumed.error);
            REQUIRE(consumed);
            CHECK(std::count(consumed.instructions.begin(), consumed.instructions.end(), load) == 1);
        }
    }
}

TEST_CASE("Ampere DEPBAR.LE preserves shared visibility for SB0 wait lists", "[video_core][ampere]") {
    // DEPBAR.LE SB5, 0x1 and DEPBAR.LE SB0, 0x0, {3,2,1} (Pokemon Pokopia's photo compute shaders).
    for (const u32 word : {0x0000d040U, 0x0000800eU, 0x00008000U, 0x00008fffU, 0x0000ffffU}) {
        const std::array<Instruction, 2> code{{{{0x0000791a, word, 0, 0x000fce00}}, Exit}};
        const auto lowered = LowerToMaxwell(code);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.size() == 1);
        REQUIRE(lowered.instructions.back() == 0xe30000000007000fULL);
        const Instruction copy{{0x02067fae, 0, 0x0f901c08, 0}};
        const auto copying = LowerToMaxwell(std::array{copy, code[0], Exit});
        INFO(copying.error);
        REQUIRE(copying);
        const bool shared_wait = (word & 0x7000U) == 0;
        REQUIRE(std::count(copying.instructions.begin(), copying.instructions.end(),
                           0xef98000000070000ULL) == (shared_wait ? 1 : 0));
        REQUIRE(copying.source_instruction_offsets[2] - copying.source_instruction_offsets[1] ==
                (shared_wait ? 1u : 0u));
        if (shared_wait) {
            REQUIRE(copying.instructions[copying.source_instruction_offsets[1]] ==
                    0xef98000000070000ULL);
        }
        REQUIRE(copying.instructions.back() == 0xe30000000007000fULL);
    }
}

TEST_CASE("Ampere guarded USHF.R.U32.HI lowers to a guarded SHR", "[video_core][ampere]") {
    // @UP0 USHF.R.U32.HI UR7, URZ, 0x2, UR6 from Pokemon Pokopia's photo compute shaders.
    const std::array<Instruction, 2> guarded{{{{0x3f070899, 0x2, 0x08011606, 0x000fcc00}}, Exit}};
    const std::array<Instruction, 2> plain{{{{0x3f077899, 0x2, 0x08011606, 0x000fcc00}}, Exit}};
    const auto lowered = LowerToMaxwell(guarded);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const auto reference = LowerToMaxwell(plain);
    REQUIRE(static_cast<bool>(reference));
    const u64 shift = reference.instructions.front();
    const u64 guarded_shift = (shift & ~(u64{0xf} << 16)) | (u64{6} << 16);
    const auto& out = lowered.instructions;
    REQUIRE(std::find(out.begin(), out.end(), guarded_shift) != out.end());
}
} // namespace
