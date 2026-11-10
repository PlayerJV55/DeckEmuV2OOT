// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string_view>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "common/alignment.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Backend::SPIRV {
namespace {
// Which of the K=8 and K=16 helpers the program calls.
std::array<bool, 2> MatrixDepths(const IR::Program& program) {
    std::array<bool, 2> needed{};
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::TensorMulAdd || inst.Flags<u32>() == 0) continue;
            const u32 depth = inst.Flags<u32>();
            if (depth != 8 && depth != 16) throw LogicError("Invalid software HMMA depth");
            needed[depth == 16] = true;
        }
    }
    return needed;
}

constexpr u32 Word0(spv::Op opcode, u32 length) {
    return (length << 16) | static_cast<u32>(opcode);
}
} // Anonymous namespace

void DefineSoftwareMatrixFunctions(EmitContext& ctx, const IR::Program& program) {
    const std::array<bool, 2> needed{MatrixDepths(program)};
    for (u32 variant = 0; variant < 2; ++variant) {
        if (!needed[variant]) continue;
        ctx.AddCapability(spv::Capability::GroupNonUniform);
        ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
        if (!ctx.profile.SupportsSubgroupStage(program.stage) ||
            ctx.profile.has_broken_spirv_subgroup_shuffle)
            throw NotImplementedException("Software HMMA requires subgroup shuffle support");
        const Id u32ty = ctx.U32[1], f32ty = ctx.F32[1];
        const auto imm = [&](u32 v) { return ctx.Const(v); };
        const auto add = [&](Id a, Id b) { return ctx.OpIAdd(u32ty, a, b); };
        const auto and_ = [&](Id a, u32 b) { return ctx.OpBitwiseAnd(u32ty, a, imm(b)); };
        const auto unpack = [&](Id v) { return ctx.OpUnpackHalf2x16(ctx.F32[2], v); };
        const auto round_half = [&](Id value) {
            // Do not express the intermediate rounding as unpack(pack(value)): drivers may
            // eliminate that round trip. Round normal significands explicitly, ties to even;
            // half subnormals lie on the exactly representable 2^-24 grid.
            const Id bits = ctx.OpBitcast(u32ty, value);
            const Id magnitude = and_(bits, 0x7fffffffU);
            const Id tie = and_(ctx.OpShiftRightLogical(u32ty, magnitude, imm(13)), 1);
            const Id rounded = and_(add(add(magnitude, imm(0xfff)), tie), ~0x1fffU);
            const Id normal = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, rounded, imm(0x47800000)), imm(0x7f800000), rounded);
            const Id scaled = ctx.OpFMul(f32ty, ctx.OpBitcast(f32ty, magnitude), ctx.Const(16777216.0f));
            const Id subnormal = ctx.OpBitcast(u32ty,
                ctx.OpFMul(f32ty, ctx.OpRoundEven(f32ty, scaled), ctx.Const(0x1p-24f)));
            const Id finite = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x38800000)), normal, subnormal);
            const Id result = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x7f800000)), magnitude, finite);
            return ctx.OpBitcast(f32ty, ctx.OpBitwiseOr(u32ty, result, and_(bits, 0x80000000)));
        };
        const Id type = ctx.TypeFunction(ctx.U32[2], ctx.U32[4], ctx.U32[2], ctx.U32[2]);
        const Id fn = ctx.OpFunction(ctx.U32[2], spv::FunctionControlMask::DontInline, type);
        ctx.matrix_functions[variant] = fn;
        const Id matrix_a = ctx.OpFunctionParameter(ctx.U32[4]);
        const Id matrix_b = ctx.OpFunctionParameter(ctx.U32[2]);
        const Id matrix_c = ctx.OpFunctionParameter(ctx.U32[2]);
        ctx.AddLabel();
        const Id counter = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, u32ty),
                                           spv::StorageClass::Function);
        std::array<Id, 4> accumulators;
        for (auto& acc : accumulators)
            acc = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, f32ty),
                                 spv::StorageClass::Function);
        std::array<Id, 8> inputs;
        for (u32 i = 0; i < 4; ++i)
            inputs[i] = ctx.OpCompositeExtract(u32ty, matrix_a, i);
        for (u32 i = 0; i < 2; ++i) {
            inputs[4 + i] = ctx.OpCompositeExtract(u32ty, matrix_b, i);
            inputs[6 + i] = ctx.OpCompositeExtract(u32ty, matrix_c, i);
        }
        for (u32 row = 0; row < 2; ++row) {
            const Id c = unpack(inputs[6 + row]);
            for (u32 col = 0; col < 2; ++col)
                ctx.OpStore(accumulators[row * 2 + col], ctx.OpCompositeExtract(f32ty, c, col));
        }
        ctx.OpStore(counter, imm(0));
        // Lane (group g, thread t) of an m16n8k16 fragment owns A rows g and g+8 and computes
        // C columns 2t and 2t+1. Its A operands live in its own quad (lanes 4g..4g+3): a quad
        // broadcast with a constant index, which AMD and NVIDIA do as a register swizzle instead
        // of a crossbar permute. Its B operands live in groups 2t and 2t+1: a real shuffle.
        // Each K=8 half is unrolled so every index is a constant and the half rounding at its
        // end needs no branch; only the two halves of K=16 share the loop.
        const bool quad_broadcast = ctx.profile.support_quad_shuffles;
        if (quad_broadcast) {
            ctx.AddCapability(spv::Capability::GroupNonUniformQuad);
        }
        const Id lane = ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id);
        const Id warp_base = and_(lane, ~31U);
        const Id a_base = add(warp_base, and_(lane, 28));
        const Id b_base = add(warp_base, ctx.OpShiftLeftLogical(u32ty, and_(lane, 3), imm(3)));
        const Id header = ctx.OpLabel(), body = ctx.OpLabel(), next = ctx.OpLabel(), end = ctx.OpLabel();
        ctx.OpBranch(header);
        ctx.AddLabel(header);
        const Id half_k = ctx.OpLoad(u32ty, counter);
        const Id more = ctx.OpULessThan(ctx.U1, half_k, imm(variant ? 2 : 1));
        ctx.OpLoopMerge(end, next, spv::LoopControlMask::DontUnroll);
        ctx.OpBranchConditional(more, body, end);
        ctx.AddLabel(body);
        const Id upper = ctx.OpINotEqual(ctx.U1, half_k, imm(0));
        const Id b = ctx.OpSelect(u32ty, upper, inputs[5], inputs[4]);
        std::array<Id, 2> a_rows;
        for (u32 row = 0; row < 2; ++row)
            a_rows[row] = ctx.OpSelect(u32ty, upper, inputs[row + 2], inputs[row]);
        std::array<Id, 4> values;
        for (u32 i = 0; i < 4; ++i)
            values[i] = ctx.OpLoad(f32ty, accumulators[i]);
        for (u32 component = 0; component < 4; ++component) {
            std::array<Id, 2> bs;
            for (u32 col = 0; col < 2; ++col)
                bs[col] = unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), b,
                                                              add(b_base, imm(col * 4 + component))));
            for (u32 row = 0; row < 2; ++row) {
                const Id shared = quad_broadcast
                    ? ctx.OpGroupNonUniformQuadBroadcast(u32ty, imm(3), a_rows[row], imm(component))
                    : ctx.OpGroupNonUniformShuffle(u32ty, imm(3), a_rows[row],
                                                   add(a_base, imm(component)));
                const Id as = unpack(shared);
                for (u32 col = 0; col < 2; ++col) {
                    Id value = values[row * 2 + col];
                    for (u32 half = 0; half < 2; ++half) {
                        value = ctx.OpFma(f32ty, ctx.OpCompositeExtract(f32ty, as, half),
                                          ctx.OpCompositeExtract(f32ty, bs[col], half), value);
                        ctx.Decorate(value, spv::Decoration::NoContraction);
                    }
                    values[row * 2 + col] = value;
                }
            }
        }
        // HMMA.F16 rounds after each K=8 group, including the midpoint of K=16.
        for (u32 i = 0; i < 4; ++i)
            ctx.OpStore(accumulators[i], round_half(values[i]));
        ctx.OpBranch(next);
        ctx.AddLabel(next);
        ctx.OpStore(counter, add(half_k, imm(1)));
        ctx.OpBranch(header);
        ctx.AddLabel(end);
        std::array<Id, 2> packed;
        for (u32 row = 0; row < 2; ++row)
            packed[row] = ctx.OpPackHalf2x16(u32ty, ctx.OpCompositeConstruct(ctx.F32[2],
                ctx.OpLoad(f32ty, accumulators[row * 2]), ctx.OpLoad(f32ty, accumulators[row * 2 + 1])));
        ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], packed[0], packed[1]));
        ctx.OpFunctionEnd();
    }
}

