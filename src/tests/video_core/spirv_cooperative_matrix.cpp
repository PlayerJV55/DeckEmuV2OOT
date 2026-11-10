// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

constexpr u32 OpName = 5;
constexpr u32 OpMemoryModel = 14;
constexpr u32 OpCapability = 17;
constexpr u32 OpTypeOpaque = 31;
constexpr u32 OpConstant = 43;
constexpr u32 OpFunction = 54;
constexpr u32 OpFunctionParameter = 55;
constexpr u32 OpFunctionEnd = 56;
constexpr u32 OpFunctionCall = 57;
constexpr u32 OpLoad = 61;
constexpr u32 OpStore = 62;
constexpr u32 OpAccessChain = 65;
constexpr u32 OpDecorate = 71;
constexpr u32 OpMemberDecorate = 72;
constexpr u32 OpCompositeExtract = 81;
constexpr u32 OpIAdd = 128;
constexpr u32 OpIMul = 132;
constexpr u32 OpShiftRightLogical = 194;
constexpr u32 OpShiftLeftLogical = 196;
constexpr u32 OpBitwiseAnd = 199;
constexpr u32 OpControlBarrier = 224;
constexpr u32 OpLoopMerge = 246;
constexpr u32 OpGroupNonUniformShuffle = 345;
constexpr u32 OpTypeCooperativeMatrixKHR = 4456;
constexpr u32 OpCooperativeMatrixLoadKHR = 4457;
constexpr u32 OpCooperativeMatrixStoreKHR = 4458;
constexpr u32 OpCooperativeMatrixMulAddKHR = 4459;

constexpr u32 CapabilityCooperativeMatrixKHR = 6022;
constexpr u32 CapabilityVulkanMemoryModel = 5345;
constexpr u32 BuiltInSubgroupId = 40;
constexpr u32 BuiltInSubgroupLocalInvocationId = 41;

struct CooperativeProgram {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile;

    explicit CooperativeProgram(u32 depth, u32 columns, u32 count = 1) {
        program.stage = Stage::Compute;
        program.workgroup_size = {64, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        // What the info pass sets for TensorMulAdd.
        program.info.uses_subgroup_shuffles = true;
        program.info.uses_subgroup_invocation_id = true;
        program.info.uses_fp32_denorms_preserve = true;
        profile.supported_spirv = 0x00010600;
        profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Compute);
        profile.max_subgroup_size = 32;
        profile.support_float_controls = true;
        profile.support_fp32_denorm_preserve = true;
        profile.cooperative_matrix_hmma_columns = columns;
        profile.max_compute_shared_memory_size = 48 * 1024;

        IR::IREmitter ir{block};
        const auto lane = ir.LaneId();
        std::array<IR::U32, 8> inputs;
        for (u32 i = 0; i < inputs.size(); ++i) {
            inputs[i] = ir.IAdd(ir.Imm32(0x3c003c00U + i * 0x01000100U),
                                ir.ShiftLeftLogical(lane, ir.Imm32(3)));
        }
        for (u32 i = 0; i < count; ++i) {
            const auto result = ir.TensorMulAdd(
                ir.CompositeConstruct(inputs[0], inputs[1], inputs[2], inputs[3]),
                ir.CompositeConstruct(inputs[4], inputs[5]),
                ir.CompositeConstruct(inputs[6], inputs[7]), depth);
            inputs[6] = IR::U32{ir.CompositeExtract(result, 0)};
            inputs[7] = IR::U32{ir.CompositeExtract(result, 1)};
        }
        program.info.used_storage_buffer_types = IR::Type::U32;
        program.info.storage_buffers_descriptors = {{0, 0, 1, true}};
        program.info.uses_local_invocation_id = true;
        const auto offset = ir.ShiftLeftLogical(ir.LocalInvocationIdX(), ir.Imm32(3));
        block.AppendNewInst(IR::Opcode::WriteStorage32, {ir.Imm32(0), offset, inputs[6]});
        block.AppendNewInst(IR::Opcode::WriteStorage32,
                            {ir.Imm32(0), ir.IAdd(offset, ir.Imm32(4)), inputs[7]});
        block.AppendNewInst(IR::Opcode::Barrier, {});
        block.AppendNewInst(IR::Opcode::DeviceMemoryBarrier, {});
    }

