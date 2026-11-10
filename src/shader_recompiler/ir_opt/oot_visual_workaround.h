// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {

struct OotVisualScales {
    float fog = 0.25f;
    float exposure = 0.65f;
    /// 1 leaves the bloom constant alone.
    float bloom_exposure = 1.0f;
};

// TEMPORARY visual workaround for Ocarina of Time, not an emulator fix. Outdoor areas come out
// far hazier than on console and the root cause is still open. Scalar edits, tuned against a
// console reference:
//  - fog integration (compute 780c0536b89fc89e): the slice thickness, the one FPAdd of
//    -exp2(a) + exp2(b), is multiplied by scales.fog before it scales extinction and scattering;
//  - log-bloom final (fragment bded5ce917183214): the exposure constant, cbuf 3 offset 0x38,
//    is multiplied by scales.exposure before the filmic curve, and the bloom constant next to
//    it, cbuf 3 offset 0x34, by scales.bloom_exposure when that is not 1.
// The hashes are of the lowered programs, so a lowering change to either program turns the
// workaround off for it. Returns the number of edits for a matching shader (0 when its shape
// changed and nothing was edited), nullopt for any other shader.
inline std::optional<size_t> ApplyOotVisualWorkaround(IR::Program& program, u64 title_id,
                                                     u64 shader_hash, bool enabled,
                                                     const OotVisualScales& scales = {}) {
    if (!enabled || title_id != 0x0400486024DEC000ULL) {
        return std::nullopt;
    }
    const bool fog = shader_hash == 0x780c0536b89fc89eULL && program.stage == Stage::Compute;
    const bool exposure = shader_hash == 0xbded5ce917183214ULL && program.stage == Stage::Fragment;
    if (!fog && !exposure) {
        return std::nullopt;
    }
    const auto has_opcode = [](const IR::Value& value, IR::Opcode opcode) {
        return !value.IsImmediate() && value.InstRecursive()->GetOpcode() == opcode;
    };
    const auto negative_exp = [&](const IR::Value& value) {
        if (has_opcode(value, IR::Opcode::FPNeg32)) {
            return has_opcode(value.InstRecursive()->Arg(0), IR::Opcode::FPExp2);
        }
        if (!has_opcode(value, IR::Opcode::BitCastF32U32)) {
            return false;
        }
        const auto bits = value.InstRecursive()->Arg(0);
        if (!has_opcode(bits, IR::Opcode::BitwiseXor32)) {
            return false;
        }
        const auto* flip = bits.InstRecursive();
        const auto source = flip->Arg(0);
        return flip->Arg(1).IsImmediate() && flip->Arg(1).U32() == 0x80000000U &&
               has_opcode(source, IR::Opcode::BitCastU32F32) &&
               has_opcode(source.InstRecursive()->Arg(0), IR::Opcode::FPExp2);
    };
    struct Match {
        IR::Block* block;
        IR::Inst* inst;
        float scale;
    };
    std::vector<Match> matches;
    size_t exposure_loads = 0;
    const auto is_cbuf3_load = [](const IR::Inst& inst, u32 offset) {
        return inst.GetOpcode() == IR::Opcode::GetCbufF32 && inst.Arg(0).IsImmediate() &&
               inst.Arg(0).U32() == 3 && inst.Arg(1).IsImmediate() && inst.Arg(1).U32() == offset;
    };
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (fog && inst.GetOpcode() == IR::Opcode::FPAdd32 &&
                ((negative_exp(inst.Arg(0)) && has_opcode(inst.Arg(1), IR::Opcode::FPExp2)) ||
                 (negative_exp(inst.Arg(1)) && has_opcode(inst.Arg(0), IR::Opcode::FPExp2)))) {
                matches.push_back({block, &inst, scales.fog});
            }
            if (exposure && is_cbuf3_load(inst, 0x38)) {
                matches.push_back({block, &inst, scales.exposure});
                ++exposure_loads;
            }
            if (exposure && scales.bloom_exposure != 1.0f && is_cbuf3_load(inst, 0x34)) {
                matches.push_back({block, &inst, scales.bloom_exposure});
            }
        }
    }
    if ((fog && matches.size() != 1) || (exposure && (exposure_loads == 0 || exposure_loads > 3))) {
        return 0;
    }
    for (const auto& [block, inst, scale] : matches) {
        const auto insertion = block->Instructions().iterator_to(*inst);
        const auto original = block->PrependNewInst(insertion, *inst);
        IR::IREmitter ir{*block, insertion};
        const auto scaled = ir.FPMul(IR::F32{IR::Value{&*original}}, ir.Imm32(scale));
        inst->ReplaceUsesWith(scaled);
    }
    return matches.size();
}

} // namespace Shader::Optimization