u32 CooperativeMatrixScratchWords(u32 columns) {
    // A (16x16), then B (16xN) and C (16xN), as packed halves.
    return 128 + 16 * columns;
}

bool UsesCooperativeMatrixHmma(const Profile& profile, const IR::Program& program) {
    const u32 columns{profile.cooperative_matrix_hmma_columns};
    if ((columns != 8 && columns != 16) || program.stage != Stage::Compute) {
        return false;
    }
    const std::array<bool, 2> depths{MatrixDepths(program)};
    if (!depths[0] && !depths[1]) {
        return false;
    }
    const auto& size{program.workgroup_size};
    const u64 subgroups{u64{size[0]} * size[1] * size[2] / 32};
    const u64 shared{Common::AlignUp(u64{program.shared_memory_size}, 16)};
    const u64 scratch{subgroups * CooperativeMatrixScratchWords(columns) * 4};
    const char* reason{nullptr};
    if (!profile.SupportsSubgroupStage(Stage::Compute)) {
        reason = "no compute subgroups";
    } else if (program.info.requires_coherent_global_memory ||
               program.info.requires_coherent_image_memory) {
        // Cooperative matrices need the Vulkan memory model, which has no Coherent decoration.
        reason = "coherent memory";
    } else if (size[0] == 0 || size[0] % 32 != 0) {
        // Full subgroups, which the cooperative operations need, require a width multiple of 32.
        reason = "workgroup width not a multiple of 32";
    } else if (shared + scratch > profile.max_compute_shared_memory_size) {
        reason = "no shared memory left for the scratch";
    }
    if (reason) {
        LOG_INFO(Shader_SPIRV, "HMMA keeps the software expansion: {} ({}x{}x{}, {} shared bytes)",
                 reason, size[0], size[1], size[2], program.shared_memory_size);
        return false;
    }
    return true;
}