    std::vector<u32> Emit(std::string_view name = {}) {
        auto code = Backend::SPIRV::EmitSPIRV(profile, program);
        if (const char* directory = std::getenv("EDEN_TENSOR_TEST_OUTPUT");
            directory && !name.empty()) {
            std::filesystem::create_directories(directory);
            std::ofstream file{std::filesystem::path{directory} / (std::string{name} + ".spv"),
                               std::ios::binary};
            file.write(reinterpret_cast<const char*>(code.data()),
                       static_cast<std::streamsize>(code.size() * sizeof(u32)));
            REQUIRE(file.good());
        }
        return code;
    }
};

struct Instruction {
    u32 opcode;
    std::vector<u32> words; ///< operands, without the opcode word
};

std::vector<Instruction> Parse(const std::vector<u32>& code) {
    std::vector<Instruction> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        result.push_back({code[offset] & 0xffffU, {code.begin() + offset + 1,
                                                   code.begin() + offset + size}});
        offset += size;
    }
    return result;
}

std::vector<std::vector<u32>> Find(const std::vector<Instruction>& code, u32 opcode) {
    std::vector<std::vector<u32>> result;
    for (const auto& inst : code) {
        if (inst.opcode == opcode) {
            result.push_back(inst.words);
        }
    }
    return result;
}

bool HasCapability(const std::vector<Instruction>& code, u32 capability) {
    return std::ranges::any_of(Find(code, OpCapability),
                               [=](const auto& words) { return words[0] == capability; });
}

std::map<u32, u32> Constants(const std::vector<Instruction>& code) {
    std::map<u32, u32> constants;
    for (const auto& words : Find(code, OpConstant)) {
        constants.emplace(words[1], words[2]);
    }
    return constants;
}

u32 BuiltinVariable(const std::vector<Instruction>& code, u32 builtin) {
    for (const auto& words : Find(code, OpDecorate)) {
        if (words[1] == 11 && words[2] == builtin) { // BuiltIn
            return words[0];
        }
    }
    FAIL("Missing builtin " << builtin);
    return 0;
}

// Runs the straight-line matrix helper for one lane of subgroup 0 and records where each
// fragment register is stored and which scratch words the result is read from.
struct LaneTrace {
    std::map<u32, u64> stores; ///< scratch word -> stored value (fragment token or constant)
    std::array<u32, 2> reads{};
};

constexpr u64 Token(u32 parameter, u32 element) {
    return (u64{1} << 32) | (parameter << 4) | element;
}

LaneTrace TraceLane(const std::vector<Instruction>& code, u32 function, u32 lane) {
    const auto constants = Constants(code);
    const u32 lane_variable = BuiltinVariable(code, BuiltInSubgroupLocalInvocationId);
    const u32 subgroup_variable = BuiltinVariable(code, BuiltInSubgroupId);
    std::map<u32, u64> values;
    std::map<u32, u32> parameters;
    size_t reads = 0;
    LaneTrace trace;
    bool inside = false;
    const auto value = [&](u32 id) -> u64 {
        if (const auto it = constants.find(id); it != constants.end()) {
            return it->second;
        }
        REQUIRE(values.contains(id));
        return values.at(id);
    };
    for (const auto& inst : code) {
        const auto& w = inst.words;
        if (inst.opcode == OpFunction) {
            inside = w[1] == function;
            continue;
        }
        if (!inside) {
            continue;
        }
        switch (inst.opcode) {
        case OpFunctionParameter:
            parameters.emplace(w[1], static_cast<u32>(parameters.size()));
            break;
        case OpLoad:
            if (w[2] == lane_variable) {
                values[w[1]] = lane;
            } else if (w[2] == subgroup_variable) {
                values[w[1]] = 0;
            } else {
                REQUIRE(reads < trace.reads.size());
                trace.reads[reads++] = static_cast<u32>(value(w[2]));
            }
            break;
        case OpCompositeExtract:
            REQUIRE(parameters.contains(w[2]));
            values[w[1]] = Token(parameters.at(w[2]), w[3]);
            break;
        case OpIAdd:
            values[w[1]] = (value(w[2]) + value(w[3])) & 0xffffffffU;
            break;
        case OpIMul:
            values[w[1]] = (value(w[2]) * value(w[3])) & 0xffffffffU;
            break;
        case OpShiftRightLogical:
            values[w[1]] = value(w[2]) >> value(w[3]);
            break;
        case OpShiftLeftLogical:
            values[w[1]] = (value(w[2]) << value(w[3])) & 0xffffffffU;
            break;
        case OpBitwiseAnd:
            values[w[1]] = value(w[2]) & value(w[3]);
            break;
        case OpAccessChain:
            // The last index is the scratch word.
            values[w[1]] = value(w.back());
            break;
        case OpStore:
            REQUIRE(!trace.stores.contains(static_cast<u32>(value(w[0]))));
            trace.stores[static_cast<u32>(value(w[0]))] = value(w[1]);
            break;
        case OpFunctionEnd:
            return trace;
        default:
            break;
        }
    }
    FAIL("Matrix helper not found");
    return trace;
}

