// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/ir_opt/oot_visual_workaround.h"

namespace {
namespace IR = Shader::IR;
constexpr u64 Title = 0x0400486024DEC000ULL;
constexpr u64 Fog = 0x780c0536b89fc89eULL;
constexpr u64 Final = 0xbded5ce917183214ULL;

struct Fixture {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    IR::IREmitter ir{block};

    Fixture() {
        program.blocks.push_back(&block);
        program.stage = Shader::Stage::Compute;
    }
};

float Evaluate(IR::Value value) {
    if (value.IsImmediate()) {
        return value.F32();
    }
    const auto& inst = *value.InstRecursive();
    switch (inst.GetOpcode()) {
    case IR::Opcode::GetCbufF32:
        return 2.0f;
    case IR::Opcode::FPExp2:
        return std::exp2(Evaluate(inst.Arg(0)));
    case IR::Opcode::FPNeg32:
        return -Evaluate(inst.Arg(0));
    case IR::Opcode::FPAdd32:
        return Evaluate(inst.Arg(0)) + Evaluate(inst.Arg(1));
    case IR::Opcode::FPMul32:
        return Evaluate(inst.Arg(0)) * Evaluate(inst.Arg(1));
    default:
        FAIL("Unexpected opcode in visual workaround fixture");
        return 0;
    }
}
} // namespace

TEST_CASE("OoT visual workaround scales slice distance before extinction and scattering",
          "[video_core][oot_visual_workaround]") {
    Fixture f;
    const auto distance = f.ir.FPAdd(f.ir.FPNeg(f.ir.FPExp2(f.ir.Imm32(1.0f))),
                                    f.ir.FPExp2(f.ir.Imm32(3.0f)));
    const auto extinction = f.ir.FPAdd(f.ir.Imm32(1.0f), f.ir.FPMul(distance, f.ir.Imm32(2.0f)));
    const auto scattering = f.ir.FPMul(distance, f.ir.Imm32(4.0f));
    const auto transmittance = f.ir.FPExp2(IR::F32{f.ir.FPNeg(extinction)});
    REQUIRE(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title, Fog, true) == 1U);
    CHECK(Evaluate(distance) == 1.5f);
    CHECK(Evaluate(extinction) == 4.0f);
    CHECK(Evaluate(scattering) == 6.0f);
    CHECK(std::abs(Evaluate(transmittance) - 0.0625f) < 1e-6f);
}

TEST_CASE("OoT exposure workaround scales only the exposure load",
          "[video_core][oot_visual_workaround]") {
    Fixture f;
    f.program.stage = Shader::Stage::Fragment;
    const auto exposure = f.ir.GetFloatCbuf(f.ir.Imm32(3U), f.ir.Imm32(0x38U));
    const auto other_bank = f.ir.GetFloatCbuf(f.ir.Imm32(4U), f.ir.Imm32(0x38U));
    const auto other_offset = f.ir.GetFloatCbuf(f.ir.Imm32(3U), f.ir.Imm32(0x34U));
    const auto color = f.ir.FPMul(f.ir.Imm32(10.0f), exposure);
    const auto alpha = f.ir.Imm32(0.75f);
    REQUIRE(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title, Final, true) == 1U);
    CHECK(std::abs(Evaluate(color) - 13.0f) < 1e-5f);
    CHECK(Evaluate(other_bank) == 2.0f);
    CHECK(Evaluate(other_offset) == 2.0f);
    CHECK(Evaluate(alpha) == 0.75f);
}

TEST_CASE("OoT visual workaround takes Selevet's scales",
          "[video_core][oot_visual_workaround]") {
    Fixture f;
    f.program.stage = Shader::Stage::Fragment;
    const auto exposure = f.ir.FPMul(f.ir.Imm32(1.0f), f.ir.GetFloatCbuf(f.ir.Imm32(3U),
                                                                         f.ir.Imm32(0x38U)));
    const auto bloom = f.ir.FPMul(f.ir.Imm32(1.0f), f.ir.GetFloatCbuf(f.ir.Imm32(3U),
                                                                      f.ir.Imm32(0x34U)));
    const Shader::Optimization::OotVisualScales scales{
        .fog = 0.12f, .exposure = 0.55f, .bloom_exposure = 1.6f};
    REQUIRE(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title, Final, true,
                                                           scales) == 2U);
    CHECK(std::abs(Evaluate(exposure) - 1.1f) < 1e-5f);
    CHECK(std::abs(Evaluate(bloom) - 3.2f) < 1e-5f);

    Fixture fog;
    const auto distance = fog.ir.FPAdd(fog.ir.FPNeg(fog.ir.FPExp2(fog.ir.Imm32(1.0f))),
                                       fog.ir.FPExp2(fog.ir.Imm32(3.0f)));
    REQUIRE(Shader::Optimization::ApplyOotVisualWorkaround(fog.program, Title, Fog, true,
                                                           scales) == 1U);
    CHECK(std::abs(Evaluate(distance) - 0.72f) < 1e-5f);
}

TEST_CASE("OoT bloom scale alone does not match the final pass",
          "[video_core][oot_visual_workaround]") {
    Fixture f;
    f.program.stage = Shader::Stage::Fragment;
    const auto bloom = f.ir.GetFloatCbuf(f.ir.Imm32(3U), f.ir.Imm32(0x34U));
    const Shader::Optimization::OotVisualScales scales{.bloom_exposure = 1.6f};
    CHECK(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title, Final, true, scales) ==
          0U);
    CHECK(Evaluate(bloom) == 2.0f);
}

TEST_CASE("OoT fog workaround matches the native sign-bit negate",
          "[video_core][oot_visual_workaround]") {
    Fixture f;
    const auto value = f.ir.FPExp2(f.ir.Imm32(1.0f));
    const auto bits = f.ir.BitCast<IR::U32>(value);
    const auto negated = f.ir.BitCast<IR::F32>(f.ir.BitwiseXor(bits, f.ir.Imm32(0x80000000U)));
    static_cast<void>(f.ir.FPAdd(negated, f.ir.FPExp2(f.ir.Imm32(3.0f))));
    REQUIRE(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title, Fog, true) == 1U);
}

TEST_CASE("OoT visual workaround leaves other titles, stages and programs alone",
          "[video_core][oot_visual_workaround]") {
    Fixture f;
    f.program.stage = Shader::Stage::Fragment;
    const auto exposure = f.ir.GetFloatCbuf(f.ir.Imm32(3U), f.ir.Imm32(0x38U));
    CHECK_FALSE(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title, Final, false));
    CHECK_FALSE(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title + 1, Final, true));
    CHECK_FALSE(Shader::Optimization::ApplyOotVisualWorkaround(
        f.program, Title, 0x6cb06edf14be1d79ULL, true));
    f.program.stage = Shader::Stage::Compute;
    CHECK_FALSE(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title, Final, true));
    CHECK(Evaluate(exposure) == 2.0f);
}

TEST_CASE("OoT visual workaround edits nothing when the matched shape changes",
          "[video_core][oot_visual_workaround]") {
    Fixture f;
    CHECK(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title, Fog, true) == 0U);
    for (int i = 0; i < 2; ++i) {
        static_cast<void>(f.ir.FPAdd(f.ir.FPNeg(f.ir.FPExp2(f.ir.Imm32(1.0f))),
                                    f.ir.FPExp2(f.ir.Imm32(3.0f))));
    }
    const auto size = f.block.size();
    CHECK(Shader::Optimization::ApplyOotVisualWorkaround(f.program, Title, Fog, true) == 0U);
    CHECK(f.block.size() == size);
}