// The accumulator is F16, as in HMMA.F16. An NVIDIA host can run the guest's own instruction;
// other hosts may round the K=16 sum once instead of after each K=8 group.
void DefineCooperativeMatrixFunctions(EmitContext& ctx, const IR::Program& program) {
    const std::array<bool, 2> depths{MatrixDepths(program)};
    const u32 columns{ctx.profile.cooperative_matrix_hmma_columns};
    ctx.AddExtension("SPV_KHR_cooperative_matrix");
    ctx.AddCapability(spv::Capability::CooperativeMatrixKHR);
    if (ctx.profile.supported_spirv < 0x00010500) {
        ctx.AddExtension("SPV_KHR_vulkan_memory_model");
    }
    ctx.AddCapability(spv::Capability::VulkanMemoryModel);
    ctx.AddCapability(spv::Capability::VulkanMemoryModelDeviceScope);
    ctx.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::Vulkan);
    ctx.AddCapability(spv::Capability::Float16);

    const Id u32ty{ctx.U32[1]};
    const auto imm = [&](u32 value) { return ctx.Const(value); };
    // The placeholder types become instructions that use these, so they are declared first.
    const Id f16{ctx.TypeFloat(16)};
    const Id subgroup{imm(static_cast<u32>(spv::Scope::Subgroup))};
    const Id sixteen{imm(16)};
    const Id width{imm(columns)};
    const Id use_a{imm(static_cast<u32>(spv::CooperativeMatrixUse::MatrixAKHR))};
    const Id use_b{imm(static_cast<u32>(spv::CooperativeMatrixUse::MatrixBKHR))};
    const Id use_c{imm(static_cast<u32>(spv::CooperativeMatrixUse::MatrixAccumulatorKHR))};
    const Id row_major{imm(static_cast<u32>(spv::CooperativeMatrixLayout::RowMajorKHR))};
    const Id column_major{imm(static_cast<u32>(spv::CooperativeMatrixLayout::ColumnMajorKHR))};
    const Id ab_stride{imm(8)};
    const Id c_stride{imm(columns / 2)};
    const Id semantics{imm(static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease |
                                            spv::MemorySemanticsMask::WorkgroupMemory))};
    const u32 non_private{static_cast<u32>(spv::MemoryAccessMask::NonPrivatePointer)};
    const std::array<Sirit::MemoryAccessLiteral, 1> non_private_access{
        spv::MemoryAccessMask::NonPrivatePointer};
    auto& rewrites{ctx.cooperative_matrix_rewrites};
    const auto matrix_type = [&](std::string_view name, Id matrix_columns, Id use) {
        const Id type{ctx.TypeOpaque(name)};
        rewrites.push_back({type.value,
                            {Word0(spv::Op::OpTypeCooperativeMatrixKHR, 7), type.value, f16.value,
                             subgroup.value, sixteen.value, matrix_columns.value, use.value}});
        return type;
    };
    const Id a_type{matrix_type("hmma_a", sixteen, use_a)};
    const Id b_type{matrix_type("hmma_b", width, use_b)};
    const Id c_type{matrix_type("hmma_c", width, use_c)};

    const u32 words{CooperativeMatrixScratchWords(columns)};
    const Id function_type{ctx.TypeFunction(ctx.U32[2], ctx.U32[4], ctx.U32[2], ctx.U32[2])};
    for (u32 variant = 0; variant < 2; ++variant) {
        if (!depths[variant]) {
            continue;
        }
        const bool short_depth{variant == 0};
        const Id fn{ctx.OpFunction(ctx.U32[2], spv::FunctionControlMask::DontInline, function_type)};
        ctx.matrix_functions[variant] = fn;
        const Id matrix_a{ctx.OpFunctionParameter(ctx.U32[4])};
        const Id matrix_b{ctx.OpFunctionParameter(ctx.U32[2])};
        const Id matrix_c{ctx.OpFunctionParameter(ctx.U32[2])};
        ctx.AddLabel();
        const auto add = [&](Id lhs, u32 rhs) { return ctx.OpIAdd(u32ty, lhs, imm(rhs)); };
        const auto pointer = [&](Id index) {
            return ctx.cooperative_matrix_scratch_block
                       ? ctx.OpAccessChain(ctx.cooperative_matrix_scratch_u32,
                                           ctx.cooperative_matrix_scratch, imm(1), index)
                       : ctx.OpAccessChain(ctx.cooperative_matrix_scratch_u32,
                                           ctx.cooperative_matrix_scratch, index);
        };
        const auto store = [&](Id index, Id value) {
            ctx.OpStore(pointer(index), value, non_private_access);
        };
        const auto fragment = [&](Id matrix, u32 element) {
            return ctx.OpCompositeExtract(u32ty, matrix, element);
        };
        const auto barrier = [&] { ctx.OpControlBarrier(subgroup, subgroup, semantics); };
        const auto load = [&](Id type, Id index, Id layout, Id stride) {
            const Id address{pointer(index)};
            const Id result{ctx.OpUndef(type)};
            rewrites.push_back({result.value,
                                {Word0(spv::Op::OpCooperativeMatrixLoadKHR, 7), type.value,
                                 result.value, address.value, layout.value, stride.value,
                                 non_private}});
            return result;
        };

        // Lane 4 * group + thread holds A rows group and group + 8, B column group and C rows
        // group and group + 8, two halves a register, adjacent in the layouts below: A row-major
        // and B column-major with 8 words a row or column, C row-major with N/2 words a row.
        const Id lane{ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id)};
        const Id group{ctx.OpShiftRightLogical(u32ty, lane, imm(2))};
        const Id thread{ctx.OpBitwiseAnd(u32ty, lane, imm(3))};
        const Id a_base{ctx.OpIMul(u32ty, ctx.OpLoad(u32ty, ctx.subgroup_id), imm(words))};
        const Id b_base{add(a_base, 128)};
        const Id c_base{add(a_base, 128 + 8 * columns)};
        const Id element{ctx.OpIAdd(u32ty, ctx.OpShiftLeftLogical(u32ty, group, imm(3)), thread)};
        const Id c_element{columns == 16 ? element
                                         : ctx.OpIAdd(u32ty, ctx.OpShiftLeftLogical(u32ty, group,
                                                                                    imm(2)),
                                                      thread)};
        const Id a_index{ctx.OpIAdd(u32ty, a_base, element)};
        const Id b_index{ctx.OpIAdd(u32ty, b_base, element)};
        const Id c_index{ctx.OpIAdd(u32ty, c_base, c_element)};
        const Id zero{ctx.u32_zero_value};

        // Other lanes may still be reading the previous result from the slice.
        barrier();
        store(a_index, fragment(matrix_a, 0));
        store(add(a_index, 64), fragment(matrix_a, 1));
        store(add(a_index, 4), short_depth ? zero : fragment(matrix_a, 2));
        store(add(a_index, 68), short_depth ? zero : fragment(matrix_a, 3));
        store(b_index, fragment(matrix_b, 0));
        store(add(b_index, 4), short_depth ? zero : fragment(matrix_b, 1));
        store(c_index, fragment(matrix_c, 0));
        store(add(c_index, 4 * columns), fragment(matrix_c, 1));
        if (columns == 16) {
            // Columns 8 to 15 of B and C pad the 16x8 operands to the host's shape.
            store(add(b_index, 64), zero);
            store(add(b_index, 68), zero);
            store(add(c_index, 4), zero);
            store(add(c_index, 68), zero);
        }
        barrier();
        const Id a{load(a_type, a_base, row_major, ab_stride)};
        const Id b{load(b_type, b_base, column_major, ab_stride)};
        const Id c{load(c_type, c_base, row_major, c_stride)};
        const Id d{ctx.OpUndef(c_type)};
        rewrites.push_back({d.value,
                            {Word0(spv::Op::OpCooperativeMatrixMulAddKHR, 6), c_type.value, d.value,
                             a.value, b.value, c.value}});
        const Id result_address{pointer(c_base)};
        const Id store_marker{ctx.OpUndef(u32ty)};
        rewrites.push_back({store_marker.value,
                            {Word0(spv::Op::OpCooperativeMatrixStoreKHR, 6), result_address.value,
                             d.value, row_major.value, c_stride.value, non_private}});
        barrier();
        const Id d0{ctx.OpLoad(u32ty, pointer(c_index), non_private_access)};
        const Id d1{ctx.OpLoad(u32ty, pointer(add(c_index, 4 * columns)), non_private_access)};
        ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], d0, d1));
        ctx.OpFunctionEnd();
    }
}