u32 HelperFunction(const std::vector<Instruction>& code) {
    const auto calls = Find(code, OpFunctionCall);
    REQUIRE(!calls.empty());
    return calls.front()[2];
}

} // Anonymous namespace

TEST_CASE("Cooperative HMMA stays off without a supported host shape",
          "[video_core][tensor][ampere][cooperative_matrix]") {
    CooperativeProgram fixture{16, 0};
    const auto code = Parse(fixture.Emit());
    REQUIRE_FALSE(fixture.program.info.uses_cooperative_matrix);
    REQUIRE_FALSE(HasCapability(code, CapabilityCooperativeMatrixKHR));
    REQUIRE(Find(code, OpLoopMerge).size() == 1);
    REQUIRE_FALSE(Find(code, OpGroupNonUniformShuffle).empty());
}

TEST_CASE("Cooperative HMMA emits one load, multiply and store per helper",
          "[video_core][tensor][ampere][cooperative_matrix]") {
    for (const u32 columns : {8U, 16U}) {
        for (const u32 depth : {8U, 16U}) {
            CooperativeProgram fixture{depth, columns, 4};
            const auto code = Parse(fixture.Emit(
                "coopmat-k" + std::to_string(depth) + "-n" + std::to_string(columns)));
            REQUIRE(fixture.program.info.uses_cooperative_matrix);
            REQUIRE(HasCapability(code, CapabilityCooperativeMatrixKHR));
            REQUIRE(HasCapability(code, CapabilityVulkanMemoryModel));
            const auto models = Find(code, OpMemoryModel);
            REQUIRE(models.size() == 1);
            REQUIRE(models[0][1] == 3); // Vulkan
            REQUIRE(Find(code, OpTypeOpaque).empty());
            REQUIRE(Find(code, OpGroupNonUniformShuffle).empty());
            REQUIRE(Find(code, OpLoopMerge).empty());
            REQUIRE(Find(code, OpFunctionCall).size() == 4);
            REQUIRE(Find(code, OpCooperativeMatrixLoadKHR).size() == 3);
            REQUIRE(Find(code, OpCooperativeMatrixMulAddKHR).size() == 1);
            REQUIRE(Find(code, OpCooperativeMatrixStoreKHR).size() == 1);

            const auto constants = Constants(code);
            std::map<u32, std::vector<u32>> types;
            for (const auto& words : Find(code, OpTypeCooperativeMatrixKHR)) {
                REQUIRE(constants.at(words[2]) == 3); // Subgroup
                types[words[0]] = {constants.at(words[3]), constants.at(words[4]),
                                   constants.at(words[5])};
            }
            REQUIRE(types.size() == 3);
            // Loaded value -> type, layout (row-major 0, column-major 1) and stride in words.
            std::map<u32, std::array<u32, 3>> loads;
            for (const auto& words : Find(code, OpCooperativeMatrixLoadKHR)) {
                REQUIRE(words.size() == 6);
                REQUIRE(words[5] == 0x20); // NonPrivatePointer
                loads[words[1]] = {words[0], constants.at(words[3]), constants.at(words[4])};
            }
            const auto multiplies = Find(code, OpCooperativeMatrixMulAddKHR);
            const auto& mul = multiplies[0];
            REQUIRE(types.at(loads.at(mul[2])[0]) == std::vector<u32>{16, 16, 0}); // A, 16x16
            REQUIRE(types.at(loads.at(mul[3])[0]) == std::vector<u32>{16, columns, 1}); // B
            REQUIRE(types.at(loads.at(mul[4])[0]) == std::vector<u32>{16, columns, 2}); // C
            REQUIRE(mul[0] == loads.at(mul[4])[0]);
            REQUIRE(loads.at(mul[2])[1] == 0);
            REQUIRE(loads.at(mul[2])[2] == 8);
            REQUIRE(loads.at(mul[3])[1] == 1);
            REQUIRE(loads.at(mul[3])[2] == 8);
            REQUIRE(loads.at(mul[4])[1] == 0);
            REQUIRE(loads.at(mul[4])[2] == columns / 2);
            const auto stores = Find(code, OpCooperativeMatrixStoreKHR);
            REQUIRE(stores[0][1] == mul[1]);
            REQUIRE(constants.at(stores[0][2]) == 0);
            REQUIRE(constants.at(stores[0][3]) == columns / 2);

            // Before staging, after staging and after the store, each at subgroup scope.
            const auto barriers = Find(code, OpControlBarrier);
            const auto subgroup_barriers = std::ranges::count_if(barriers, [&](const auto& w) {
                return constants.at(w[0]) == 3 && constants.at(w[1]) == 3;
            });
            REQUIRE(subgroup_barriers == 3);
        }
    }
}

