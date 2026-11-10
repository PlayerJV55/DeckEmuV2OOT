// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"

namespace {
using namespace Shader;

constexpr u32 OpCompositeExtract = 81;
constexpr u32 OpImageGather = 96;
constexpr u32 OpImageDrefGather = 97;
constexpr u32 OpImageSparseGather = 314;
constexpr u32 OpImageSparseDrefGather = 315;
constexpr u32 ImageOperandsOffset = 0x10;
constexpr u32 ImageOperandsConstOffsets = 0x20;

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, u32 opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16;
        REQUIRE(count > 0);
        REQUIRE(i + count <= code.size());
        if ((code[i] & 0xffff) == opcode) {
            result.emplace_back(code.begin() + i + 1, code.begin() + i + count);
        }
        i += count;
    }
    return result;
}

// A per-tap gather whose packed offsets are immediates or come from a run-time value.
std::vector<u32> EmitPtpGather(bool dynamic, bool dref, bool sparse) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {32, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    auto& info = program.info;
    info.texture_descriptors.push_back(
        {.type = TextureType::Color2D, .is_depth = dref, .count = 1});
    info.uses_local_invocation_id = dynamic;
    info.uses_sparse_residency = sparse;

    IR::IREmitter ir{block};
    const auto offsets = [&](u32 base) -> IR::Value {
        if (!dynamic) {
            return ir.CompositeConstruct(ir.Imm32(base), ir.Imm32(base + 1), ir.Imm32(base + 2),
                                         ir.Imm32(base + 3));
        }
        const IR::U32 value{ir.LocalInvocationIdX()};
        const auto field = [&](u32 bit) {
            return ir.BitFieldExtract(value, ir.Imm32(bit), ir.Imm32(6U), true);
        };
        return ir.CompositeConstruct(field(0), field(8), field(16), field(24));
    };
    const IR::Value coords{ir.CompositeConstruct(ir.Imm32(0.5f), ir.Imm32(0.5f))};
    const IR::Value offset{offsets(1)};
    const IR::Value offset2{offsets(5)};
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Color2D);
    flags.is_depth.Assign(dref ? 1 : 0);
    flags.gather_component.Assign(1);
    const auto gather{
        dref ? block.PrependNewInst(block.end(), IR::Opcode::ImageGatherDref,
                                    {ir.Imm32(0U), coords, offset, offset2, ir.Imm32(0.25f)},
                                    flags.raw)
             : block.PrependNewInst(block.end(), IR::Opcode::ImageGather,
                                    {ir.Imm32(0U), coords, offset, offset2}, flags.raw)};
    if (sparse) {
        static_cast<void>(ir.GetSparseFromOp(IR::Value{&*gather}));
    }
    Profile profile{};
    profile.supported_spirv = 0x00010300;
    return Backend::SPIRV::EmitSPIRV(profile, program);
}
} // Anonymous namespace

TEST_CASE("SPIR-V per-tap gathers with run-time offsets gather once per tap",
          "[video_core][spirv][gather]") {
    for (const bool dref : {false, true}) {
        CAPTURE(dref);
        const auto code = EmitPtpGather(true, dref, false);
        const auto gathers = Instructions(code, dref ? OpImageDrefGather : OpImageGather);
        REQUIRE(gathers.size() == 4);
        const auto extracts = Instructions(code, OpCompositeExtract);
        for (const auto& gather : gathers) {
            REQUIRE(gather.size() == 7);
            CHECK(gather[5] == ImageOperandsOffset);
            // Each tap keeps the i0j0 texel, the fourth component, of its own gather.
            CHECK(std::ranges::any_of(extracts, [&](const auto& extract) {
                return extract.size() == 4 && extract[2] == gather[1] && extract[3] == 3;
            }));
        }
    }
}

TEST_CASE("SPIR-V per-tap gathers keep a single gather for immediate or sparse offsets",
          "[video_core][spirv][gather]") {
    for (const bool dref : {false, true}) {
        CAPTURE(dref);
        const auto immediate = EmitPtpGather(false, dref, false);
        const auto gathers = Instructions(immediate, dref ? OpImageDrefGather : OpImageGather);
        REQUIRE(gathers.size() == 1);
        REQUIRE(gathers[0].size() == 7);
        CHECK(gathers[0][5] == ImageOperandsConstOffsets);

        const auto sparse = EmitPtpGather(true, dref, true);
        CHECK(Instructions(sparse, dref ? OpImageDrefGather : OpImageGather).empty());
        CHECK(Instructions(sparse, dref ? OpImageSparseDrefGather : OpImageSparseGather).size() ==
              1);
    }
}