void PatchCooperativeMatrix(std::vector<u32>& code, const EmitContext& ctx) {
    const auto& rewrites{ctx.cooperative_matrix_rewrites};
    if (rewrites.empty()) {
        return;
    }
    std::vector<u32> patched(code.begin(), code.begin() + 5);
    patched.reserve(code.size() + rewrites.size() * 4);
    size_t replaced{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 length{code[offset] >> 16};
        if (length == 0 || offset + length > code.size()) {
            throw LogicError("Malformed SPIR-V while placing cooperative matrix instructions");
        }
        const auto opcode{static_cast<spv::Op>(code[offset] & 0xffffU)};
        const u32 result{opcode == spv::Op::OpTypeOpaque ? code[offset + 1]
                         : opcode == spv::Op::OpUndef    ? code[offset + 2]
                                                         : 0U};
        const auto it{result == 0 ? rewrites.end()
                                  : std::ranges::find(rewrites, result,
                                                      &std::pair<u32, std::vector<u32>>::first)};
        if (it != rewrites.end()) {
            patched.insert(patched.end(), it->second.begin(), it->second.end());
            ++replaced;
        } else {
            patched.insert(patched.end(), code.begin() + offset, code.begin() + offset + length);
        }
        offset += length;
    }
    if (replaced != rewrites.size()) {
        throw LogicError("Cooperative matrix placeholders not found");
    }
    code = std::move(patched);
}

Id EmitTensorMulAdd(EmitContext& ctx, IR::Inst* inst, Id a, Id b, Id c) {
    const u32 depth = inst->Flags<u32>();
    if (depth != 8 && depth != 16) {
        throw NotImplementedException("Unsupported matrix multiply depth");
    }
    return ctx.OpFunctionCall(ctx.U32[2], ctx.matrix_functions[depth == 16], a, b, c);
}

} // namespace Shader::Backend::SPIRV