TEST_CASE("Cooperative HMMA declares the Vulkan memory model extension before SPIR-V 1.5",
          "[video_core][tensor][ampere][cooperative_matrix]") {
    CooperativeProgram fixture{16, 16};
    fixture.profile.supported_spirv = 0x00010300;
    const auto code = Parse(fixture.Emit("coopmat-spirv13"));
    REQUIRE(fixture.program.info.uses_cooperative_matrix);
    std::vector<std::string> extensions;
    for (const auto& inst : code) {
        if (inst.opcode == 10) { // OpExtension
            extensions.emplace_back(reinterpret_cast<const char*>(inst.words.data()));
        }
    }
    REQUIRE(std::ranges::find(extensions, "SPV_KHR_vulkan_memory_model") != extensions.end());
    REQUIRE(std::ranges::find(extensions, "SPV_KHR_cooperative_matrix") != extensions.end());
}

TEST_CASE("Cooperative HMMA stages the Ampere fragments in the host matrix layouts",
          "[video_core][tensor][ampere][cooperative_matrix]") {
    for (const u32 columns : {8U, 16U}) {
        for (const u32 depth : {8U, 16U}) {
            CooperativeProgram fixture{depth, columns};
            const auto code = Parse(fixture.Emit());
            const u32 function = HelperFunction(code);
            const u32 a_base = 0;
            const u32 b_base = 128;
            const u32 c_base = 128 + 8 * columns;
            std::map<u32, u64> scratch;
            for (u32 lane = 0; lane < 32; ++lane) {
                const LaneTrace trace = TraceLane(code, function, lane);
                for (const auto& [word, stored] : trace.stores) {
                    REQUIRE(scratch.emplace(word, stored).second); // each word written once
                }
                const u32 group = lane / 4;
                const u32 thread = lane % 4;
                // A (m16n8k16 fragment): a0 = A[g][2t..], a1 = A[g+8][2t..], a2 = A[g][8+2t..],
                // a3 = A[g+8][8+2t..]; row-major, two halves a word.
                const auto a_word = [&](u32 row, u32 col) { return a_base + row * 8 + col / 2; };
                REQUIRE(trace.stores.at(a_word(group, 2 * thread)) == Token(0, 0));
                REQUIRE(trace.stores.at(a_word(group + 8, 2 * thread)) == Token(0, 1));
                REQUIRE(trace.stores.at(a_word(group, 8 + 2 * thread)) ==
                        (depth == 8 ? 0 : Token(0, 2)));
                REQUIRE(trace.stores.at(a_word(group + 8, 8 + 2 * thread)) ==
                        (depth == 8 ? 0 : Token(0, 3)));
                // B: b0 = B[2t..][g], b1 = B[8+2t..][g]; column-major.
                const auto b_word = [&](u32 row, u32 col) { return b_base + col * 8 + row / 2; };
                REQUIRE(trace.stores.at(b_word(2 * thread, group)) == Token(1, 0));
                REQUIRE(trace.stores.at(b_word(8 + 2 * thread, group)) ==
                        (depth == 8 ? 0 : Token(1, 1)));
                // C and D: c0 = C[g][2t..], c1 = C[g+8][2t..]; row-major, N/2 words a row.
                const auto c_word = [&](u32 row, u32 col) {
                    return c_base + row * (columns / 2) + col / 2;
                };
                REQUIRE(trace.stores.at(c_word(group, 2 * thread)) == Token(2, 0));
                REQUIRE(trace.stores.at(c_word(group + 8, 2 * thread)) == Token(2, 1));
                REQUIRE(trace.reads ==
                        std::array{c_word(group, 2 * thread), c_word(group + 8, 2 * thread)});
                if (columns == 16) {
                    REQUIRE(trace.stores.at(b_word(2 * thread, group + 8)) == 0);
                    REQUIRE(trace.stores.at(b_word(8 + 2 * thread, group + 8)) == 0);
                    REQUIRE(trace.stores.at(c_word(group, 8 + 2 * thread)) == 0);
                    REQUIRE(trace.stores.at(c_word(group + 8, 8 + 2 * thread)) == 0);
                }
            }
            // The subgroup fills its whole slice, padding included.
            REQUIRE(scratch.size() == 128 + 16 * columns);
            REQUIRE(scratch.begin()->first == 0);
            REQUIRE(scratch.rbegin()->first == 128 + 16 * columns - 1);
        }
    }
}

