// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

// Runs the few Maxwell words these lowerings emit.
struct GuardMachine {
    std::array<u32, 256> registers{};
    std::array<bool, 8> predicates{true, false, true, true, false, true, false, true};

    void Run(std::span<const u64> code) {
        const auto read = [&](u32 reg) { return reg == 255 ? 0U : registers[reg]; };
        for (const u64 word : code) {
            const u32 guard = (word >> 16) & 15;
            if (predicates[guard & 7] == bool(guard & 8)) continue;
            const u32 dest = word & 255, a = (word >> 8) & 255, b = (word >> 20) & 255;
            const u32 immediate = static_cast<u32>(word >> 20);
            const u32 top = static_cast<u32>(word >> 48);
            u32 value = 0;
            if (top == 0x38e8) { // P2R
                u32 packed = 0;
                for (u32 i = 0; i < 7; ++i) packed |= u32{predicates[i]} << i;
                value = (read(a) & ~(immediate & 127)) | (packed & immediate & 127);
            } else if (top == 0x38f0) { // R2P
                for (u32 i = 0; i < 7; ++i)
                    if (immediate & (1U << i)) predicates[i] = (read(a) >> i) & 1;
                continue;
            } else if ((word >> 56) == 1) { // MOV32I
                value = immediate;
            } else if (top == 0x3828) { // SHR by an immediate
                value = read(a) >> (immediate & 31);
            } else if (top == 0x5ca0) { // SEL
                value = predicates[(word >> 39) & 7] != bool((word >> 42) & 1) ? read(a) : read(b);
            } else if ((top & 0xfffc) == 0x5c10) { // IADD
                value = (((word >> 49) & 1) ? 0U - read(a) : read(a)) +
                        (((word >> 48) & 1) ? 0U - read(b) : read(b));
            } else {
                REQUIRE(word == 0xe30000000007000fULL); // EXIT
                continue;
            }
            if (dest != 255) registers[dest] = value;
        }
    }
};

TEST_CASE("Ampere guarded ULEA.HI obeys positive and inverted guards", "[video_core][ampere]") {
    for (u32 guard = 0; guard < 15; ++guard) {
        for (u32 upreds : {0U, 0x55U, 0x7fU}) {
            // @UPn ULEA.HI UR0, UR1, 0xd, URZ, 0x3
            const Instruction lea{{0x891 | (guard << 12) | (1U << 24), 13U, 0x080f183fU, 0}};
            const auto lowered = LowerToMaxwell(std::array{lea, Exit});
            INFO(lowered.error);
            REQUIRE(static_cast<bool>(lowered));
            GuardMachine machine;
            machine.registers[216] = 0xdeadbeef;
            machine.registers[217] = 0xf0000000;
            machine.registers[239] = upreds;
            const auto predicates = machine.predicates;
            machine.Run(lowered.instructions);
            const bool executes =
                (guard & 7) == 7 || (bool((upreds >> (6 - (guard & 7))) & 1) != bool(guard & 8));
            REQUIRE(machine.registers[216] == (executes ? 13U + (0xf0000000U >> 29) : 0xdeadbeef));
            REQUIRE(machine.registers[239] == upreds);
            REQUIRE(machine.predicates == predicates);
        }
    }
}

TEST_CASE("Ampere guarded uniform forms that need R253 stay rejected", "[video_core][ampere]") {
    // @UP0 UIADD3 with a carry out and @UP0 ULEA.X keep needing the scratch the guard lives in.
    REQUIRE_FALSE(static_cast<bool>(
        LowerToMaxwell(std::array{Instruction{{0x01000890, 13, 0x0ff3e03f, 0}}, Exit})));
    REQUIRE_FALSE(static_cast<bool>(
        LowerToMaxwell(std::array{Instruction{{0x01000891, 13, 0x080e1c3f, 0}}, Exit})));
}

TEST_CASE("Ampere UIMAD.X adds its uniform carry predicate", "[video_core][ampere]") {
    for (u32 carry = 0; carry < 7; ++carry) {
        for (bool negate : {false, true}) {
            for (u32 upreds : {0U, 0x55U, 0x7fU}) {
                for (bool immediate : {false, true}) {
                    // UIMAD.X UR0, UR1, 0x1, UR3, [!]UPn or UIMAD.X UR0, URZ, URZ, UR3, [!]UPn
                    const Instruction multiply =
                        immediate ? Instruction{{0x78a4 | (1U << 24), 1U,
                                                 0x000e0603U | (carry << 23) | (u32{negate} << 26), 0}}
                                  : Instruction{{0x72a4 | (63U << 24), 63U,
                                                 0x000e0403U | (carry << 23) | (u32{negate} << 26), 0}};
                    const auto lowered = LowerToMaxwell(std::array{multiply, Exit});
                    INFO(lowered.error);
                    REQUIRE(static_cast<bool>(lowered));
                    GuardMachine machine;
                    machine.registers[217] = 100;
                    machine.registers[219] = 7;
                    machine.registers[239] = upreds;
                    const auto predicates = machine.predicates;
                    machine.Run(lowered.instructions);
                    const bool up = bool((upreds >> (6 - carry)) & 1) != negate;
                    REQUIRE(machine.registers[216] == (immediate ? 107U : 7U) + u32{up});
                    REQUIRE(machine.predicates == predicates);
                }
            }
        }
    }
}
} // namespace