TEST_CASE("Cooperative HMMA falls back to the software helpers when it cannot run",
          "[video_core][tensor][ampere][cooperative_matrix]") {
    const auto falls_back = [](CooperativeProgram& fixture) {
        const auto code = Parse(fixture.Emit());
        REQUIRE_FALSE(fixture.program.info.uses_cooperative_matrix);
        REQUIRE_FALSE(HasCapability(code, CapabilityCooperativeMatrixKHR));
        REQUIRE(Find(code, OpLoopMerge).size() == 1);
    };
    {
        CooperativeProgram fixture{16, 16};
        fixture.program.workgroup_size = {48, 1, 1}; // no full subgroups
        falls_back(fixture);
    }
    {
        CooperativeProgram fixture{16, 16};
        fixture.program.shared_memory_size = 47 * 1024; // no room for the scratch
        falls_back(fixture);
    }
    {
        CooperativeProgram fixture{16, 8};
        fixture.program.info.requires_coherent_global_memory = true;
        falls_back(fixture);
    }
    {
        CooperativeProgram fixture{16, 12}; // not a shape the backend models
        falls_back(fixture);
    }
}

TEST_CASE("Cooperative HMMA scratch follows the guest's explicit workgroup layout",
          "[video_core][tensor][ampere][cooperative_matrix]") {
    CooperativeProgram fixture{16, 8};
    fixture.profile.support_explicit_workgroup_layout = true;
    fixture.program.shared_memory_size = 1000;
    const auto code = Parse(fixture.Emit("coopmat-explicit-layout"));
    REQUIRE(fixture.program.info.uses_cooperative_matrix);
    u32 scratch = 0;
    for (const auto& inst : code) {
        if (inst.opcode == OpName && inst.words.size() > 1) {
            const auto* text = reinterpret_cast<const char*>(inst.words.data() + 1);
            if (std::string{text} == "hmma_scratch") {
                scratch = inst.words[0];
            }
        }
    }
    REQUIRE(scratch != 0);
    // The scratch's Block puts it past the guest's 1000 bytes, rounded to 16.
    const auto decorations = Find(code, OpMemberDecorate);
    REQUIRE(std::ranges::any_of(decorations, [](const auto& w) {
        return w[1] == 1 && w[2] == 35 && w[3] == 1008; // Offset
    }));
    REQUIRE(std::ranges::any_of(Find(code, OpDecorate), [=](const auto& w) {
        return w[0] == scratch && w[1] == 20; // Aliased
    }));
}
